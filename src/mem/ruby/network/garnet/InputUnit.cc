/*
 * Copyright (c) 2020 Inria
 * Copyright (c) 2016 Georgia Institute of Technology
 * Copyright (c) 2008 Princeton University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "mem/ruby/network/garnet/InputUnit.hh"

#include "debug/RubyNetwork.hh"
#include "debug/RubyMulticast.hh"
#include "mem/ruby/network/garnet/Credit.hh"
#include "mem/ruby/network/garnet/Router.hh"
#include "mem/ruby/network/garnet/NetworkInterface.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

InputUnit::InputUnit(int id, PortDirection direction, Router *router)
  : Consumer(router), m_router(router), m_id(id), m_direction(direction),
    m_vc_per_vnet(m_router->get_vc_per_vnet())
{
    const int m_num_vcs = m_router->get_num_vcs();
    m_num_buffer_reads.resize(m_num_vcs/m_vc_per_vnet);
    m_num_buffer_writes.resize(m_num_vcs/m_vc_per_vnet);
    for (int i = 0; i < m_num_buffer_reads.size(); i++) {
        m_num_buffer_reads[i] = 0;
        m_num_buffer_writes[i] = 0;
    }

    // Instantiating the virtual channels
    virtualChannels.reserve(m_num_vcs);
    for (int i=0; i < m_num_vcs; i++) {
        virtualChannels.emplace_back();
    }

    for (int i = 0; i < m_num_vcs; ++i) {
        multicastBuffers.emplace_back(this);
    }

    m_vnet_busy_count.resize(m_router->get_num_vnets(), 0);
}

/*
 * The InputUnit wakeup function reads the input flit from its input link.
 * Each flit arrives with an input VC.
 * For HEAD/HEAD_TAIL flits, performs route computation,
 * and updates route in the input VC.
 * The flit is buffered for (m_latency - 1) cycles in the input VC
 * and marked as valid for SwitchAllocation starting that cycle.
 *
 */

void
InputUnit::wakeup()
{
    if (this->isMulticastFanoutInPort()) {
        // This is a duplicated fanout InPort.
        assert(m_router->get_net_ptr()->getMulticastMode() ==
            GarnetNetwork::MulticastModeE::FANOUT_FLIT_AT_FORK);
        return;
    }
    if (m_in_link->isReady(curTick())) {
        flit *t_flit = m_in_link->consumeLink();
        DPRINTF(RubyNetwork, "InU-%d-%s "
            "Consuming:%s Width: %d Flit:%s\n",
            this->get_direction(),
            m_router->get_id(), m_in_link->name(),
            m_router->getBitWidth(), *t_flit);

        this->handleMulticastFlit(t_flit);

        this->addFlit(t_flit);
    }

    if (m_in_link->isReady(curTick())) {
        this->m_router->schedule_wakeup(Cycles(1));
    }
}

void
InputUnit::addFlit(flit *t_flit)
{

    int vc = t_flit->get_vc();

    t_flit->increment_hops(); // for stats

    auto flitType = t_flit->get_type();
    if ((flitType == HEAD_) || (flitType == HEAD_TAIL_)) {

        assert(virtualChannels[vc].get_state() == IDLE_);
        set_vc_active(vc, curTick());

        // Route computation for this vc
        int outport = m_router->route_compute(t_flit->get_route(),
            m_id, m_direction);

        // Update output port in VC
        // All flits in this packet will use this output port
        // The output port field in the flit is updated after it wins SA
        grant_outport(vc, outport);

    } else {
        assert(virtualChannels[vc].get_state() == ACTIVE_);
    }


    DPRINTF(RubyNetwork, "InU-%d-%s-%d size %d flit %d of %s.\n",
        m_router->get_id(),
        m_router->getPortDirectionName(this->get_direction()),
        vc,
        virtualChannels[vc].getSize(),
        t_flit->get_id(), *(t_flit->get_msg_ptr()));

    int vnet = vc/m_vc_per_vnet;
    // number of writes same as reads
    // any flit that is written will be read only once
    m_num_buffer_writes[vnet]++;
    m_num_buffer_reads[vnet]++;

    Cycles pipe_stages = m_router->get_pipe_stages();
    if (pipe_stages == 1) {
        // 1-cycle router
        // Flit goes for SA directly
        insertSAFlit(vc, t_flit, curTick());
    } else {
        assert(pipe_stages > 1);
        // Router delay is modeled by making flit wait in buffer for
        // (pipe_stages cycles - 1) cycles before going for SA

        Cycles wait_time = pipe_stages - Cycles(1);
        insertSAFlit(vc, t_flit, m_router->clockEdge(wait_time));

        // Wakeup the router in that cycle to perform SA
        m_router->schedule_wakeup(Cycles(wait_time));
        this->m_router->m_input_sched++;
    }
}

// Send a credit back to upstream router for this VC.
// Called by SwitchAllocator when the flit in this VC wins the Switch.
void
InputUnit::increment_credit(int in_vc, bool free_signal, Tick curTime)
{
    DPRINTF(RubyNetwork, "InU-%d-%s credit++ vc:%d free:%d\n",
        m_router->get_id(), this->get_direction(),
        in_vc, free_signal);

    if (this->isMulticastFanoutInPort()) {
        // We only care about free_signal.
        // And no need to send back any credit.
        if (free_signal) {
            auto mainInputUnit = m_router->getInputUnit(
                this->mainInPortNum);
            mainInputUnit->fanoutVCFreed(in_vc, curTime);
            return;
        }
    } else {
        // This is main input unit.
        if (free_signal) {
            auto &multicastBuffer = this->multicastBuffers.at(in_vc);
            if (multicastBuffer.isBuffering()) {
                if (m_router->get_net_ptr()->getMulticastMode() ==
                    GarnetNetwork::MulticastModeE::FANOUT_FLIT_AT_FORK) {
                    // We need to check if we have all fanout vcs freed.
                    this->fanoutVCFreed(in_vc, curTime);
                    return;
                }
            }
        }
        // Otherwise, just send back the 
        sendCredit(in_vc, free_signal, curTime);
    }
}

void
InputUnit::fanoutVCFreed(int in_vc, Tick curTime)
{
    auto &multicastBuffer = this->multicastBuffers.at(in_vc);
    assert(multicastBuffer.isBuffering());
    assert(m_router->get_net_ptr()->getMulticastMode() ==
        GarnetNetwork::MulticastModeE::FANOUT_FLIT_AT_FORK);
    assert(!this->isMulticastFanoutInPort());

    multicastBuffer.fanoutVCFreed++;
    assert(multicastBuffer.fanoutVCFreed <=
        multicastBuffer.routes.size() + 1);

    DPRINTF(RubyMulticast, "InU-%d-%s-%d fanout VC freed %d routes %d\n",
        m_router->get_id(), this->get_direction(), in_vc,
        multicastBuffer.fanoutVCFreed,
        multicastBuffer.routes.size());
    
    if (multicastBuffer.fanoutVCFreed ==
        multicastBuffer.routes.size() + 1) {
        // Every all multicast flit is done.
        // We can send back the free credit.
        this->sendCredit(in_vc, true, curTime);
        multicastBuffer.clear();
    }
}

void
InputUnit::sendCredit(int in_vc, bool free_signal, Tick curTime)
{
    Credit *t_credit = new Credit(in_vc, free_signal, curTime);
    creditQueue.insert(t_credit);
    m_credit_link->scheduleEventAbsolute(m_router->clockEdge(Cycles(1)));
}

bool
InputUnit::functionalRead(Packet *pkt, WriteMask &mask)
{
    bool read = false;
    for (auto& virtual_channel : virtualChannels) {
        if (virtual_channel.functionalRead(pkt, mask))
            read = true;
    }

    return read;
}

uint32_t
InputUnit::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    for (auto& virtual_channel : virtualChannels) {
        num_functional_writes += virtual_channel.functionalWrite(pkt);
    }

    return num_functional_writes;
}

void
InputUnit::resetStats()
{
    for (int j = 0; j < m_num_buffer_reads.size(); j++) {
        m_num_buffer_reads[j] = 0;
        m_num_buffer_writes[j] = 0;
    }
}

InputUnit::PortToDestinationMap InputUnit::groupDestinationByRouting(
    flit* inflyFlit,
    const std::vector<MachineID> &destMachineIDs) {
    const auto &route = inflyFlit->get_route();
    PortToDestinationMap grouped;
    for (auto &destMachineID : destMachineIDs) {
        // Create a fake RouteInfo to the the routing decision.
        auto destRawNodeID = destMachineID.getRawNodeID();
        RouteInfo fakeRoute;
        fakeRoute.vnet = route.vnet;
        fakeRoute.net_dest.add(destMachineID);
        fakeRoute.src_ni = route.src_ni;
        fakeRoute.src_router = route.src_router;
        fakeRoute.dest_ni = destRawNodeID;
        fakeRoute.dest_router = m_router->get_net_ptr()
            ->get_router_id(destRawNodeID, route.vnet);

        int outport = m_router->route_compute(fakeRoute, m_id, m_direction);
        grouped.emplace(std::piecewise_construct,
            std::forward_as_tuple(outport),
            std::forward_as_tuple()).first->second.push_back(destMachineID);
    }

    if (Debug::RubyMulticast) {
        std::stringstream ss;
        for (const auto &group : grouped) {
            auto outport = group.first;
            auto outDirection = m_router->getOutportDirection(outport);
            ss << " [" << outDirection << ']';
            for (const auto &destMachineID : group.second) {
                ss << ' ' << destMachineID;
            }
        }
        DPRINTF(RubyMulticast, "InU-%d-%s Multicast to %s\n",
            m_router->get_id(),
            m_router->getPortDirectionName(this->get_direction()),
            ss.str());
    }

    return grouped;
}

void InputUnit::handleMulticastFlit(flit *f) {
    auto &destination = f->get_route().net_dest;
    if (destination.count() == 1) {
        return;
    }
    switch (m_router->get_net_ptr()->getMulticastMode()) {
    case GarnetNetwork::MulticastModeE::DUPLICATE_MSG_AT_FORK:
        this->allocateMulticastBuffer(f, false /* fanout */);
        this->cloneMulticastFlitAsMsg(f);
        break;
    case GarnetNetwork::MulticastModeE::FANOUT_FLIT_AT_FORK:
        this->allocateMulticastBuffer(f, true /* fanout */);
        this->fanoutMulticastFlit(f);
        break;
    default:
        panic("Cannot handle MulticastMode.");
        break;
    }
}

void InputUnit::allocateMulticastBuffer(flit *f, bool fanout) {
    auto flitType = f->get_type();
    if (flitType != HEAD_ && flitType != HEAD_TAIL_) {
        return;
    }
    auto &destination = f->get_route().net_dest;

    int vc = f->get_vc();
    auto &multicastBuffer = this->multicastBuffers.at(vc);

    std::vector<MachineID> destMachineIDs = destination.getAllDestMachineID();

    /**
     * Group destination by routing out port. We prioritize the largest group.
     */
    auto destGroups = this->groupDestinationByRouting(f, destMachineIDs);
    auto selectOutport = destGroups.begin()->first;
    auto selectGroupSize = destGroups.begin()->second.size();
    for (const auto &group : destGroups) {
        if (group.second.size() > selectGroupSize) {
            selectOutport = group.first;
            selectGroupSize = group.second.size();
        } else if (group.second.size() == selectGroupSize) {
            // Heuristic to deprioritize the one going to myself.
            if (m_router->getOutportDirection(group.first) != "Local") {
                selectOutport = group.first;
                selectGroupSize = group.second.size();
            }
        }
    }
    DPRINTF(RubyMulticast, "InU-%d-%s-%d Select outport %s.\n",
        m_router->get_id(),
        m_router->getPortDirectionName(this->get_direction()),
        vc,
        m_router->getPortDirectionName(
            m_router->getOutportDirection(selectOutport)));
    /**
     * If all destination goes to the same outport, then we are fine.
     */
    if (destGroups.size() == 1) {
        return;
    }
    /**
     * If there is remaining destinations, duplicate the flits.
     */
    const auto &route = f->get_route();
    std::vector<RouteInfo> remainRoutes;
    RouteInfo *remainRoute = nullptr;

    int remainDestRawNodeId = -1;
    for (const auto &group : destGroups) {
        if (group.first == selectOutport) {
            continue;
        }

        if (remainRoutes.empty() || fanout) {
            // Add new route.
            remainRoutes.emplace_back();
            remainRoute = &remainRoutes.back();
            remainRoute->vnet = route.vnet;
            remainRoute->src_ni = route.src_ni;
            remainRoute->src_router = route.src_router;
            // Clear the hop count of the new route.
            remainRoute->hops_traversed = -1;
            // Clear the remainDestRawNodeId.
            remainDestRawNodeId = -1;
        }

        for (const auto &dest : group.second) {
            remainRoute->net_dest.add(dest);
            if (remainDestRawNodeId == -1) {
                remainDestRawNodeId = dest.getRawNodeID();
                remainRoute->dest_ni = remainDestRawNodeId;
                remainRoute->dest_router = m_router->get_net_ptr()
                    ->get_router_id(remainDestRawNodeId, route.vnet);
            }
        }
    }

    std::vector<MsgPtr> remainMsgs;
    for (const auto &remainRoute : remainRoutes) {
        auto remainMsg = f->get_msg_ptr()->clone();
        remainMsg->getDestination() = remainRoute.net_dest;
        remainMsg->setVnet(f->get_vnet());
        remainMsgs.push_back(remainMsg);
    }

    // Allocate the MulticastBuffer.
    multicastBuffer.allocate(
        std::move(remainRoutes), std::move(remainMsgs));
}

void InputUnit::cloneMulticastFlitAsMsg(flit *f) {

    /**
     * We may need to duplicate the message if it's multicast.
     */
    int vc = f->get_vc();
    auto &multicastBuffer = this->multicastBuffers.at(vc);
    if (!multicastBuffer.isBuffering()) {
        return;
    }

    // Modify the original flit to subtract these destinations.
    this->removeMulticastDestFromFlit(f, multicastBuffer);

    auto flitType = f->get_type();
    if (flitType == TAIL_ || flitType == HEAD_TAIL_) {
        // We can duplicate msg to network interface,
        // and release multicast buffer.
        this->duplicateMulticastMsgToNetworkInterface(multicastBuffer);
        multicastBuffer.clear();
    }
}

void
InputUnit::removeMulticastDestFromFlit(flit *f, MulticastBuffer &buffer) {
    auto &route = f->get_route();
    for (const auto &remainRoute : buffer.routes) {
        route.net_dest.removeNetDest(remainRoute.net_dest);
    }

    assert(!route.net_dest.isEmpty() && "No dest.");
    auto selectDestRawNodeID = route.net_dest.smallestElement().getRawNodeID();
    route.dest_ni = selectDestRawNodeID;
    route.dest_router = m_router->get_net_ptr()->get_router_id(
        selectDestRawNodeID, route.vnet);
    f->get_msg_ptr()->getDestination() = route.net_dest;
}

void InputUnit::duplicateMulticastMsgToNetworkInterface(
    MulticastBuffer &buffer) {

    for (int i = 0; i < buffer.msgs.size(); ++i) {
        auto msg = buffer.msgs.at(i);
        const auto &route = buffer.routes.at(i);
        this->duplicateMulticastMsgToNetworkInterface(msg, route);
    }
}

void
InputUnit::duplicateMulticastMsgToNetworkInterface(
    MsgPtr &msg, const RouteInfo &route) {
    
    bool duplicateToInput = true;
    // Bypass logic for if the message is for local machine id.
    if (m_router->get_net_ptr()->isMulticastLocalBypassEnabled() &&
        msg->getDestination().count() == 1 &&
        route.dest_router == this->m_router->get_id()) {
        auto destMachineId = msg->getDestination().singleElement();
        auto destNodeId = destMachineId.getRawNodeID();
        auto destNI = m_router->get_net_ptr()->getNetworkInterface(destNodeId);
        destNI->injectMsgToOutput(msg);
        duplicateToInput = false;
    } else {
        // Get the Local NetworkInterface.
        // ! This assumes one router per node.
        auto senderNI = m_router->get_net_ptr()
            ->getNetworkInterface(route.src_ni);
        auto senderNodeId = senderNI->get_node_id();
        auto senderMachineId = MachineID::getMachineIDFromRawNodeID(
            senderNodeId);
        auto senderMachineType = senderMachineId.getType();
        /**
         * Try to get the LocalMachineId. Here I assume all
         * routers are connected to the L2 cache.
         */
        auto localMachineType = MachineType_NULL;
        if (senderMachineType == MachineType_Directory ||
            senderMachineType == MachineType_L2Cache) {
            localMachineType = MachineType_L2Cache;
        } else if (senderMachineType == MachineType_L1Cache) {
            localMachineType = MachineType_L1Cache;
        }
        if (localMachineType == MachineType::MachineType_NULL) {
            panic("Multicast from Machine %s -> %s: %s.",
                senderMachineId, msg->getDestination(), *msg);
        }
        auto localMachineId = MachineID(localMachineType, m_router->get_id());
        if (localMachineId.getNum() >= MachineType_base_count(
            localMachineType)) {
            panic("Local MachineId %s Overflow. Total %d.",
                  localMachineId, MachineType_base_count(localMachineType));
        }
        auto localNodeId = localMachineId.getRawNodeID();
        auto localNI = m_router->get_net_ptr()->getNetworkInterface(
            localNodeId);
        // Inject the message.
        localNI->injectMsgToInput(msg);
    }

    if (Debug::RubyMulticast) {
        std::stringstream ss;
        for (const auto &destNodeId : msg->getDestination().getAllDest()) {
            auto destMachineId = MachineID::getMachineIDFromRawNodeID(destNodeId);
            ss << ' ' << destMachineId;
        }
        DPRINTF(RubyMulticast, "InU-%d-%s "
            "Duplicated Multicast to %s msg %s.\n",
            m_router->get_id(),
            m_router->getPortDirectionName(this->get_direction()),
            duplicateToInput ? "Input" : "Output",
            *msg);
    }
}

void InputUnit::fanoutMulticastFlit(flit *f) {

    /**
     * We may need to duplicate the message if it's multicast.
     */
    int vc = f->get_vc();
    auto &multicastBuffer = this->multicastBuffers.at(vc);
    if (!multicastBuffer.isBuffering()) {
        return;
    }

    for (int i = 0; i < multicastBuffer.routes.size(); ++i) {
        auto &msg = multicastBuffer.msgs.at(i);
        const auto &route = multicastBuffer.routes.at(i);
        auto remainFlit = new flit(
            f->getPacketID(),
            f->get_id(),
            f->get_vc(),
            f->get_vnet(),
            route,
            f->get_size(),
            msg,
            f->msgSize,
            f->m_width,
            m_router->curCycle());

        if (i >= this->fanoutInPortNums.size()) {
            panic("Multicast Fanout Overflow.");
        }
        auto fanoutInPortNum = this->fanoutInPortNums.at(i);
        auto fanoutInputUnit = m_router->getInputUnit(fanoutInPortNum);
        
        fanoutInputUnit->addFlit(remainFlit);
    }

    // Modify the original flit to subtract these destinations.
    this->removeMulticastDestFromFlit(f, multicastBuffer);
}

} // namespace garnet
} // namespace ruby
} // namespace gem5

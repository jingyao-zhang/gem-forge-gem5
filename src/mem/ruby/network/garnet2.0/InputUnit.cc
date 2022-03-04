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


#include "mem/ruby/network/garnet2.0/InputUnit.hh"

#include "debug/RubyNetwork.hh"
#include "debug/RubyMulticast.hh"
#include "mem/ruby/network/garnet2.0/Credit.hh"
#include "mem/ruby/network/garnet2.0/Router.hh"
#include "mem/ruby/network/garnet2.0/NetworkInterface.hh"
#include "debug/RubyNetwork.hh"

using namespace std;

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
    flit *t_flit = this->selectFlit();

    if (t_flit) {
        int vc = t_flit->get_vc();
        t_flit->increment_hops(); // for stats

        if ((t_flit->get_type() == HEAD_) ||
            (t_flit->get_type() == HEAD_TAIL_)) {

            assert(virtualChannels[vc].get_state() == IDLE_);
            set_vc_active(vc, m_router->curCycle());
        } else {
            assert(virtualChannels[vc].get_state() == ACTIVE_);
        }

        this->allocateMulticastBuffer(t_flit);
        this->duplicateMulitcastFlit(t_flit);

        auto flitType = t_flit->get_type();
        if ((flitType == HEAD_) || (flitType == HEAD_TAIL_)) {
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


        DPRINTF(RubyNetwork, "InputUnit[%d][%s][%d] size %d flit %d of %s.\n",
            m_router->get_id(),
            m_router->getPortDirectionName(this->get_direction()),
            vc,
            virtualChannels[vc].getSize(),
            t_flit->get_id(), *(t_flit->get_msg_ptr()));

        // Buffer the flit
        virtualChannels[vc].insertFlit(t_flit);

        int vnet = vc/m_vc_per_vnet;
        // number of writes same as reads
        // any flit that is written will be read only once
        m_num_buffer_writes[vnet]++;
        m_num_buffer_reads[vnet]++;

        Cycles pipe_stages = m_router->get_pipe_stages();
        if (pipe_stages == 1) {
            // 1-cycle router
            // Flit goes for SA directly
            t_flit->advance_stage(SA_, m_router->curCycle());
        } else {
            assert(pipe_stages > 1);
            // Router delay is modeled by making flit wait in buffer for
            // (pipe_stages cycles - 1) cycles before going for SA

            Cycles wait_time = pipe_stages - Cycles(1);
            t_flit->advance_stage(SA_, m_router->curCycle() + wait_time);

            // Wakeup the router in that cycle to perform SA
            m_router->schedule_wakeup(Cycles(wait_time));
        }

    }

    if (m_in_link->isReady(m_router->curCycle()) ||
        this->totalReadyMulitcastFlits > 0) {
        // Due to MulticastBuffer, we need to aggressively schedule wakeup.
        this->m_router->schedule_wakeup(Cycles(1));
    }
}

// Send a credit back to upstream router for this VC.
// Called by SwitchAllocator when the flit in this VC wins the Switch.
void
InputUnit::increment_credit(int in_vc, bool free_signal, Cycles curTime)
{
    Credit *t_credit = new Credit(in_vc, free_signal, curTime);
    creditQueue.insert(t_credit);
    m_credit_link->scheduleEventAbsolute(m_router->clockEdge(Cycles(1)));
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
        fakeRoute.dest_router = m_router->get_net_ptr()->get_router_id(destRawNodeID);

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
            ss << " [" << m_router->getPortDirectionName(outDirection) << ']';
            for (const auto &destMachineID : group.second) {
                ss << ' ' << destMachineID;
            }
        }
        DPRINTF(RubyMulticast, "InputUnit[%d][%s] Multicast to %s\n",
            m_router->get_id(),
            m_router->getPortDirectionName(this->get_direction()),
            ss.str());
    }

    return grouped;
}

flit *InputUnit::selectFlit() {

    auto checkIfCanPop = [this](flit *f) -> bool {
        auto flitType = f->get_type();
        if (flitType == HEAD_ || flitType == HEAD_TAIL_) {
            if (virtualChannels[f->get_vc()].get_state() != IDLE_) {
                return false;
            }
        }
        return true;
    };

    /**
     * Prioritize the MulitcastDuplicateBuffer.
     */
    if (m_router->get_net_ptr()->isMulticastEnabled() &&
        this->totalReadyMulitcastFlits > 0) {
        int selectBufferIdx = -1;
        for (int i = 0; i < virtualChannels.size(); ++i) {
            auto idx = (this->currMulticastBufferIdx + i) % virtualChannels.size();
            auto &buffer = this->multicastBuffers.at(idx);
            if (buffer.isReady()) {
                selectBufferIdx = idx;
                break;
            }
        }
        assert(selectBufferIdx >= 0 && "No ready MulticastBuffer.");
        auto &buffer = this->multicastBuffers.at(selectBufferIdx);
        flit *f = buffer.peek();

        /**
         * Check if we can find an idle vc for the header vc.
         */
        auto flitType = f->get_type();
        if (flitType == HEAD_ || flitType == HEAD_TAIL_) {
            auto vc = this->calculateVCForMulticastDuplicateFlit(f->get_vnet());
            if (vc != -1) {
                // We found one idle vc, set all flits.
                buffer.setVCForFrontMsg(vc);
            } else {
                // The below checkIfCanPop() will fail for us.
            }
        }

        /**
         * Check if the vc is idle.
         */
        if (checkIfCanPop(f)) {
            /**
             * If this is the tail flit, we want to increment
             * currMulitcastBufferIdx to round robin.
             */
            buffer.pop();
            auto flitType = f->get_type();
            if (flitType == TAIL_ || flitType == HEAD_TAIL_) {
                this->currMulticastBufferIdx = (selectBufferIdx + 1)
                    % virtualChannels.size();
            }

            DPRINTF(RubyMulticast, "InputUnit[%d][%s][%d] "
                "MulticastBuffer Pop: Flit %d.\n",
                m_router->get_id(),
                m_router->getPortDirectionName(this->get_direction()),
                f->get_vc(),
                f->get_id());
            return f;
        }
    }

    if (m_in_link->isReady(m_router->curCycle())) {
        flit *f = m_in_link->peekLink();
        if (checkIfCanPop(f)) {
            m_in_link->consumeLink();
            return f;
        }
    }
    return nullptr;
}

void InputUnit::allocateMulticastBuffer(flit *f) {
    auto flitType = f->get_type();
    if (flitType != HEAD_ && flitType != HEAD_TAIL_) {
        return;
    }
    auto &destination = f->get_route().net_dest;
    if (destination.count() == 1) {
        return;
    }

    int vc = f->get_vc();
    auto &multicastBuffer = this->multicastBuffers.at(vc);

    std::vector<NodeID> destRawNodeIDs = destination.getAllDest();
    std::vector<MachineID> destMachineIDs;
    for (auto &destRawNodeID : destRawNodeIDs) {
        destMachineIDs.push_back(
            MachineID::getMachineIDFromRawNodeID(destRawNodeID));
    }
    assert(m_router->get_net_ptr()->isMulticastEnabled() &&
        "Message with multiple destinations received when Multicast disabled.");

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
        }
    }
    DPRINTF(RubyMulticast, "InputUnit[%d][%s][%d] Select outport %s.\n",
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
    int remainDestRawNodeId = -1;
    RouteInfo remainRoute;
    remainRoute.vnet = route.vnet;
    for (const auto &group : destGroups) {
        if (group.first == selectOutport) {
            continue;
        }
        for (const auto &dest : group.second) {
            if (remainDestRawNodeId == -1) {
                remainDestRawNodeId = dest.getRawNodeID();
            }
            remainRoute.net_dest.add(dest);
        }
    }
    remainRoute.src_ni = route.src_ni;
    remainRoute.src_router = route.src_router;
    remainRoute.dest_ni = remainDestRawNodeId;
    remainRoute.dest_router = m_router->get_net_ptr()->get_router_id(
        remainDestRawNodeId);
    // Clear the hop count of the new route.
    remainRoute.hops_traversed = -1;

    auto remainMsg = f->get_msg_ptr()->clone();
    remainMsg->getDestination() = remainRoute.net_dest;
    remainMsg->setVnet(f->get_vnet());
    // Allocate the MulticastDuplicateBuffer.
    multicastBuffer.allocate(remainRoute, remainMsg);
}

void InputUnit::duplicateMulitcastFlit(flit *f) {

    /**
     * We may need to duplicate the message if it's multicast.
     */
    int vc = f->get_vc();
    auto &multicastBuffer = this->multicastBuffers.at(vc);
    if (!multicastBuffer.isBuffering()) {
        return;
    }
    auto remainFlit = new flit(
        f->get_id(),
        f->get_vc(),
        f->get_vnet(),
        multicastBuffer.route,
        f->get_size(),
        multicastBuffer.msg,
        m_router->curCycle());
    multicastBuffer.push(remainFlit);
    DPRINTF(RubyMulticast, "InputUnit[%d][%s][%d] "
        "MulticastBuffer Push Flit %d.\n",
        m_router->get_id(),
        m_router->getPortDirectionName(this->get_direction()),
        vc,
        remainFlit->get_id());
    std::vector<MachineID> destMachineIDs;
    {
        // Compute the all destinations.
        auto &destination = f->get_route().net_dest;
        std::vector<NodeID> destRawNodeIDs = destination.getAllDest();
        for (auto &destRawNodeID : destRawNodeIDs) {
            destMachineIDs.push_back(
                MachineID::getMachineIDFromRawNodeID(destRawNodeID));
        }
    }
    // Modify the original flit to subtract these destinations.
    auto &route = f->get_route();
    int selectDestRawNodeID = -1;
    route.net_dest.clear();
    for (const auto &dest : destMachineIDs) {
        if (multicastBuffer.route.net_dest.isElement(dest)) {
            continue;
        }
        if (selectDestRawNodeID == -1) {
            selectDestRawNodeID = dest.getRawNodeID();
        }
        route.net_dest.add(dest);
    }
    route.dest_ni = selectDestRawNodeID;
    route.dest_router = m_router->get_net_ptr()->get_router_id(
        selectDestRawNodeID);
    f->get_msg_ptr()->getDestination() = route.net_dest;
}

int InputUnit::calculateVCForMulticastDuplicateFlit(int vnet) {
    for (int i = 0; i < m_vc_per_vnet; i++) {
        auto vc = vnet * m_vc_per_vnet + i;
        if (virtualChannels[vc].get_state() == IDLE_) {
            m_vnet_busy_count[vnet] = 0;
            return vc;
        }
    }

    m_vnet_busy_count[vnet]++;
    panic_if(m_vnet_busy_count[vnet] > 50000,
        "%s: Possible network deadlock in vnet: %d at time: %llu \n",
        name(), vnet, curTick());

    return -1;
}

void InputUnit::duplicateMulticastMsgToNetworkInterface(
    MulticastDuplicateBuffer &buffer) {
    auto f = buffer.peek();
    auto msg = f->get_msg_ptr();

    // Get the Local NetworkInterface.
    // ! This assumes one router per node.
    auto senderNI = m_router->get_net_ptr()->getNetworkInterface(f->get_route().src_ni);
    auto senderNodeId = senderNI->get_node_id();
    auto senderMachineId = MachineID::getMachineIDFromRawNodeID(senderNodeId);
    auto senderMachineType = senderMachineId.getType();
    /**
     * Try to get the LocalMachineId. Here I assume all routers are connected to the L2 cache.
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
    if (localMachineId.getNum() >= MachineType_base_count(localMachineType)) {
        panic("Local MachineId %s Overflow. Total %d.",
              localMachineId, MachineType_base_count(localMachineType));
    }
    auto localNodeId = localMachineId.getRawNodeID();
    auto localNI = m_router->get_net_ptr()->getNetworkInterface(localNodeId);
    // Inject the message.
    localNI->injectMulticastDuplicateMsg(msg);

    if (Debug::RubyMulticast) {
        std::stringstream ss;
        for (const auto &destNodeId : msg->getDestination().getAllDest()) {
            auto destMachineId = MachineID::getMachineIDFromRawNodeID(destNodeId);
            ss << ' ' << destMachineId;
        }
        DPRINTF(RubyMulticast, "InputUnit[%d][%s] Inject Duplicated Multicast from %s to %s.\n",
            m_router->get_id(),
            m_router->getPortDirectionName(this->get_direction()),
            senderMachineId,
            msg->getDestination());
    }

    // Release all flits.
    auto size = f->get_size();
    for (int i = 0; i < size; ++i) {
        delete buffer.pop();
    }
    assert(this->totalReadyMulitcastFlits == 0);
}

#include "Message.hh"

void Message::chainMsg(const MsgPtr &msg) {
  assert(!msg->m_chainMsg && "Already chained.");
  const auto &dest1 = this->getDestination();
  const auto &dest2 = msg->getDestination();
  if (!(dest1.count() == 1 && dest1.isEqual(dest2))) {
    panic("Can't chain multicast or different destination message:\n%s\n\n%s.",
          *this, *msg);
  }
  msg->m_chainMsg = this->m_chainMsg;
  this->m_chainMsg = msg;
}
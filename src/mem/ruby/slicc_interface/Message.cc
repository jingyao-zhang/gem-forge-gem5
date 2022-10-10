#include "Message.hh"

void Message::chainMsg(const MsgPtr &msg) {
  assert(!msg->m_chainMsg && "Already chained.");
  const auto &dest = msg->getDestination();
  if (dest.count() != 1) {
    panic("Can't chain multicast multi-dest message:\n%s\n\n%s.", *this, *msg);
  }
  msg->m_chainMsg = this->m_chainMsg;
  this->m_chainMsg = msg;
}
#ifndef __MEM_RUBY_SYSTEM_IDEAL_SEQUENCER_HH__
#define __MEM_RUBY_SYSTEM_IDEAL_SEQUENCER_HH__

#include "mem/ruby/slicc_interface/RubyRequest.hh"

#include <queue>
#include <unordered_map>

/**
 * A helper class to implement the ideal memory in sequencer.
 */

class Sequencer;

class IdealSequencer {
public:
  using RubyReqPtr = std::shared_ptr<RubyRequest>;

  IdealSequencer(Sequencer *_seq) : seq(_seq) {
    // Global initializer.
    if (drainEvent == nullptr) {
      drainEvent = new IdealDrainEvent();
    }
  }

  /**
   * Push one request out to the ideal system.
   */
  void pushRequest(RubyReqPtr req);

private:
  Sequencer *seq;

  /**
   * An ideal RubyRequest, used in our global queue.
   */
  struct IdealRubyRequest {
    IdealSequencer *seq;
    RubyReqPtr req;
    Tick readyTick;
    IdealRubyRequest(IdealSequencer *_seq, RubyReqPtr _req, Tick _readyTick)
        : seq(_seq), req(_req), readyTick(_readyTick) {}
    IdealRubyRequest(IdealSequencer *_seq, RubyReqPtr _req)
        : seq(_seq), req(_req), readyTick(0) {}
  };

  /**
   * Global queue on the ideal RubyRequests.
   */
  static std::queue<IdealRubyRequest> globalQueue;

  /**
   * Global locked map to implement x86 LockedRMW operation.
   */
  static std::unordered_map<Addr, IdealSequencer *> globalLockedMap;
  static std::unordered_map<Addr, std::queue<IdealRubyRequest>>
      globalLockedQueue;

  /**
   * Global drain callback event.
   */
  class IdealDrainEvent : public Event {
  public:
    IdealDrainEvent() {}
    void process() override;
    const char *description() const override;
  };
  // I use pointer here to avoid destructor, which assert(!scheduled()) but
  // fails, and I am not sure about the reason.
  static IdealDrainEvent *drainEvent;
};

#endif
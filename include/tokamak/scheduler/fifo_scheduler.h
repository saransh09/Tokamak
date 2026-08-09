#pragma once

#include "tokamak/backend/backend.h"
#include "tokamak/common/clock.h"
#include "tokamak/request/request.h"
#include "tokamak/request/state.h"
#include "tokamak/scheduler/tick_report.h"
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>
namespace tokamak {

// FIFO scheduler: oldest-submitted-first, unbounded batches (no KV-cache
// capacity constraint exists yet -- see learning/learnings_005.md Section 3
// for why this is a genuine, named limitation of Milestone 1's scope, not
// an oversight). No preemption, no chunked prefill, no fairness beyond
// submission order.
//
// tick() runs same-tick prefill-to-decode promotion (ADR-007): a request
// that finishes prefill this tick is included in this same tick's decode
// batch, never parked across a tick boundary.
//
// Retention note: FifoScheduler retains every Request it is given ownership
// of -- including terminal (Completed/Failed/Cancelled/Rejected) ones -- in
// its master list for the lifetime of the scheduler. There is currently no
// "reap terminal requests" mechanism; this is fine for Milestone 1's
// short-lived deterministic-simulation tests but is a known gap flagged
// for whenever the scheduler needs to run for a long time in-process.
class FifoScheduler {
public:
  // `backend` and `clock` are not owned; caller must keep them alive for
  // the scheduler's lifetime (same reference-lifetime discipline as
  // RequestLifecycle's Clock&)
  FifoScheduler(InferenceBackend &backend, const Clock &clock);

  // Takes ownership of `request` and enqueues it for prefill. Returns a
  // reference to the now-scheduler-owned Request -- valid for the scheduler's lifetime per
  // the retention note above -- so callers/tests can poll
  // request.lifecycle().state() after calling tick().
  Request& submit(std::unique_ptr<Request> request);

  // Cancels the request with the given id if it is currently tracked
  // (waiting or decoding). No-op if not found or already terminal
  // (matches RequestLifecycle::cancel()'s idempotence). Releases the
  // backend handle if one had been minted (ADR-007).
  void cancel(const RequestId &id);

  // Runs one scheduling iteration:
    //   1. Expire anything already past its deadline (waiting or decoding)
    //      before doing any backend work -- avoids wasting a prefill/decode
    //      call on a request that's already doomed.
    //   2. Prefill phase: prefill() everything in the waiting queue; every
    //      success is promoted straight through to Decoding this same tick.
    //   3. Decode phase: decode() everything now in Decoding (prior-tick +
    //      newly-promoted) as one batch.
    // Returns a TickReport summarizing what this call actually did (ADR-008).
  TickReport tick();

  std::size_t waiting_count() const { return waiting_.size(); }
  std::size_t decoding_count() const { return decoding_.size(); }

private:
  TickReport expire_deadlines();
  TickReport prefill_phase();
  TickReport decode_phase();

  // Shared retirement path: transitions `request` to `terminal_state`,
  // releases its backend handle if one exists (ADR007: unconditional,
  // every retirement path), and drops it from waiting_/decoding_/handles_.
  // Does not remove it from `requests_` (see retention note).
  void retire(Request &request, RequestState terminal_state);

  InferenceBackend &backend_;
  const Clock &clock_;

  // Master ownersip list. Raw pointers below are stable for as long as
  // entries here are never erased (see retention note)
  std::vector<std::unique_ptr<Request>> requests_;

  std::deque<Request *> waiting_;
  std::vector<Request *> decoding_;

  std::unordered_map<RequestId, SequenceHandle> handles_;
};

} // namespace tokamak

#include "tokamak/scheduler/fifo_scheduler.h"
#include "tokamak/backend/backend.h"
#include "tokamak/request/state.h"

namespace tokamak {

FifoScheduler::FifoScheduler(InferenceBackend &backend, const Clock &clock)
    : backend_(backend), clock_(clock) {}

Request &FifoScheduler::submit(std::unique_ptr<Request> request) {
  Request *raw = request.get();
  requests_.emplace_back(std::move(request));
  raw->lifecycle().transition_to(RequestState::kAdmitted);
  raw->lifecycle().transition_to(RequestState::kWaitingPrefill);
  waiting_.push_back(raw);
  return *raw;
}

void FifoScheduler::retire(Request &request, RequestState terminal_state) {
  request.lifecycle().transition_to(terminal_state);
  if (handles_.contains(request.id())) {
    backend_.release(handles_.at(request.id()));
    handles_.erase(request.id());
  }
  for (auto it = waiting_.begin(); it != waiting_.end(); it++) {
    if (*it == &request) {
      waiting_.erase(it);
      break;
    }
  }
  for (auto it = decoding_.begin(); it != decoding_.end(); it++) {
    if (*it == &request) {
      decoding_.erase(it);
      break;
    }
  }
}

void FifoScheduler::expire_deadlines() {
  std::vector<Request *> expired_waiting;
  std::vector<Request *> expired_decoding;

  for (Request *req : waiting_) {
    if (clock_.now() > req->deadline_at())
      expired_waiting.push_back(req);
  }
  for (Request *req : decoding_) {
    if (clock_.now() > req->deadline_at())
      expired_decoding.push_back(req);
  }

  for (Request *req : expired_waiting) {
    // kWaitingPrefill has no direct edge to kFailed -- stop over at
    // kPrefilling first, same reasoning as prefill_phase()'s failure path.
    req->lifecycle().transition_to(RequestState::kPrefilling);
    retire(*req, RequestState::kFailed);
  }
  for (Request *req : expired_decoding) {
    // kDecoding -> kFailed is a direct legal edge.
    retire(*req, RequestState::kFailed);
  }
}

void FifoScheduler::prefill_phase() {
  std::vector<Request *> pending(waiting_.begin(), waiting_.end());

  PrefillBatch batch;
  batch.sequences.reserve(pending.size());
  for (Request *req : pending) {
    batch.sequences.push_back(
        PrefillRequest{TokenizedPrompt{req->prompt_token_ids()}});
  }

  PrefillResult results = backend_.prefill(batch);

  for (std::size_t i = 0; i < pending.size(); ++i) {
    Request *req = pending[i];
    const auto &outcome = results.outcomes[i];

    if (outcome.has_value()) {
      handles_.emplace(req->id(), outcome.value().handle);
      req->lifecycle().transition_to(RequestState::kPrefilling);
      req->lifecycle().transition_to(RequestState::kWaitingDecode);
      req->lifecycle().transition_to(RequestState::kDecoding);
      std::erase(waiting_, req);
      decoding_.push_back(req);
    } else {
      req->lifecycle().transition_to(RequestState::kPrefilling);
      retire(*req, RequestState::kFailed);
    }
  }
}

void FifoScheduler::decode_phase() {
  std::vector<Request *> pending(decoding_.begin(), decoding_.end());

  DecodeBatch batch;
  batch.sequences.reserve(pending.size());
  for (Request *req : pending) {
    batch.sequences.push_back(DecodeRequest{handles_.at(req->id())});
  }

  DecodeResult results = backend_.decode(batch);

  for (std::size_t i = 0; i < pending.size(); ++i) {
    Request *req = pending[i];
    const auto &outcome = results.outcomes[i];

    if (outcome.has_value()) {
      req->emit_token();

      const bool eos = outcome.value().finished;
      const bool exhausted =
          req->output_tokens_emitted() >= req->max_output_tokens();

      if (eos || exhausted) {
        retire(*req, RequestState::kCompleted);
      }
    } else {
      retire(*req, RequestState::kFailed);
    }
  }
}

void FifoScheduler::tick() {
  expire_deadlines();
  prefill_phase();
  decode_phase();
}

void FifoScheduler::cancel(const RequestId &id) {
  for (Request *req : waiting_) {
    if (req->id() == id) {
      retire(*req, RequestState::kCancelled);
      break;
    }
  }
  for (Request *req : decoding_) {
    if (req->id() == id) {
      retire(*req, RequestState::kCancelled);
      break;
    }
  }
}

} // namespace tokamak

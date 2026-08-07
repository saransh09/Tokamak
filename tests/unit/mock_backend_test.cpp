#include <catch2/catch_test_macros.hpp>

#include "tokamak/backend/mock_backend.h"
#include "tokamak/common/clock.h"

using namespace std::chrono_literals;
using tokamak::BackendCapabilities;
using tokamak::DecodeBatch;
using tokamak::DecodeRequest;
using tokamak::FakeClock;
using tokamak::MockBackend;
using tokamak::PrefillBatch;
using tokamak::PrefillRequest;
using tokamak::SequenceHandle;
using tokamak::TokenizedPrompt;

TEST_CASE("capabilities() reports honest values", "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  BackendCapabilities caps = backend.capabilities();

  REQUIRE(caps.supports_continuous_batching);
  REQUIRE_FALSE(caps.supports_prefix_sharing);
  REQUIRE_FALSE(caps.supports_kv_export);
}

TEST_CASE("tokenize() is deterministic and byte-based", "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  TokenizedPrompt first = backend.tokenize("AB");
  TokenizedPrompt second = backend.tokenize("AB");

  REQUIRE(first.token_ids == second.token_ids);
  REQUIRE(first.token_ids.size() == 2);
  REQUIRE(first.token_ids[0] == 65); // 'A'
  REQUIRE(first.token_ids[1] == 66); // 'B'
}

TEST_CASE("prefill() advances the clock proportional to prompt length",
          "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 2ms, 5ms);

  PrefillBatch batch;
  batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1, 2, 3}}}); // 3
  batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1, 2}}}); // 2

  backend.prefill(batch);

  // 5 total tokens * 2ms/token = 10ms
  REQUIRE(clock.now() == tokamak::TimePoint{10ms});
}

TEST_CASE("prefill() mints distinct sequence handles", "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  PrefillBatch batch;
  batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});

  auto result = backend.prefill(batch);

  REQUIRE(result.outcomes.size() == 2);
  REQUIRE(result.outcomes[0].has_value());
  REQUIRE(result.outcomes[1].has_value());
  REQUIRE_FALSE(result.outcomes[0]->handle == result.outcomes[1]->handle);
}

TEST_CASE("decode() advances the clock by a flat cost for a batch of one",
          "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  PrefillBatch prefill_batch;
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1, 2, 3}}});
  auto prefill_result = backend.prefill(prefill_batch);
  SequenceHandle handle = prefill_result.outcomes[0]->handle;

  auto before = clock.now();

  DecodeBatch decode_batch;
  decode_batch.sequences.push_back(DecodeRequest{.handle = handle});
  backend.decode(decode_batch);

  REQUIRE(clock.now() - before == 5ms);
}

TEST_CASE(
    "decode() advances the clock by the same flat cost for a batch of three",
    "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  PrefillBatch prefill_batch;
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  auto prefill_result = backend.prefill(prefill_batch);

  auto before = clock.now();

  DecodeBatch decode_batch;
  for (auto &outcome : prefill_result.outcomes) {
    decode_batch.sequences.push_back(DecodeRequest{.handle = outcome->handle});
  }
  backend.decode(decode_batch);

  REQUIRE(clock.now() - before == 5ms);
}

TEST_CASE("decode() increments tokens_emitted and returns increasing token ids",
          "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  PrefillBatch prefill_batch;
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  auto prefill_result = backend.prefill(prefill_batch);
  SequenceHandle handle = prefill_result.outcomes[0]->handle;

  DecodeBatch decode_batch;
  decode_batch.sequences.push_back(DecodeRequest{.handle = handle});

  auto first = backend.decode(decode_batch);
  auto second = backend.decode(decode_batch);
  auto third = backend.decode(decode_batch);

  REQUIRE(first.outcomes[0]->token_id == 1);
  REQUIRE(first.outcomes[0]->finished == false);
  REQUIRE(second.outcomes[0]->token_id == 2);
  REQUIRE(second.outcomes[0]->finished == false);
  REQUIRE(third.outcomes[0]->token_id == 3);
  REQUIRE(third.outcomes[0]->finished == false);
}

TEST_CASE("configure_eos() causes finished=true at the configured token count",
          "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  PrefillBatch prefill_batch;
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  auto prefill_result = backend.prefill(prefill_batch);
  SequenceHandle handle = prefill_result.outcomes[0]->handle;

  backend.configure_eos(handle, 3);

  DecodeBatch decode_batch;
  decode_batch.sequences.push_back(DecodeRequest{.handle = handle});

  auto first = backend.decode(decode_batch);
  auto second = backend.decode(decode_batch);
  auto third = backend.decode(decode_batch);

  REQUIRE(first.outcomes[0]->finished == false);
  REQUIRE(second.outcomes[0]->finished == false);
  REQUIRE(third.outcomes[0]->finished == true);
}

TEST_CASE("release() completes without error on a valid handle",
          "[mock_backend]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);

  PrefillBatch prefill_batch;
  prefill_batch.sequences.push_back(
      PrefillRequest{.prompt = TokenizedPrompt{.token_ids = {1}}});
  auto prefill_result = backend.prefill(prefill_batch);
  SequenceHandle handle = prefill_result.outcomes[0]->handle;

  DecodeBatch decode_batch;
  decode_batch.sequences.push_back(DecodeRequest{.handle = handle});
  backend.decode(decode_batch);

  backend.release(handle);
  // No crash => success. We cannot positively assert the internal state was
  // erased without exercising the panic() path (double-release), which
  // Catch2 cannot catch since panic() calls std::abort().
  SUCCEED("release() did not abort on a valid handle");
}

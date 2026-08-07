# Learnings 004 — Backend Compute Model: Prefill vs Decode, and Why Batching Works

## Questions

1. Why does autoregressive inference have two distinct computational phases?
2. Why does prefill cost scale with token count, but decode cost doesn't scale with
   batch width?
3. What does "memory-bandwidth-bound" actually mean, and why does it make batching
   free?
4. Why does MockBackend need persistent per-sequence state (SequenceHandle +
   sequences_ map)?
5. Why is panic() the only correct response to an unknown SequenceHandle?
6. Why does tokenize()'s determinism specifically matter?
7. How does this connect to what we already built (Request, RequestLifecycle,
   FakeClock)?

---

## 1. Why does autoregressive inference have two distinct computational phases?

Autoregressive transformer inference is not one uniform operation — it's split into two
fundamentally different computational regimes:

**Prefill** (processing the prompt): the model runs one forward pass over the *entire*
prompt at once. Every prompt token's representation gets computed in parallel through
every transformer layer. This is **compute-bound** — the GPU/CPU is doing genuine, large
matrix multiplications, and the amount of work scales roughly linearly with the number
of prompt tokens. Twice the prompt length ≈ roughly twice the compute.

**Decode** (generating one new token): the model runs one forward pass producing exactly
*one* new token, using the previously-computed KV cache from prefill (and prior decode
steps) so it doesn't have to recompute anything about earlier tokens. This
single-token forward pass is **memory-bandwidth-bound**, not compute-bound — the
bottleneck is *loading the model's weights from memory*, not doing arithmetic on them.

This split is why the scheduler (project.md §8.3) separates work into a "prefill queue"
and a "decode queue" — they have different cost profiles, different resource usage
patterns, and different scheduling strategies.

---

## 2. Why does prefill cost scale with token count, but decode cost doesn't scale with batch width?

**Prefill**: processing 512 prompt tokens requires genuinely doing ~512× more matrix
multiplication than processing 1 token. The arithmetic intensity is real. Our
MockBackend models this as:

```cpp
clock_.advance(per_token_prefill_cost_ * num_tokens);
```

**Decode**: loading the model's weight matrices from memory costs almost the same
whether you're computing one token for one sequence, or one token for 64 sequences
simultaneously. The weights get loaded from DRAM into fast memory *once*, and then
reused across all sequences in the batch in the same pass. The arithmetic (one
matrix-vector multiply per sequence) is tiny relative to the cost of loading the matrix
itself.

This is **the single most important fact in this entire project** — it's *why
continuous batching exists at all*. If decode cost scaled linearly with batch width (the
way prefill cost scales with token count), there would be *no throughput benefit to
batching decode steps together*, and the scheduler's entire reason for existing would
evaporate.

Our MockBackend models this as:

```cpp
// One fixed cost per decode() CALL, regardless of how many sequences are in the batch
clock_.advance(per_decode_cost_);
```

---

## 3. What does "memory-bandwidth-bound" actually mean?

A computation is **compute-bound** when the processor can feed data to the arithmetic
units faster than the arithmetic units can process it — more cores/faster math helps.

A computation is **memory-bandwidth-bound** when the processor's arithmetic units are
idle most of the time, *waiting* for data to arrive from memory. The bottleneck isn't
"how fast can I multiply" but "how fast can I load the matrix I need to multiply with."

For a single-token decode step on a 7B-parameter model: the model's weights are ~14 GB
(in FP16). Each decode step must *read* those weights from DRAM. A typical GPU has
~1–2 TB/s memory bandwidth. So just reading the weights takes ~7–14 ms — and the actual
arithmetic (one matrix-vector multiply per layer, with a vector of size ~4096) takes a
tiny fraction of that time.

**Why this makes batching "free":** you're already spending 7 ms loading the weights
anyway. Whether you multiply that loaded weight matrix against 1 vector (1 sequence) or
64 vectors (64 sequences) barely changes the total time, because the memory load
(the bottleneck) is the same. You get 64× the useful output tokens for essentially
the same wall-clock cost. This is continuous batching's entire economic proposition.

---

## 4. Why does MockBackend need persistent per-sequence state?

In a real backend, when you call `prefill()`, the backend doesn't just return a token —
it **allocates and retains KV-cache memory** for that sequence, tied to that sequence's
identity, and keeps it around across many subsequent `decode()` calls, until you
explicitly `release()` it.

That KV cache *is* the sequence's "memory" of everything generated so far; without it,
every decode step would have to recompute attention over the entire history from
scratch (defeating the entire point of caching).

`SequenceHandle` is a stand-in for "a ticket that refers to this specific sequence's
KV-cache allocation, wherever it physically lives." `MockBackend`'s `sequences_` map
is standing in for "the backend's internal KV-cache table," and `SequenceState`
(tokens emitted so far, optional EOS trigger) is standing in for whatever bookkeeping
a real backend keeps about a sequence's generation progress.

This is why `decode()` must look the handle up by identity in persistent state rather
than recomputing anything from the request each time — it mirrors the real constraint
that a sequence's state genuinely lives somewhere between calls, and you're not allowed
to lose track of it or double-free it (project.md §7: "Cache resources are released
exactly once").

---

## 5. Why is panic() the only correct response to an unknown SequenceHandle?

The **only** way to obtain a valid `SequenceHandle` is as the return value of
`prefill()`. There is no public constructor exposed to callers, no way to fabricate
one, no parsing from a string.

So if `decode()` is ever called with a handle `MockBackend` doesn't recognize, that
can only mean one of two things happened *inside Tokamak's own code*:

1. The caller kept using a handle after already calling `release()` on it — a
   use-after-release bug, the moral equivalent of use-after-free.
2. The caller somehow got a handle from a *different* backend instance and mixed it in.

Neither of these is something a client's request content could ever trigger — they're
purely bugs in our own scheduler/lifecycle bookkeeping. The type system's design
(opaque handle, only mintable by `prefill()`) makes "invalid handle" *structurally*
mean "our own code is broken" — which is precisely the ADR-002 `panic()` category,
not a typed `BackendError`.

---

## 6. Why does tokenize()'s determinism specifically matter?

project.md §13.2's entire reason for deterministic simulation tests: reproducible,
byte-identical results across runs, so a scheduler trace or golden test can assert
exact output.

If `tokenize()` produced different token IDs for the same input text across runs
(e.g., using a hash seeded by wall-clock time, or iteration order over an unordered
container), every downstream test depending on token counts or IDs would become flaky
— nondeterminism introduced at the lowest layer poisons every test built on top of it.

This is the same principle as `FakeClock` needing to be "at least as constrained as
production, never less" (learnings_001) — except here the constraint is "always
produces the same output for the same input," full stop.

---

## 7. How does this connect to what we already built?

Think about how `decode()` gets called: **not once for a whole generation**, but
**repeatedly, once per scheduling iteration**, with the scheduler deciding each time
which sequences get to advance one token.

This is exactly the shape of `RequestLifecycle`'s `DECODING` state in project.md §7's
diagram:

```
DECODING ◀── token emitted ── more tokens ──┘
```

That's a *loop*, not a single call. Every iteration of that loop, for every request
currently in `DECODING`, corresponds to exactly one `DecodeRequest` inside one
`DecodeBatch` passed to `MockBackend::decode()`.

The scheduler (coming in the next milestone step) is the thing that will, on each
iteration, gather up all currently-decoding requests' handles into one `DecodeBatch`,
hand it to the backend once, and distribute the returned tokens back out to each
`Request::emit_token()` call.

`MockBackend`'s per-sequence `tokens_emitted` counter and your
`Request::output_tokens_emitted_` counter are tracking the *same conceptual thing*
from two different vantage points:

- The backend's view: "how much has this KV-cache-tracked sequence generated."
- Tokamak's view: "how much output has this client-facing request received."

They're deliberately redundant, the same way a real system has both a backend-side
generation counter and an application-side output-token accounting — because they're
owned by different subsystems with different responsibilities (project.md §6:
architectural rule about subsystems having narrow contracts and not collapsing into a
single manager class).

---

## 8. Design decision: why EOS configuration is a MockBackend-only method, not in the shared interface

project.md §7's lifecycle shows three distinct terminating conditions:
`EOS / stop / limit`. These are independent:

- **EOS**: the *model itself* decides generation is done (output an end-of-sequence
  token). This is the backend's decision — Tokamak doesn't choose it.
- **stop**: a client-supplied stop sequence is matched in the output. This is
  Tokamak's text-matching logic, not the backend's.
- **limit**: `max_output_tokens` is reached. This is `Request::emit_token()`'s
  `panic()` guard, already built.

For testing, we need MockBackend to be able to simulate EOS ("the model decided to
stop"). But this is a *test-only configuration hook* — no real backend accepts an
externally-forced "stop after N tokens" parameter at the interface level. The real
model's EOS decision emerges from its own weights and sampling, not from an API knob.

Therefore `configure_eos()` is a method on `MockBackend` specifically, not on the
abstract `InferenceBackend` interface — keeping the shared interface clean and matching
project.md §8.4's principle that test hooks shouldn't leak into the narrow shared
contract every backend must implement.

---

## 9. Why panic() inside a noexcept function is safe

`release()` is declared `noexcept` (matching `InferenceBackend`'s interface contract),
yet its implementation calls `panic()` for an invalid handle. This is safe because
`panic()` calls `std::abort()`, which:

- Never throws (it's a C library function that terminates the process immediately).
- Never returns (it's `[[noreturn]]`).

The `noexcept` contract promises "this function will never propagate an exception to
the caller." `std::abort()` satisfies that trivially — the process is gone before any
exception could propagate. This is the same reason `assert()` works inside `noexcept`
functions.

The general principle: any `[[noreturn]]` call path (abort, terminate, infinite loop)
is always compatible with `noexcept`, because the "no exception" promise is vacuously
true when the function never returns at all.

---

## 10. Why tokenize() needs a double-cast through unsigned char

```cpp
static_cast<std::uint32_t>(static_cast<unsigned char>(c))
```

`char`'s signedness is **implementation-defined** in C++. On platforms where `char` is
signed (common on x86), a character like `'\x80'` (value -128 as signed char) would,
if cast directly to `uint32_t`, undergo sign-extension:

```
'\x80' as signed char  →  -128
-128 as uint32_t       →  4294967168  (0xFFFFFF80)
```

That's a completely wrong token ID. The fix is to go through `unsigned char` first,
which reinterprets the bit pattern as 0–255 without sign extension:

```
'\x80' → (unsigned char)128 → (uint32_t)128
```

This is exactly the kind of bug `-Wsign-conversion` exists to catch. The double-cast
is the idiomatic C++ pattern for "treat raw bytes as unsigned values" — you'll see it
in every serious byte-processing library (protobuf, abseil, folly).

---

## 11. Why Duration × std::size_t compiles clean under -Wsign-conversion

```cpp
clock_.advance(per_token_prefill_cost_ * request.prompt.token_ids.size());
```

`Duration` is `std::chrono::steady_clock::duration` (typically `nanoseconds` with a
signed `int64_t` rep). `.size()` returns `std::size_t` (unsigned). We expected
`-Wsign-conversion -Werror` might reject this mixed-signedness multiplication.

It compiles clean because `std::chrono::duration`'s `operator*` is a **function
template** defined in the `<chrono>` system header. Compilers (Clang, GCC) suppress
`-Wsign-conversion` warnings that originate *inside* system headers (those included
with angle brackets or from system include paths) — the diagnostic fires only for
conversions in *your* code, not in instantiated standard-library templates. The actual
signed/unsigned mixing happens inside `<chrono>`'s implementation, not at your call
site, so no warning surfaces.

This is a general principle worth knowing: wrapping a signed/unsigned interaction
inside a standard-library function call often silences the warning, which can be either
convenient (avoiding noisy false positives from well-tested library code) or dangerous
(hiding genuine bugs behind library calls). Here it's fine — `chrono`'s multiplication
semantics are well-defined and won't overflow for realistic token counts.

---

## 12. Why PrefillOutcome implicitly converts to std::expected<PrefillOutcome, BackendError>

```cpp
result.outcomes.push_back(PrefillOutcome{.handle = handle});
```

`result.outcomes` is `std::vector<std::expected<PrefillOutcome, BackendError>>`, yet
we push a raw `PrefillOutcome` without wrapping it. This works because
`std::expected<T, E>` has an **implicit converting constructor** from `T` (when `T` is
not `std::unexpected` and not another `expected` specialization). The standard
deliberately makes the "success path" zero-ceremony — you just hand it the value, and
the implicit conversion wraps it in the expected's engaged (value) state.

The error path requires explicit wrapping: `std::unexpected(BackendError{...})`. This
asymmetry is intentional — success is the common case and should be syntactically
lightweight; errors are exceptional and benefit from being visually marked.

---

## 13. Hidden-friend operator== with = default on SequenceHandle

```cpp
class SequenceHandle {
  friend bool operator==(const SequenceHandle&, const SequenceHandle&) = default;
};
```

Two things happening here:

**Hidden friend:** Declaring `operator==` as a `friend` inside the class body (without
a prior declaration outside) makes it a "hidden friend" — it's only found via
Argument-Dependent Lookup (ADL), not by normal unqualified lookup. This means it won't
participate in overload resolution unless at least one argument is actually a
`SequenceHandle`. Benefit: reduces spurious overload candidates in unrelated code,
slightly faster compilation, and clearly scopes the operator to this type only.

**`= default`:** The compiler generates a memberwise comparison (here, comparing
`id_` values directly). Combined with C++20's rewrite rules, this also implicitly
provides `operator!=` — no need to define it separately.

This is the modern C++ idiom for "value-equality on an opaque wrapper type" — minimal
boilerplate, correct by construction, and you'll likely reuse it for any future
handle/ID types in the project.

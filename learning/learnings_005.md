# Learnings 005 — Continuous Batching Scheduler Design: Real-World Patterns

## Questions

1. Within one scheduling tick, should a freshly-prefilled request decode in the same
   tick, or wait until the next one?
2. What happens to a `SequenceHandle` after `decode()` reports a `BackendError` for it
   — does the backend consider it already cleaned up?
3. What is the actual problem "unbounded batches" defers, and why does it matter?
4. How do real schedulers (vLLM et al.) solve unbounded batch growth?
5. Why does "never waste an iteration" matter specifically for TTFT?
6. What is chunked prefill, and what starvation problem does it solve?

---

## 1. Same-tick vs. next-tick decode pickup

`RequestState` has both `WaitingDecode` and `Decoding` as distinct states (project.md
§7) — modeling "prefill finished, but not yet picked up by a decode step" as a real,
separate moment. The open design question: does a request promoted out of prefill this
tick also get its first decode token *this same tick*, or does it sit in
`WaitingDecode` until the next tick's decode phase runs?

Two options:

- **Next-tick parking**: `WaitingDecode` is a real state a request rests in across a
  tick boundary. Cleaner to reason about in isolation (decode phase only ever touches
  "last tick's decoding set"), but costs one full tick of pure, avoidable latency
  before the first token — directly inflating TTFT for no structural reason.
- **Same-tick promotion**: prefill phase promotes `WaitingPrefill → ... → Decoding`
  fully within the tick it succeeds; the decode phase (running right after, same tick)
  then includes both newly-promoted and already-decoding requests in one batch call.
  `WaitingDecode` still exists conceptually (matches the lifecycle diagram, useful for
  tracing/observability) but a request never actually rests there across a tick
  boundary.

**Resolved: same-tick promotion.** Real continuous-batching schedulers (vLLM, TGI,
TensorRT-LLM) are built around one governing principle: **never waste an iteration**.
If useful work (a freshly-prefilled sequence ready to decode) is available *this* tick,
deferring it to the next tick for no structural reason is exactly the kind of waste
these systems are designed to eliminate. Since TTFT is project.md's own headline metric
(§5), and every tick spent idle in `WaitingDecode` is directly, 1:1, added TTFT
latency, the "cleaner to test" option is actually the *worse* engineering choice here —
worth explicitly noticing that "easier to reason about in isolation" and "the right
design" are not always the same thing.

This means our cost model still holds exactly one `prefill()` call and one `decode()`
call per tick (matching the flat-per-call decode cost from learnings_004) — the
same-tick promotion doesn't add extra backend calls, it just changes which requests are
eligible to be included in the one decode() call that tick.

---

## 2. What happens to a SequenceHandle after a BackendError?

Neither `backend.h` nor `mock_backend.h` originally specified this. If `decode()`
reports `BackendError` for one sequence in a batch (a real, backend-side failure — not
an invalid-handle `panic()` case), does the scheduler still owe the backend a
`release()` call for that handle?

Real backends answer this by keeping resource-management concerns **structurally
separate from compute-result reporting**. KV-cache/context managers (vLLM's
`BlockManager`, llama.cpp's context) are owned and freed by an explicit, separate call
— a failed compute step for a sequence does not implicitly mean its cache allocation
was already torn down. Conflating "it failed" with "it must already be cleaned up" is
a classic source of two opposite bugs: resource leaks (nobody ever calls release, so
the backend statically believes the sequence is still live) or double-frees (something
else also tries to free it, wrongly assuming it wasn't already freed).

**Resolved: the scheduler must always call `release()` exactly once for every
`SequenceHandle` it ever obtained, on every retirement path** — success, `BackendError`,
cancellation, or deadline expiry, with no exceptions. Error reporting and resource
cleanup are orthogonal concerns, and the safe, universal invariant is symmetry: every
`prefill()` outcome that returns a handle is matched by exactly one later `release()`
call, unconditionally. See ADR-007 for the formal contract amendment.

---

## 3. What "unbounded batches" actually defers

We deliberately scoped the FIFO scheduler to "unbounded batches" — every waiting
request gets prefilled, every decoding request gets decoded, every tick, no caps. This
is fine at Milestone 1's toy scale (dozens of synthetic requests against MockBackend,
which has no real memory constraint), but it's worth being honest about what this
defers rather than treating it as a permanently-fine simplification.

In a long-running real server, `Decoding` only ever grows — new admissions keep joining
it, and nothing currently says "no, not yet." Every sequence in `Decoding` holds real
KV-cache memory (finite GPU DRAM). Unbounded growth eventually hits a hard physical
wall: no more memory for the *next* prefill. This is exactly why project.md §8.3 lists
"available KV-cache pages" as a batch constraint, and why §8.5 (KV-Cache Policy Layer)
exists as an entire subsystem — it's the thing that turns "unbounded" into "bounded by
physical memory, gracefully," rather than "unbounded until it crashes."

**The concrete limitation**: our FIFO scheduler's unbounded-batch design is only
correct because MockBackend has no real memory to exhaust. The moment a real backend
(llama.cpp, Milestone 2) is introduced, this design needs revisiting — this is a
genuine, named boundary of Milestone 1's scope, not an oversight.

---

## 4. How real schedulers solve unbounded growth: preemption

vLLM's answer — and the general pattern across serious inference engines — is
**preemption**: when the scheduler can't fit a new prefill due to KV-cache exhaustion,
it can forcibly evict an already-running decode sequence out of the active batch,
via one of two strategies:

- **Swapping**: copy the evicted sequence's KV-cache out to CPU memory, freeing GPU
  pages; swap it back in later when capacity frees up.
- **Recomputation**: drop the KV-cache entirely; when the sequence is rescheduled,
  redo its prefill from scratch, paying the compute cost again but reclaiming memory
  immediately with no swap I/O.

The trade-off: recomputation is simpler (no swap I/O, no memory-management complexity)
but wastes compute; swapping preserves compute investment but costs I/O bandwidth and
adds complexity. Which wins depends on how cheap recompute is relative to swap
bandwidth — short sequences usually favor recompute; very long contexts usually favor
swapping. Real systems implement both and choose per-situation.

**Connection to what we already built**: preemption is the scheduling-level analog of
`RequestState`'s `WaitingDecode`/`WaitingPrefill`/`Decoding` split — it's just "a
request that already reached `Decoding` gets forcibly pushed back to a waiting state."
Nothing in `is_valid_transition()` currently forbids this direction of transition —
the state machine already structurally permits a future preemption-aware policy to be
built on top of it; we simply don't build the triggering logic yet.

---

## 5. Why "never waste an iteration" matters for TTFT specifically

This isn't abstract elegance — TTFT is project.md's literal, named headline metric
(§5). Every tick a finished-prefill request spends idle in `WaitingDecode` before its
first decode step is directly, 1:1, added TTFT latency.

This reveals a subtlety worth internalizing: the state machine having an intermediate
state (`WaitingDecode`) and the scheduler minimizing time spent in that state are two
separate, independently-tunable concerns. The state exists for correctness and
observability — you can always ask "what state is this request in right now," which
matters for tracing/debugging. A good scheduler's entire job is making the actual
time-in-that-state as close to zero as physically possible. Having the state doesn't
mean requests should linger in it.

---

## 6. Chunked prefill and the starvation problem it solves

A related, more advanced technique worth knowing: some schedulers (vLLM's "chunked
prefill") deliberately break one large prefill into smaller pieces across multiple
ticks, interleaved with decode steps for other sequences, rather than running the
entire prefill in one shot.

**Why this matters**: prefill cost scales with prompt token count (compute-bound, per
learnings_004). A single very long prompt's prefill can take long enough, in one
uninterrupted shot, to block decode progress for every other already-running sequence
for that entire duration — starving their TTFT/inter-token-latency. Chunking trades
"this one big prompt finishes prefill slightly later" for "everyone else doesn't stall
behind it" — a direct, concrete mechanism solving the exact fairness requirement named
in §8.3: "long prompts cannot permanently starve short prompts."

This is explicitly out of scope for the FIFO baseline — FIFO is deliberately *not*
fair by construction ("oldest first" is its entire definition) — but it's the real
tool later policies (deadline-aware, adaptive) will need in their box to actually
satisfy §8.3's fairness requirements.

---

## 7. Summary: what Milestone 1's FIFO scheduler explicitly does and doesn't solve

| Concern | Milestone 1 FIFO scope |
|---|---|
| Ordering | Oldest-submitted-first, no reordering by priority/deadline |
| Batch size | Unbounded — no KV-cache/memory constraint exists yet |
| Prefill→decode latency | Minimized via same-tick promotion |
| Resource cleanup | Universal, unconditional `release()` on every retirement path |
| Preemption | Not implemented — no memory pressure to trigger it against |
| Starvation prevention (chunked prefill, fairness) | Not implemented — FIFO is explicitly unfair by design |

Each "not implemented" row names a real subsystem or technique that later milestones
(KV-cache policy, deadline-aware/adaptive scheduling) will need to introduce — this
table is a map of what's deliberately deferred, not what's missing by accident.

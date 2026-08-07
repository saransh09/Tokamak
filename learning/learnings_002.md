# Learnings 002 — Request Lifecycle State Machine: Why It Exists

## Questions

1. What is a state machine, formally?
2. Why don't typical Python API projects need this explicitly?
3. Why enforce it so strictly (abort on invalid transition) instead of Python's usual
   "fail this one request, move on"?
4. What design philosophy does this connect to?
5. So we're going one step beyond typical web API projects — why exactly?

---

## 1. What is a state machine, formally?

Computer science has a precise name for what we built: a **finite state machine (FSM)**
— a classical concept from automata theory, defined by:

- a finite set of **states** (our `RequestState` enum),
- a set of **transitions** (our `is_valid_transition()` table) saying which state
  changes are legal,
- an **initial state** (`kReceived`),
- one or more **terminal/accepting states** (`kCompleted`, `kRejected`, `kCancelled`,
  `kFailed`).

There's a further distinction worth knowing: a **Moore machine** produces output based
only on the *current* state; a **Mealy machine** produces output *as a side effect of a
transition*. Our `RequestLifecycle` is Mealy-style — recording `admitted_at`/
`completed_at` happens *during* the transition, not just by inspecting the current
state. This isn't just trivia — it's why the state machine needed the `Clock` we built
first: transitions are the only place we can correctly timestamp "when did this actually
happen."

---

## 2. Why don't typical Python API projects need this explicitly?

They usually do have an implicit state machine — it's just not enforced, and it's easy
to miss because of *how* Python projects are usually structured. Two separate reasons:

### (a) The request lifetime maps onto a single function call

In a typical FastAPI/Flask/Django view:

```python
def handle_order(request):
    order = Order.objects.get(id=request.order_id)
    order.status = "shipped"
    order.save()
    return response
```

The entire "lifecycle" of handling this HTTP request happens **inside one call stack, on
one thread, start to finish, synchronously (from the caller's perspective).** The
"state" of the request *is* the current line of code / call-stack position. There's no
need for an explicit state variable because the program counter already encodes it —
when the function returns, the request is simply over.

**Tokamak's requests do not work this way.** A request is admitted, then sits in a queue
for an *unknown* amount of time, then gets partially processed (prefill), then sits
again, then gets decoded incrementally across *many separate scheduler iterations*,
interleaved with potentially hundreds of other requests — and might get cancelled from
an entirely different thread while it's sitting in a queue, seconds or minutes later.
**There is no single call stack frame that represents "this request" for its whole
life.** The request's state has to be stored explicitly, as data, because nothing in the
call stack encodes it. This is the core reason: our problem is fundamentally about
**managing long-lived, resumable, concurrently-touched entities**, not "handle one
request start-to-finish, then forget it."

This is closely related to a concept from async/continuation-based programming: in
ordinary synchronous code, "what happens next" is implicit (the next line). In an
event-driven scheduler like ours — where a worker thread might handle a *different*
request's next step on its next iteration — "what happens next" for any given request
has to be looked up from explicit stored state. The state machine *is* that lookup
table.

### (b) When Python projects *do* have long-lived entities, they usually delegate the state machine elsewhere — and often don't enforce it

Think of a Django `Order` model with a `status` field (`pending`, `paid`, `shipped`,
`delivered`). That's a real state machine! But in a typical codebase, it's just a
`CharField` with `choices=...` — nothing stops *any* code path from doing
`order.status = "shipped"` while it's still `"pending"`, skipping payment entirely.
This is a very common real bug class in production CRUD apps — "impossible" states leak
through because there's no enforcement, only convention.

The ecosystem actually recognizes this is a real, general problem — hence tools like
**Django-FSM**, **Temporal**, **AWS Step Functions**, and Celery workflow patterns, which
exist specifically to formalize "objects that move through stages over time, must not
skip steps, and must be safely resumable." What we just built for `RequestLifecycle` is
a tiny, purpose-built, in-process version of exactly that category of tool — we can't
use something like Temporal here because it talks to an external orchestration service
over the network, and our scheduler needs to make thousands of decisions per second with
microsecond-level overhead. So we hand-roll the minimum viable version, in-process, with
zero I/O.

---

## 3. Why enforce it so strictly (abort on invalid transition) instead of Python's usual "fail this one request, move on"?

This is really a question about **blast radius**, and it's one of the sharpest
differences between "web API script" thinking and "systems/server" thinking:

- In a typical Python request handler, if something goes wrong mid-request, the
  exception propagates, the framework returns a 500, the request dies, and — critically
  — **everything about that request is thrown away and garbage-collected.** The next
  request starts completely fresh. The damage is contained to one request.

- In Tokamak, a request's state is entangled with **shared, long-lived resources** other
  concurrent requests depend on: KV-cache pages, scheduler queues, batch slots. If a
  request's state silently becomes corrupted (e.g., something thinks it's still
  `Decoding` when it already completed), the bug doesn't just affect that one request —
  it can leak a cache page forever, double-free a resource, or leave the scheduler
  holding a stale reference that only manifests as a crash *much later*, far from the
  original mistake, possibly under a completely different request. This is exactly the
  "invariant violation" project.md's Section 14 is worried about — and it's why we chose
  "abort immediately, loudly, at the exact moment the violation happens" over "silently
  continue and hope."

Python's "one bad request just 500s" safety net exists because the *architecture*
isolates requests from each other. Our architecture explicitly does **not** isolate
requests from each other (they share cache pages, batch slots, scheduler queues by
design — that's the entire point of continuous batching!). So we have to build the
safety net ourselves, at the state-machine layer, rather than relying on
process/request isolation to contain mistakes.

---

## 4. A design philosophy this connects to: "make illegal states unrepresentable"

This is a known phrase in software design (associated with Yaron Minsky and the
OCaml/functional-programming world, and echoed a lot in Rust culture too). The idea:
rather than writing code that *checks* for bad states after the fact, design your types
so bad states **can't be constructed in the first place**. Our `enum class RequestState`
+ exhaustive `is_valid_transition()` table is a concrete, testable instance of this:
instead of trusting every call site in the codebase to "remember" not to skip a step,
we centralize the rule in one function that can be — and was — exhaustively unit tested
(literally every one of the 10x10 `(from, to)` pairs is checkable, which is a level of
completeness you basically can't get when the "rule" is scattered implicitly across
dozens of Django view functions).

---

## 5. So, to directly answer the framing: why go beyond what Python projects do?

Yes — this is one deliberate step beyond what most Python API projects do, and it's not
incidental complexity. It's a direct consequence of three things converging:

1. Requests are **long-lived and resumable**, not one-shot call-stack-scoped.
2. Requests **share mutable resources** with other concurrent requests (no isolation /
   blast-radius containment).
3. We're writing the **scheduler/runtime itself**, not calling someone else's
   already-hardened one — so nobody upstream is going to catch these mistakes for us.

This is genuinely closer to how real production LLM-serving systems (vLLM, TensorRT-LLM)
and other long-lived stateful server systems reason about request lifecycles internally,
even though most of them are implemented in less rigidly-typed ways than what we're
doing here in C++.

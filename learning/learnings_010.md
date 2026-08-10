# Learnings 010 — Thread-Safe Bounded Buffers: Theory Behind TokenChannel

## Questions

1. What makes a shared buffer "thread-safe" — what specific hazards does a
   mutex protect against here?
2. Why does `pop()` use `std::unique_lock` + `condition_variable::wait()`
   while `try_push()` uses `std::lock_guard` — what's the mechanical
   difference, and why does it matter which one each method uses?
3. Why is the wait predicate `!buffer_.empty() || closed_` and not just
   `!buffer_.empty()` — what would go wrong with the simpler version?
4. Why does `try_push()` call `notify_one()` while `close()` calls
   `notify_all()`?
5. What does "bounded" actually buy us compared to an unbounded queue,
   beyond the raw memory-growth argument already covered in learnings_009?
6. Why is `mutex_` declared `mutable` on a member that otherwise looks like
   it should just be a normal private field?
7. `TokenChannel` could in principle be lock-free instead of mutex-based —
   why didn't we build it that way?

---

## 1. The mutex's job: making `buffer_` and `closed_` look atomic from outside

`TokenChannel` has exactly two threads touching it in its intended use: one
producer (the engine thread, calling `try_push`) and one consumer (a
connection coroutine, calling `pop`). Without synchronization, two real
hazards exist even with just two threads:

- **Data races on `std::deque`'s internals.** `push_back` and `pop_front`
  each read and mutate multiple internal pointers/counters inside the
  `deque`. If one thread is mid-`push_back` (say, it has resized an internal
  chunk but not yet updated the size counter) while another thread calls
  `pop_front` concurrently, the deque's internal state is observed
  half-updated — this is undefined behavior in C++, not just "might return a
  weird value." The mutex's real job is preventing any two of `try_push`,
  `pop`, `close`, `is_closed`, `size` from executing their bodies
  concurrently, so `buffer_` is only ever observed in a state some *single*
  method left it in, never mid-mutation.
- **Torn reads of `closed_`.** A bare `bool` written by one thread and read
  by another without synchronization is also a data race in the C++ memory
  model (even though on most real hardware a single bool write is
  practically atomic) — and more importantly, without a mutex or atomic,
  there's no guarantee *when* (or if) the writing thread's change becomes
  visible to the reading thread at all. The mutex gives us not just "no
  torn reads" but a real *happens-before* relationship: everything the
  producer did before calling `close()` (while holding the lock) is
  guaranteed visible to the consumer once it acquires the same lock inside
  `pop()`.

**Takeaway**: "thread-safe" here specifically means: every operation that
reads or writes `buffer_` or `closed_` does so while holding `mutex_`, so
the compiler and hardware are prevented from doing anything that would let
one thread observe another's write mid-progress, and the mutex's
acquire/release pattern gives us a defined ordering guarantee between
threads, not just "no crash."

---

## 2. `lock_guard` vs `unique_lock`: only one of them can unlock itself early

Both are RAII wrappers around a mutex — construct to lock, destruct to
unlock, exception-safe either way. The difference that actually matters
here: `std::lock_guard` locks once at construction and unlocks once at
destruction, with no methods to do anything else. `std::unique_lock` adds
`lock()` / `unlock()` / `try_lock()` — it can release and reacquire the
mutex *during its own lifetime*, not just at the two RAII endpoints.

`condition_variable::wait(lock, predicate)` needs exactly that extra
capability, because of what it has to do internally, roughly:

```cpp
while (!predicate()) {
    // must UNLOCK mutex here, so the producer can acquire it to push
    // ... sleep until notified ...
    // must RE-LOCK mutex here, before predicate() runs again
}
```

If the consumer kept the mutex locked while sleeping, no producer could
ever call `try_push` (it would block forever trying to acquire the same
mutex) — the consumer would be waiting for a push that can never happen,
because it's the one holding the lock the push needs. `wait()` solving this
requires a lock type it can manipulate mid-flight, which is exactly what
`lock_guard` refuses to expose (deliberately — it's meant to be the
simplest possible "lock for this scope" tool, with no room for misuse).

`try_push()` never waits — it either succeeds or returns `false`
immediately, all within one uninterrupted critical section — so it has no
need for `unique_lock`'s extra flexibility, and `lock_guard` is the
correctly minimal tool for that job.

**Takeaway**: pick `lock_guard` when a method's entire body is "lock, do
some work, unlock" with no waiting in between; reach for `unique_lock` only
when something inside the critical section (almost always a
`condition_variable::wait`) needs to unlock and relock the mutex itself.
Using `unique_lock` everywhere "to be safe" isn't wrong, but it obscures
which methods actually need that extra power — `lock_guard` on `try_push`
is documentation, not just a smaller type.

---

## 3. The predicate has to cover every condition that should end the wait — not just the happy one

`non_empty_cv_.wait(lock, [this] { return !buffer_.empty() || closed_; })`
is shorthand for "keep sleeping until this predicate is true," and the
predicate is evaluated every time the consumer is woken (see Learning 6 in
learnings_009's spirit — the resuming thread must recheck reality, not
assume the reason it was woken is the reason it should stop waiting).

If the predicate were just `!buffer_.empty()`, consider what happens when
the channel is closed while empty and no more pushes will ever come (this
is exactly the shutdown / request-completion path): `close()` calls
`notify_all()`, the consumer wakes up, re-checks `!buffer_.empty()` — which
is still `false`, since closing doesn't add anything to the buffer — and
goes right back to sleep. Forever. No one will ever call `try_push` again
(the request is over), so no future notification will ever arrive either.
The consumer coroutine (in the real HTTP handler, not just this unit test)
would hang indefinitely on a completed request, which is precisely the kind
of resource leak Section 8.1's disconnect/cleanup requirements exist to
prevent.

Adding `|| closed_` to the predicate means "stop waiting if there's
something to read, **or** if there will never be anything to read again" —
both are legitimate reasons to stop blocking, and `pop()`'s body already
handles both correctly (empty-and-closed returns `nullopt`, non-empty
returns a value regardless of `closed_`).

**Takeaway**: a wait predicate must enumerate every condition under which
waiting should end, not just the condition that names the method's primary
purpose. "Wait for data" sounds like `!empty()` is the whole story, but the
real requirement is "wait for data, or proof that no more data is coming" —
missing the second half turns a clean shutdown path into a permanent hang.

---

## 4. `notify_one` says "one thing can proceed"; `notify_all` says "everything must find out"

`std::condition_variable::notify_one()` wakes at most one waiting thread;
`notify_all()` wakes every waiting thread (each of which then re-checks its
own predicate and may go straight back to sleep if it's still false — waking
up is not a promise of proceeding).

`try_push()` adds exactly one `TokenEvent` to the buffer. In the intended
SPSC usage, there's only one consumer anyway, so the distinction is moot in
practice — but stating the *intent* correctly still matters: one unit of
work became available, so waking one waiter is the precise action, and it
avoids the (here, purely theoretical, but real in a general MPSC version of
this class) cost of waking N consumers when only one of them can actually
get the new item, the rest waking up, finding the buffer already drained by
whichever consumer won the race, and going back to sleep for nothing.

`close()` is different in kind, not degree: it's a permanent state
transition every current and future waiter needs to observe, not a single
unit of work being handed to whichever consumer gets there first. Using
`notify_one()` here would risk exactly one waiter (if multiple existed)
waking up and seeing `closed_ == true`, while a second waiter never gets
notified at all and sleeps forever — the same class of hang as Learning 3,
just introduced via the wrong notify call instead of the wrong predicate.
`notify_all()` is the only correct choice for a state change that must be
visible to every current waiter, not a claim exactly one of them should
win.

**Takeaway**: `notify_one` vs `notify_all` should be chosen by asking "how
many waiters does this event actually satisfy?" — one new item satisfies
one waiter's predicate (usually), but a terminal state change like `closed_
= true` satisfies (and must reach) every waiter's predicate simultaneously.

---

## 5. Bounded capacity turns backpressure into a decision point instead of a latent failure

learnings_009 (§3) already covered *why* the channel is bounded — an
unbounded queue converts a slow-consumer problem from an explicit, timed
event into an unattributable future OOM. The piece worth adding here, now
that the class actually exists and is tested: boundedness is what makes
`try_push`'s `bool` return value *meaningful* in the first place.

An unbounded `push()` has nothing useful to return — it always succeeds
(until the whole process runs out of memory, which isn't a per-call
condition it could report anyway). Because `TokenChannel` is bounded,
`try_push` returning `false` is a real, immediate, per-call signal: "the
consumer isn't keeping up, right now, by this specific measurable amount
(the configured capacity)." That signal is what Piece B's engine thread
will use to start a backpressure timer and eventually cancel a request —
none of that logic has anywhere to attach if the push operation can't ever
fail. The bound isn't just a safety limit; it's the mechanism that produces
the one piece of information ("full or not") the entire backpressure design
in ADR-011 depends on.

**Takeaway**: a bounded buffer doesn't just cap worst-case memory — it
manufactures a boolean signal (full/not-full) at exactly the place where a
system needs to make a real-time decision about a slow consumer. An
unbounded buffer has no equivalent signal to give you, at any capacity,
ever — which is a second, independent reason (beyond memory) that
"bounded" was the right choice, not merely a more cautious one.

---

## 6. `mutable` reconciles "logically read-only" with "mechanically must lock"

`const` on a member function is a promise to callers: "calling this will
not change any state you can observe through this object's public
interface." `size()` and `is_closed()` keep that promise — calling them
twice in a row without any intervening `try_push`/`pop`/`close` call
returns the same answer both times, from the caller's point of view.

But *mechanically*, both methods must lock `mutex_` to safely read
`buffer_.size()` or `closed_` (per Learning 1 — reading without the lock is
itself a data race, even for a "just reading" operation, since it could
race a concurrent write). Locking a mutex changes the mutex's own internal
state (it flips from unlocked to locked and back). A member function marked
`const` normally cannot call any non-const method on a member — attempting
`mutex_.lock()` inside a `const` method would fail to compile, because
`mutex_` is treated as `const std::mutex` inside that method's body, and
`lock()` is (correctly) not a `const` method on `std::mutex`.

`mutable` on `mutex_` (and on `non_empty_cv_`, similarly, if it were ever
touched from a const method) tells the compiler: this specific member is
exempt from the enclosing method's `const`-ness — it may be mutated even
when called through a `const` object or from a `const` method, because its
mutation is an implementation detail of providing safe access, not a change
to the object's logical state. This is the standard, narrow, well-understood
use of `mutable`: synchronization primitives (mutexes, and sometimes caches)
that back otherwise-const observer methods.

**Takeaway**: `mutable` is not a general escape hatch from `const`-correctness
— it's specifically for members whose mutation is invisible to the object's
logical state from the outside (a lock's locked/unlocked flag isn't part of
what `size()` conceptually returns) but is mechanically required to safely
compute that logical state. Reaching for `mutable` on anything else (e.g. a
cache whose staleness *is* observable, or a counter that changes the
answer) would be misusing the keyword to lie about constness rather than to
correctly express it.

---

## 7. Lock-free was available; a blocking consumer makes it not worth the cost here

A lock-free SPSC ring buffer (fixed-size array, atomic head/tail indices,
`memory_order_acquire`/`release` on the right operations) is a real,
well-established alternative — it avoids the mutex entirely, and `push`/
`pop` become wait-free (bounded number of instructions, no possibility of
blocking on another thread).

It wasn't chosen here for three concrete reasons, not just "mutexes are
simpler":

- **The consumer needs to block, not spin.** `pop()`'s contract is "wait
  until a token is available or the channel closes" — a lock-free ring
  buffer's `pop` is normally non-blocking (returns "empty" immediately if
  there's nothing), which pushes the "what do I do while waiting" question
  onto the caller. The caller would then need *some* blocking/notification
  mechanism anyway (a semaphore, an eventfd, or... a condition variable) —
  at which point most of the complexity `condition_variable` was meant to
  avoid comes right back, just relocated.
- **Contention is negligible for this workload.** One push per generated
  token per tick (tens of milliseconds apart, at fastest), one pop per SSE
  frame write. This is nowhere near a hot loop where mutex acquisition
  overhead (roughly tens of nanoseconds when uncontended) would show up in
  a profile. Lock-free structures earn their complexity when contention is
  real and measured — introducing one here would be optimizing a cost that
  doesn't exist yet.
- **Section 16 sets an explicit bar for lock-free code**: "Use lock-free
  structures only with a benchmark and a written memory-ordering argument."
  No benchmark exists showing the mutex is a bottleneck, so reaching for
  lock-free now would violate that guideline's intent even if the code
  happened to be correct.

The public interface (`try_push` / `pop` / `close`) doesn't expose the
mutex or any implementation detail — so if a future profiling pass ever did
justify a lock-free rewrite, it would be an internal change behind this
same boundary, not a redesign of every caller.

**Takeaway**: "lock-free is faster" is only a reason to use it once
contention is real and measured, and once the consumer-side blocking
problem it doesn't solve has a real answer that's actually simpler than a
condition variable — neither was true here, so the mutex-based design is
the right amount of mechanism for the actual requirement, not a placeholder
for something better later.

---

## Summary: theory behind `TokenChannel`'s specific design choices

| Concern | What we learned |
|---|---|
| What the mutex actually protects | Not just "no crash" — it prevents observing `buffer_`/`closed_` mid-mutation and establishes a real happens-before ordering between producer and consumer threads |
| `lock_guard` vs `unique_lock` | Use `unique_lock` only where something inside the critical section (a `condition_variable::wait`) needs to unlock/relock itself; `lock_guard` on `try_push` documents that it never waits |
| The wait predicate | Must name every condition that should end waiting, not just the "happy path" one — omitting `\|\| closed_` turns a clean shutdown into a permanent hang |
| `notify_one` vs `notify_all` | Choose by asking how many waiters a given event actually satisfies — one new item vs. a terminal state change every waiter must observe |
| Why bounded, beyond memory | A bound is what makes `try_push`'s boolean return value meaningful at all — it manufactures the full/not-full signal the entire backpressure design depends on |
| `mutable` on `mutex_` | Reconciles "logically read-only from the caller's view" with "mechanically must lock to safely compute that view" — a narrow, correct use of the keyword, not a general const-escape |
| Why not lock-free | The consumer's blocking requirement doesn't go away with lock-free (it relocates), contention here is negligible, and Section 16 requires a benchmark + memory-ordering argument neither of which exists to justify it yet |

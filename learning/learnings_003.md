# Learnings 003 — Member-Initializer Lists and the "No Call Operator" Error

## Questions

1. What is a "member-initializer list," really?
2. What actually went wrong in your code?
3. A related concept worth knowing: initialization order
4. Why call-operator errors specifically look confusing

---

## 1. What is a "member-initializer list," really?

In C++, a constructor has two separate places where "setting up member variables" can
happen:

```cpp
Request::Request(RequestId id, const Clock& clock, ...)
    : id_(std::move(id)),      // <-- member-initializer list
      lifecycle_(clock),
      deadline_at_(clock.now() + deadline_from_now)
{
    // <-- constructor body (empty here, but could contain more logic)
}
```

The part after `:` and before `{` is the **member-initializer list**. It's a list of
separate initializations, one per member, each written as
`member_name(value_to_initialize_with)`. Crucially: **each `member_(...)` is its own
independent clause**, and they're separated by commas at the *top level* — commas here
mean "next initializer," not "next function argument."

This is subtly different from a normal function call like `foo(a, b, c)`, where commas
separate arguments *to the same call*. The member-init-list looks similar (parentheses,
commas) but means something structurally different: it's a sequence of `name(args)`
mini-constructor-calls, chained by commas, each operating on a *different* member.

---

## 2. What actually went wrong in your code

```cpp
id_(std::move(id), lifecycle_(clock), deadline_at_(clock.now() + deadline_from_now), ...)
```

Because everything is inside **one set of parentheses** attached to `id_`, the compiler
parses this as: *"call `id_` as if it were a callable/function, passing these five things
as arguments to that call."* That's exactly the "does not provide a call operator" error
— `id_` is a `std::string`, and `std::string` doesn't have `operator()` either, but more
importantly, `lifecycle_(clock)` *inside* those parens gets parsed as its own
sub-expression: "call `lifecycle_` (a `RequestLifecycle` object) like a function, passing
`clock`." Since `RequestLifecycle` has no `operator()`, that's the specific error you
saw.

The fix — closing `id_`'s parens immediately, then a comma, then starting a *new*
top-level initializer — tells the compiler "these are five separate member
initializations," not "one call with five arguments."

---

## 3. A related concept worth knowing: initialization order

One subtlety that bites people even after they fix the syntax: **members are initialized
in the order they're *declared* in the class, not the order they appear in the
initializer list.** So even though you might write:

```cpp
: priority_(priority), id_(std::move(id)), lifecycle_(clock) ...
```

they will actually be initialized in whatever order they're declared inside the class
body (`id_`, then `lifecycle_`, then `deadline_at_`, etc., per your `private:` section).
Most compilers (including Clang) will warn you (`-Wreorder`) if your initializer-list
order doesn't match declaration order, specifically because it's a common source of
confusion — and since we compile with `-Werror`, that warning would become a hard build
failure, which is actually a nice safety net here.

This matters practically: if member B's initializer depended on member A already being
initialized, but A is declared *after* B in the class, you'd get silently-wrong values
(using A before it's set up) rather than an error — which is exactly the kind of subtle
bug class this ordering rule exists to warn you about.

---

## 4. Why call-operator errors specifically look confusing

`operator()` — the "call operator" — is what makes an object "callable," i.e., usable as
`my_object(args)`, like a function. C++ allows any class to overload this (these are
called "functors" or "function objects" — used a lot in the STL, e.g., custom comparators
for `std::sort`). Clang's error is really just saying: *"you wrote something that looks
like 'call this object,' but this type never defined what that means."* Recognizing "oh,
this parenthesized-thing-after-an-identifier is being interpreted as a function call, not
an initializer" is the key debugging insight here — and it's a good pattern-match to keep
for future constructor bugs.

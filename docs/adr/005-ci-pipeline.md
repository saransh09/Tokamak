# ADR-005: CI pipeline via GitHub Actions

## Status
Accepted

## Date
2026-08-08

## Context

project.md Milestone 0 explicitly lists "CI for Linux release and sanitizer builds" as
a deliverable, with exit criteria requiring "CI produces reproducible binaries or build
logs" and "Sanitizer smoke tests pass." The repository already has three CMake presets
(`dev`, `sanitizer`, `release`) defined in `CMakePresets.json`, anticipating exactly
this — the presets encode all build configuration (flags, sanitizer instrumentation,
binary output directories), so the CI workflow only needs to invoke them, not
reinvent build logic.

The repository is hosted on GitHub (`saransh09/Tokamax`). No CI has existed to date;
all build verification has been manual (`cmake --build --preset dev` locally on macOS).
This leaves the project vulnerable to: commits that break the build on Linux (different
compiler behavior, missing headers), regressions caught only by `-Werror` that someone
forgets to run locally, memory/UB bugs that only surface under sanitizer
instrumentation, and test failures introduced without being noticed until much later.

### Options considered

**A. GitHub Actions.** Native to the hosting platform, zero external account setup,
free for public/private repos at this scale, directly integrates with PR status checks.
Workflow files live in the repository (`.github/workflows/`), versioned alongside the
code they build. Preinstalled runners include recent GCC and Clang on Ubuntu.

**B. CircleCI / Travis CI.** External service requiring separate account, webhook
configuration, and credential management. No meaningful advantage for a single-repo
C++ project already on GitHub. Adds operational surface area with no offsetting benefit.

**C. Self-hosted runner / Jenkins.** Useful when proprietary hardware (GPU, FPGA) is
required for CI. Not needed at this stage — all current tests are CPU-only, no model
weights, no GPU. Can be added later (Milestone 2+) when real-backend integration tests
need hardware access.

### Scope decisions

**Two jobs, not one.** The `dev` preset (standard debug build with `-Werror`) and the
`sanitizer` preset (ASan + UBSan) serve different purposes: the first catches compile
errors and warnings-as-errors; the second catches memory corruption, undefined behavior,
and leaks that compile and run correctly but are latently broken. Combining them into
one job would mean a sanitizer failure blocks feedback on whether the code even compiles
— separating them gives faster, independent signal.

**ThreadSanitizer deferred.** TSan requires a separate build (cannot combine with ASan
— mutually exclusive instrumentation). No `tsan` CMake preset exists yet, and there is
no concurrent code in the project to stress-test (all current code is single-threaded
deterministic simulation). A TSan job will be added when the scheduler introduces real
concurrency (Milestone 1's simulation runner or Milestone 2's async HTTP frontend).

**No clang-format / clang-tidy job yet.** Neither `.clang-format` nor `.clang-tidy`
configuration files exist in the repository. Adding a CI format-check without first
establishing the formatting rules would be premature. This is a separate Milestone 0
deliverable ("Formatting and static-analysis configuration") to be addressed
independently.

**Trigger: push to main + all pull requests.** Every commit on `main` (including direct
pushes and merged PRs) and every PR targeting any branch gets both jobs. This ensures
no code reaches `main` without passing, while also catching breakage introduced by
direct pushes (which skip the PR gate). This is the standard, uncontroversial default
for small teams.

## Decision

Use GitHub Actions with a single workflow file (`.github/workflows/ci.yml`) containing
two independent jobs:

1. **`build-and-test`**: checkout → `cmake --preset dev` → `cmake --build --preset dev`
   → `ctest --preset dev`. Runs on `ubuntu-latest`. Validates compilation under
   `-Werror` and all unit tests pass.

2. **`sanitizers`**: checkout → `cmake --preset sanitizer` →
   `cmake --build --preset sanitizer` → `ctest --preset sanitizer`. Runs on
   `ubuntu-latest`. Validates no AddressSanitizer or UndefinedBehaviorSanitizer
   violations occur during test execution.

Both jobs triggered on: push to `main`, and all pull requests.

ThreadSanitizer, clang-format, and clang-tidy jobs are explicitly deferred (see Scope
decisions above).

## Consequences

- Every PR and push to `main` gets automated build verification on Linux, catching
  platform-specific issues (compiler differences between Apple Clang and GCC/Linux
  Clang, header availability, linker behavior) that local macOS builds miss.
- `-Werror` violations are now caught automatically — no reliance on developers
  remembering to build locally before pushing.
- Memory corruption, use-after-free, buffer overflows, signed-integer overflow, and
  other undefined behavior are caught by the sanitizer job, even when they don't
  manifest as visible failures in a non-instrumented build.
- CI does NOT catch: semantic/logic bugs that compile and pass tests (e.g., the
  `supports_prefix_sharing = true` honesty bug from ADR-004's addendum), design
  violations, or performance regressions. These remain the domain of code review and
  future benchmark-regression jobs.
- The `FetchContent` step (downloading Catch2) will add ~10-20s to the first CI run
  per job; GitHub Actions caches can be added later if this becomes annoying, but at
  current project size it's not worth optimizing yet.
- Adding a ThreadSanitizer job later is a backwards-compatible extension: add a `tsan`
  CMake preset and a third job to the workflow file.

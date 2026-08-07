# ADR-006: Build acceleration via ccache and Ninja

## Status
Accepted

## Date
2026-08-08

## Context

CI runs (ADR-005) start from a clean checkout every time, meaning all compilation —
including third-party dependencies like Catch2 v3 (which is a compiled static library,
not header-only) — happens from scratch on every push and PR. Locally, incremental
builds are fast because `build/` persists between invocations, but CI has no such
persistence. As the codebase grows, this problem compounds: every new source file adds
to the from-scratch cost CI pays.

The existing build configuration uses Unix Makefiles as the CMake generator and has no
compiler caching. The machine (10 cores locally, 4 cores on `ubuntu-latest` GitHub
Actions runners) is underutilized because Makefiles' recursive scheduling model does
not parallelize as aggressively as possible, and no explicit `-j` flag or
`CMAKE_BUILD_PARALLEL_LEVEL` is set in the presets.

### Options considered for compilation caching

**A. `actions/cache` on the `build/_deps` directory.**
Caches the compiled Catch2 artifacts between CI runs. Simple, but fragile: any change
to CMake flags, compiler version, or preset configuration silently invalidates the
cache without detection, potentially causing mysterious link failures or stale-object
bugs. Also only helps CI, not local fresh builds.

**B. `ccache` (compiler cache) with `actions/cache` backing its cache directory.**
Sits in front of the compiler, hashes preprocessed source + flags, and returns cached
`.o` files when the hash matches. This is *content-addressed* — if Catch2's source and
your compiler flags haven't changed, it's a cache hit regardless of whether `build/`
was wiped. Invalidation is automatic and correct (flag change = different hash = miss).
Works locally too (install `ccache` via Homebrew, same mechanism). Used by LLVM,
Firefox/Gecko, Android, and most serious C++ CI pipelines.

**C. `sccache` (Mozilla's Rust rewrite of ccache).**
Adds cloud/remote cache backends (S3, GCS, Azure). Overkill for a single-repo project
with one CI runner and no distributed build. Worth revisiting if the project ever needs
shared caching across multiple developers or CI machines.

**D. vcpkg / Conan binary caching.**
Package managers that can fetch *precompiled* binaries for third-party dependencies,
skipping compilation entirely. Proportionate when a project has many third-party
dependencies; disproportionate for a project with exactly one (Catch2). Revisit when
llama.cpp, Boost.Asio, or other large dependencies arrive in Milestone 2+.

### Options considered for build parallelism

**E. Unix Makefiles with explicit `-j$(nproc)`.**
Works, but Make's recursive model has known scheduling inefficiencies (over-
subscription, poor load balancing across recursive submakes).

**F. Ninja generator.**
Purpose-built for fast incremental and parallel builds. Computes a flat dependency
graph (no recursion), schedules work more efficiently than Make, starts faster (no
Makefile parsing overhead), and is the de facto standard generator for modern C++
projects using CMake. One-line preset change.

## Decision

1. **Switch the CMake generator to Ninja** for all presets. Ninja is available via
   `apt-get install ninja-build` on CI runners and `brew install ninja` locally.

2. **Enable `ccache`** as the compiler launcher (`CMAKE_CXX_COMPILER_LAUNCHER:
   STRING=ccache`). On CI, use the `hendrikmuhs/ccache-action` GitHub Action (or
   equivalent), which handles cache save/restore automatically. Locally, install
   `ccache` via Homebrew for the same benefit on fresh builds.

3. **Set parallel jobs explicitly** via the Ninja generator's default (uses all
   available cores automatically — no manual `-j` needed, unlike Make).

vcpkg/Conan and sccache are explicitly deferred as disproportionate to current project
size.

## Consequences

- First CI run after this change still pays full compilation cost (cold cache). Every
  subsequent run with unchanged source files gets near-instant cache hits for those
  translation units — Catch2's ~30s compile becomes <1s on cache hit.
- Local developers who install `ccache` (`brew install ccache ninja`) get the same
  benefit: `rm -rf build && cmake --preset dev && cmake --build --preset dev` is fast
  on second run because the compiler cache persists in `~/.cache/ccache` (or
  `~/.ccache`) independent of the build directory.
- Ninja's automatic parallelism means no manual `-j` flag or
  `CMAKE_BUILD_PARALLEL_LEVEL` tuning is needed — it uses all available cores by
  default.
- Cache invalidation is automatic and correct: any change to source content, included
  headers, or compiler flags produces a different hash and triggers a real recompile.
  No stale-object risk.
- CI cache size grows with the number of unique translation units × build
  configurations (dev, sanitizer). At current project size this is trivial (<100MB);
  GitHub Actions provides 10GB of cache space per repository.
- When additional third-party dependencies arrive (Milestone 2+), the decision on
  whether to adopt vcpkg/Conan should be revisited — the threshold is roughly "3+
  compiled dependencies where build-from-source time exceeds the overhead of managing
  a package-manager toolchain."

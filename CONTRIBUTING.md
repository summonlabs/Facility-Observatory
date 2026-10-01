# Contributing to Facility Observatory

Facility Observatory is licensed under the Apache License 2.0. By submitting a
contribution you agree that it is licensed under the same terms. There is no
Contributor License Agreement to sign and no copyright assignment: you keep the
copyright in what you contribute, and Section 5 of the licence covers the
inbound grant.

## Before you start

Read `README.md` and, in particular, the systems boundary. This repository owns
observation only. A change that would let the runtime mutate facility state,
incident lifecycle, capacity, placement, policy, power, cooling, maintenance or
recovery is out of scope by construction, not by preference.

## Building and testing

The repository builds with CMake 3.20 or newer and a C++20 compiler. The
continuous expectation is the same as the release standard:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

A change is expected to satisfy all of the following.

* Release, Debug and (where the toolchain supports it) AddressSanitizer builds
  are clean under the repository's warning policy. On MSVC that means `/W4 /WX`
  with zero first-party warnings.
* The full CTest suite passes. Tests declare no timeouts: a hang is a defect to
  diagnose, not something to bound.
* New behaviour arrives with tests that would fail without it. Prefer a property
  test with a stated seed over a single handwritten example when the behaviour
  is an invariant.
* Randomized tests state their seed so a failure can be replayed exactly.
* Any claim in documentation is backed by something the repository actually
  does. Do not describe capability that has not been built and measured.

## Code quality expectations

* Portable C++20 and standard CMake. New third-party dependencies need a written
  justification; the current dependency set is the C++ standard library only.
* Strong types for identities, generations, epochs, revisions, quantities and
  authority state. If two values must never be interchanged, give them different
  types.
* Unknown, stale, conflicting, unsupported, indeterminate and refused outcomes
  are modelled explicitly. Do not collapse them into a default value.
* Checked arithmetic for anything that could overflow, and bounded behaviour for
  anything that could grow without limit.
* Persistence stays versioned and integrity checked. Keep the commit point
  explicit and keep idempotency identity inside the same durable record as the
  change it protects.
* Lock discipline: acquire in the documented rank order, never upgrade a shared
  lock, never emit a callback while holding a lock. `LockOrderAudit` enforces
  this at run time and the concurrency suite asserts it stays empty.

## Commit messages

Commit messages are concise, neutral and public-facing. Describe the change, not
the process. Do not add co-author trailers or AI attribution of any kind.

## Reporting a problem

Open an issue with the exact command, the observed result and the expected
result. If the problem involves durable content, include the output of
`facility-observatory verify --journal <path>`; it reports structural faults
without modifying the file.

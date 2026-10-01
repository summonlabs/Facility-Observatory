# Facility Observatory

A read-only evidence observatory for Data Center Control Plane facility state.

Facility Observatory records what adjacent facility authorities publish, keeps
their disagreements intact, and answers one question deterministically and with
its reasons attached:

> Given the currently published evidence from facility authorities, what is
> known, what is stale, unknown, conflicting or unsupported, where do
> authoritative views diverge, and why?

It is a C++20 library with a command line front end. It has no dependencies
beyond the C++ standard library.

---

## What this repository is not

Observation is not ownership. This runtime owns **observation, normalization,
correlation, explanation, provenance, freshness, divergence detection and
historical inspection** of facility-wide authoritative evidence.

It does **not** own, and cannot mutate:

* facility state
* incident lifecycle
* capacity or placement
* policy
* power or cooling
* maintenance or recovery
* the authority of adjacent DCCP, ASI, DFI, BMS or DCIM runtimes

The boundary is not a convention. Every operation the runtime could be asked to
perform is enumerated in `fo::BoundaryOperation`, and anything that would transfer
authority is refused with `mutation_refused` rather than partially attempted:

```cpp
const fo::Status refused = fo::Observatory::assert_operation(fo::BoundaryOperation::control_cooling);
// refused.code() == fo::ReasonCode::mutation_refused
```

Three consequences follow, and they are visible throughout the model:

* **Observation is not ownership.** Seeing another runtime's evidence does not
  transfer its authority. Cross-authority disagreement is reported, never
  silently resolved.
* **Acknowledgement is not effect.** Recording that an authority claimed
  something says nothing about whether the claim took effect.
* **Recovered evidence is not fresh evidence.** A record read back from durable
  storage is historical until a live publication refreshes it.

---

## Build

Requires CMake 3.20 or newer and a C++20 compiler. There is nothing to fetch.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful options:

| Option | Default | Meaning |
| --- | --- | --- |
| `FACILITY_OBSERVATORY_BUILD_TESTS` | `ON` when top level | Build the test suite |
| `FACILITY_OBSERVATORY_BUILD_CLI` | `ON` | Build the command line tool |
| `FACILITY_OBSERVATORY_BUILD_BENCHMARKS` | `ON` when top level | Build the benchmark runner |
| `FACILITY_OBSERVATORY_WARNINGS_AS_ERRORS` | `ON` | Promote first-party warnings to errors |
| `FACILITY_OBSERVATORY_ENABLE_ASAN` | `OFF` | AddressSanitizer build |
| `BUILD_SHARED_LIBS` | `OFF` | Build the runtime as a shared library |

The verified configuration is MSVC 19.44 (Visual Studio 2022), CMake 4.3, Ninja,
C++20, `/W4 /WX`, zero first-party warnings.

---

## Quick start

```sh
# Record what authorities published. One JSON object per line.
facility-observatory ingest --journal observatory.foj --file evidence.jsonl

# Ask what is known, and why.
facility-observatory view --journal observatory.foj --subject rack:sea1-r07 --aspect power.draw

# Find where authoritative views do not agree.
facility-observatory divergence --journal observatory.foj

# Check durable storage without modifying it.
facility-observatory verify --journal observatory.foj
```

An evidence line looks like this. Note that decimals travel as strings: the wire
format has no floating point anywhere.

```json
{"kind":"evidence","authority":"dccp-a","authority_kind":"dccp","source":"telemetry",
 "epoch":1,"generation":7,"revision":1,
 "observed_at":"2026-03-01T12:34:56Z","published_at":"2026-03-01T12:34:56Z",
 "subject":"rack:sea1-r07","aspect":"power.draw",
 "value":{"kind":"scalar","decimal":"4.25","unit":"kilowatts"}}
```

Two other document shapes are accepted: `{"kind":"authority", ...}` to register a
publisher, and `{"kind":"epoch", ...}` to advance an authority's epoch fence.

---

## Library use

```cpp
#include <facility_observatory/observatory.hpp>

fo::ObservatoryOptions options;
options.journal.path = "observatory.foj";
options.journal.origin = "my-service";

auto observatory = fo::Observatory::open(options);
if (!observatory) {
  // observatory.reason() carries the code and a human-readable detail
  return;
}

fo::AuthorityDescriptor authority;
authority.id = fo::AuthorityId::parse("dccp-a").value();
authority.kind = fo::AuthorityKind::dccp;
authority.role = fo::AuthorityRole::authority;
authority.epoch = fo::Epoch::from_value(1);
observatory.value().register_authority(authority);

fo::EvidenceRecord record;   // fill in key, epoch/generation/revision, times, value
const fo::IngestionOutcome outcome = observatory.value().record_evidence(record);
// outcome.code        why this admission ended the way it did
// outcome.durable     whether the durable commit point was reached first
// outcome.sequence    the journal position that now holds it

const fo::Evaluation evaluation =
    observatory.value().evaluate(entity, aspect, evaluation_instant);
// evaluation.state          known | stale | unknown | conflicting | unsupported | indeterminate
// evaluation.value          present when state is known or stale
// evaluation.participants   every record considered, in canonical order
// evaluation.contributors   the records that produced the answer
// evaluation.disagreement   every distinct claim, preserved, when conflicting
// evaluation.participant_notes  why each participant was accepted or rejected
```

Every public result type has exactly one JSON rendering, in
`facility_observatory/render.hpp`, used by the CLI, by the tests and by
downstream consumers, so documentation and behaviour cannot drift apart.

---

## Architecture

| Layer | Header | Responsibility |
| --- | --- | --- |
| Primitives | `strong.hpp`, `checked.hpp`, `time.hpp` | Strong identities, exact decimal quantities, total checked arithmetic, UTC instants |
| Outcomes | `outcome.hpp` | Value-or-reason results with a stable reason-code vocabulary |
| Model | `authority.hpp`, `evidence.hpp` | Who published what, about which subject, at which position in whose sequence |
| Policy | `policy.hpp` | Freshness windows, aspect classes, authority precedence |
| Engine | `store.hpp`, `divergence.hpp` | Admission, evaluation, aggregation, cross-domain divergence |
| Durability | `platform.hpp`, `journal.hpp`, `codec.hpp` | Kernel-enforced writer lock, versioned integrity-checked journal, explicit codec |
| Runtime | `observatory.hpp`, `lock_order.hpp` | Single-writer facade, lock-rank audit, event delivery |
| Edges | `contracts.hpp`, `json.hpp`, `render.hpp` | Typed input from adjacent runtimes, deterministic JSON |

The engine is deliberately **not** thread safe. Concurrency ownership belongs to
the facade, which holds exactly one lock over the store so that a read-to-write
upgrade is structurally impossible.

---

## Domain model

### Evidence

An `EvidenceRecord` is a claim, not a fact:

| Field | Meaning |
| --- | --- |
| `key.authority`, `key.source` | Which runtime family and which publication channel |
| `key.entity` | The domain and identifier pair, for example rack:sea1-r07 |
| `key.aspect` | Dotted path such as `power.draw` |
| `epoch` | The authority's own fencing epoch; epoch 0 means never published |
| `generation`, `revision` | Position within that epoch, and the authority's correction of it |
| `observed_at`, `published_at` | When the authority says it saw the world, and when it said so |
| `value` | Absent, exact decimal scalar with a unit, enumeration, bounded UTF-8 text, or boolean |
| `provenance` | When it arrived, whether it is live, recovered, synthetic or imported, and where it sits durably |

### Admission

Offering a record to the runtime produces an explicit outcome. Nothing is
silently dropped and nothing is silently accepted:

| Outcome | Meaning |
| --- | --- |
| `ok` | Admitted as current at this generation and revision |
| `duplicate_evidence` | Byte-identical publication already held; idempotent no-op |
| `superseded_revision` | A newer generation or revision exists; retained as history |
| `conflicting_evidence` | The authority published a different claim at the same generation and revision, and both are preserved |
| `stale_epoch` | The authority has moved past this epoch; retained as history, never current |
| `authority_unknown` | The publisher was never registered |
| `indeterminate` | Times are missing, or the publication precedes the observation |
| `record_too_large`, `too_many_records`, `invalid_argument` | A bounded resource or shape rule was hit |

### Observation state

Six states, all explicit. There is no probably-fine state.

| State | Meaning |
| --- | --- |
| `known` | A single fresh claim is current |
| `stale` | Evidence exists but is older than the freshness window, or is recovered |
| `unknown` | No authority has published anything for this subject and aspect |
| `conflicting` | Two or more authorities, or one authority twice, disagree |
| `unsupported` | Evidence exists but cannot be interpreted, for example incompatible units |
| `indeterminate` | Evidence exists but is not evaluable, for example future-dated |

### Freshness

Freshness is judged on **how old the observation is**, not on when it arrived.
An authority that stops observing produces stale evidence even if it keeps
republishing the same reading.

* older than `fresh_for`, but within `stale_after`, the state is stale
* older than `stale_after` the record is expired, so it no longer asserts a current value
* leading the evaluation instant by more than `future_skew` it is refused as future-dated
* a publication instant earlier than the observation instant is unevaluable

### Precedence and disagreement

By default, two authorities that disagree produce `conflicting`, and every
distinct claim is preserved in `Evaluation::disagreement`. When an aspect policy
declares an ordered authority preference, the highest-ranked authority that has
current evidence determines the value, and the disagreement is **still
reported**. The observatory never invents a precedence rule of its own.

### Units

Quantities are exact decimals: an `int64` mantissa and a scale from 0 to 9, with
checked arithmetic throughout. Conversions are exact or they fail; there is no
rounding unless the caller explicitly asks for one. Values are reported in the
canonical unit of their dimension with trailing zeros removed, so `4.25 kilowatts`
reads back as `4250 watts` and `1500.0 watts` as `1500 watts`.

---

## Divergence

Divergence is where two authoritative views of the same physical reality do not
agree. The observatory reports the disagreement and the evidence behind both
sides; it never decides which authority is right.

| Class | Detects |
| --- | --- |
| `source_disagreement` | Two authorities assert different values for one aspect |
| `aggregation_mismatch` | A published total disagrees with the sum of its constituents beyond a relative tolerance |
| `temporal_skew` | Participants observed the world far apart in time |
| `coverage_gap` | Topology places a member under a parent that publishes nothing the total is built from |
| `unit_incompatible` | The same aspect is reported in unconvertible units |
| `identity_mismatch` | Topology names a subject no authority has ever published |
| `recovered_versus_live` | Durable history and live evidence disagree |

Containment is itself evidence, which is what makes cross-domain reconciliation
work without the observatory assuming a topology: a child publishes
`topology.parent` with a text value naming its parent, and that link can be
stale, missing or disputed like any other claim.

---

## Persistence

### Format

The journal is append-only and versioned. All integers are little-endian.

```
file header, 32 bytes
  +0   magic            8 bytes   FCOBJRN1
  +8   format_version   u32       currently 1
  +12  flags            u32       must be zero
  +16  created_at       i64       unix nanoseconds
  +24  header_crc32c    u32       CRC-32C over bytes 0 to 24
  +28  reserved         u32       must be zero

frame, repeated
  +0   payload_length   u32       bounded by max_payload_bytes
  +4   record_type      u8
  +5   flags            u8        must be zero
  +6   reserved         u16       must be zero
  +8   sequence         u64       strictly 1, 2, 3, ... with no gaps
  +16  committed_at     i64       unix nanoseconds
  +24  payload          payload_length bytes
       crc32c           u32       CRC-32C over bytes 0 to 24 + payload_length
```

The record codec validates everything on the way back in: lengths are bounded,
identifiers are re-checked against their class, UTF-8 must be well formed, units
must be known, and a trailing byte is an error rather than a tolerated extra.

### Commit point

The commit point is the successful return of `Journal::commit()`, which flushes
every frame appended since the previous commit. With the default
`commit_on_append`, each observation reaches that point before it returns.

`Observatory::record_evidence` deliberately commits **before** the in-memory view
moves. It plans the admission decision without mutating anything, makes the frame
durable, and only then applies the decision, reporting `internal_error` if the
committed decision differs from the planned one, because that difference would be
a defect rather than a result. A crash can therefore never leave the store ahead
of storage.

### Recovery

Recovery is deterministic and conservative:

| Finding | Action |
| --- | --- |
| Clean file | Open, no modification |
| Frame extends past the end of the file | Torn tail: truncate to the last complete frame and report exactly how many bytes were lost |
| CRC mismatch in the final frame | Indistinguishable from a torn write, so treated as a torn tail |
| CRC mismatch with complete frames after it | Interior corruption: refuse to open, leave the file untouched |
| Sequence gap | Interior corruption: refuse |
| Preamble no writer could have produced | Interior corruption: refuse |
| Short file, bad magic, bad header CRC, non-zero flags | Header corruption: refuse |
| Unsupported format version | Refuse, naming the version |
| Truncation disabled | Refuse and leave the file alone |

A damaged length field is never silently truncated away: the writer emits the
24-byte preamble as a single prefix, so a genuinely torn frame always carries a
sane length and a known record type. Anything else is corruption and is refused.

### Single writer

Authoritative ownership is enforced by the kernel, not by convention: an
exclusive lock is taken on the journal lock file for the process lifetime,
through `LockFileEx` on Windows and `fcntl(F_SETLK)` elsewhere. A second writer,
in another process or through a second handle in this process, fails immediately
with `journal_locked` rather than blocking, and the lock is released by the
kernel when the process dies, including on a hard kill.

### Read-only inspection

`verify` and the query commands open the journal read-only: no writer lock is
taken, nothing is written, and a torn tail is reported rather than repaired.

---

## Concurrency

Lock ranks, acquired in strictly increasing order:

| Rank | Lock | Held for |
| --- | --- | --- |
| 1 | `lifecycle` | Lifecycle transitions |
| 2 | `ingest` | Serialising every mutation |
| 3 | `journal_queue` | Hand-off to the journal writer thread |
| 4 | `journal_writer` | The writer thread's durable append |
| 5 | `event_dispatch` | Event log and handler bookkeeping |
| 6 | `store` | The evidence store itself, always innermost |

The rules, each of which the audit enforces at run time:

* Readers take the store lock shared and never upgrade it. Mutations retake it
  exclusively.
* The journal is touched only by the writer thread, which holds nothing else
  while appending.
* The store lock is never held while the ingest or journal locks are acquired.
* Event handlers run after **every** lock has been released. A handler that calls
  back into the observatory receives `internal_error` instead of deadlocking.
* Shutdown sets the stopping flag, notifies, and joins the writer without holding
  worker-required state. It is idempotent, and work offered after it is refused
  with `shutting_down`.

`fo::LockOrderAudit` records any acquisition that does not strictly increase the
rank, and the concurrency suite asserts the count stays at zero under a
three-writer, four-reader stress run.

---

## Contracts with adjacent runtimes

`facility_observatory/contracts.hpp` carries a publication from an adjacent
runtime into the observatory and nothing else. There is no message that asks the
observatory to change facility state.

* Every envelope declares the runtime family that published it, and the contract
  refuses to reinterpret one family's payload as another's claim.
* The evidence contract version is explicit and checked on the way in.
* Envelopes are bounded by shape, time and domain before they reach the store.

| Runtime family | May publish into |
| --- | --- |
| dccp | facility, site, rack, capacity, incident |
| asi | asset, dependency, rack, site |
| dfi | dependency, capacity, site |
| bms | facility, site, rack |
| dcim | facility, site, rack, asset, capacity |
| plant | facility, site, rack, asset |
| economic | capacity, site |
| synthetic | any modelled subject |

---

## Command line

```
facility-observatory <command> [options]

  version                                     runtime identity
  selftest                                    built-in deterministic checks
  verify      --journal <path>                scan the journal without modifying it
  ingest      --journal <path> [--file <p>]   record JSON-lines observations (stdin if no --file)
  status      --journal <path>                summary of what is currently held
  subjects    --journal <path> [--domain <d>] list known subjects
  view        --journal <path> --subject <s> [--aspect <a>] [--at <ts>]
  aggregate   --journal <path> --aspect <a> [--subject <s>]... [--kind <k>] [--allow-partial]
  divergence  --journal <path> [--subject <s>] [--at <ts>]
  history     --journal <path> --subject <s> [--aspect <a>]
  snapshot    --journal <path> [--at <ts>]
  reconstruct --journal <path> --sequence <n> [--at <ts>]

global options
  --pretty          indent the JSON output
  --at <rfc3339>    evaluation instant; defaults to the newest durable publication
  --read-only       never take the writer lock and never modify the journal
```

When `--at` is omitted, the evaluation instant is the newest publication instant
in the store rather than the wall clock. A result is therefore reproducible from
the journal alone.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Known, or the command completed |
| 1 | Internal failure |
| 2 | Usage or malformed input |
| 3 | Unknown, unsupported or out of contract |
| 4 | Conflicting evidence, or an indeterminate answer |
| 5 | Boundary refusal |
| 6 | Persistence fault |

`divergence` exits 4 when it finds anything, so a pipeline can gate on it.
`verify` exits 6 on a structural fault, and 0 only on a clean file.

---

## Install and downstream use

```sh
cmake --install build --prefix /some/clean/prefix
```

The install exports a CMake package and places the command line tool in the
prefix bin directory. An independent out-of-tree consumer needs only:

```cmake
find_package(FacilityObservatory 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE FacilityObservatory::facility_observatory)
```

A worked consumer lives in `examples/consumer`. It is built and run against the
installed prefix, from a copy outside the source tree, as part of validation.

---

## Validation

Everything reported here was executed on the host described under Benchmarks.
The suite contains **117 cases across 10 executables**, all passing, with no test
declaring a timeout: a hang is treated as a defect to diagnose.

| Suite | Cases | Covers |
| --- | --- | --- |
| `test_strong` | 20 | Checked arithmetic at every boundary, exact decimal parsing and rendering, division exactness and rounding modes, unit conversion round trips, UTF-8 validation, identifier classes, counter overflow, boundary refusals |
| `test_time_json` | 15 | RFC 3339 round trips, offsets, adaptive fractions, leap-second and malformed-input rejection, duration saturation, JSON round trips, escape handling, insertion order, depth and size limits, seeded parser fuzzing |
| `test_codec_policy` | 14 | Codec round trips for every value kind, truncation and trailing-byte rejection, malformed UTF-8 rejection, freshness boundaries and monotonicity, policy validation, contract boundary matrix, ingest document shapes |
| `test_store` | 18 | Every admission path, epoch fencing, evaluation states, unit normalisation, recovered-evidence semantics, precedence without hiding disagreement, canonical history, permutation invariance over 40 shuffled admission orders, aggregation kinds and completeness, bounded resources, plan and commit agreement over 300 randomized rounds |
| `test_divergence` | 9 | Every divergence class, tolerance behaviour, canonical ordering, result bounds, scoped queries |
| `test_journal` | 14 | Create, reopen, header validation, torn-tail recovery at three cut points, truncation refusal, interior corruption, sequence gaps, impossible frame shapes, payload and record limits, restart durability, snapshot reconstruction, corruption refusal on open |
| `test_concurrency` | 7 | Lock-rank audit acceptance, inversion and self-nesting detection, three-writer and four-reader stress with zero violations, event delivery outside every lock, re-entrancy refusal, idempotent shutdown, concurrent snapshots |
| `test_multiprocess` | 5 | Real separate processes: a held lock excludes a second writer, the kernel releases the lock on process exit, evidence written by a child is recovered by the parent with an identical digest, two writers are serialised |
| `test_e2e` | 6 | The installed command: identity, self test, usage errors, the full ingest, verify, status, view, history, divergence, aggregate, snapshot and reconstruct flow, malformed input handling, persistence-fault reporting |
| `test_adversarial` | 9 | Journal paths beyond the classic 260-character limit, source disappearance distinguished from ordinary ageing, a throwing event handler, handler re-entrancy refusal, close racing ingestion, reconstruction bounds, empty-journal answers, default-policy durability across a restart, every boundary operation |

Notable properties the suite establishes rather than asserts:

* **Permutation invariance.** Admitted in any order, the same set of publications
  produces an identical store digest, an identical evaluation and identically
  ordered history.
* **Restart stability.** The digest is a function of the retained evidence set,
  not of receipt metadata, so it survives a restart unchanged while every
  recovered dynamic claim stays out of the current view.
* **Plan and commit agreement.** The decision computed before the durable write
  matches the decision applied after it, over 300 randomized rounds.

### Build and sanitizer results

| Configuration | Result |
| --- | --- |
| Release, MSVC `/W4 /WX` | Builds clean, zero first-party warnings, 10 of 10 suites pass |
| Debug, MSVC `/W4 /WX` | Builds clean, zero first-party warnings, 10 of 10 suites pass |
| RelWithDebInfo with AddressSanitizer | Builds clean, 10 of 10 suites pass with no sanitizer report |
| Install to a clean prefix, then out-of-tree consumer | Package found by `find_package`, consumer built, linked and run against the installed prefix |

---

## Benchmarks

`bench_observatory` counts **completed work**: operations that returned
successfully, divided by the wall time they took. Every figure is labelled
REAL, SYNTHETIC or UNSUPPORTED.

* **REAL** means the measurement exercises the real mechanism: real syscalls,
  real files, real CRC verification, real checked arithmetic.
* **SYNTHETIC** means the workload is generated evidence. No physical facility
  hardware is attached to this machine, so the evidence fed in is modelled.
* **UNSUPPORTED** means a claim this repository deliberately does not make.

Run it with:

```sh
cmake --build build --target bench
```

### Measured results

Single run on the development host: Windows, MSVC 19.44, Release with `-O2 /Ob2`,
Ninja, local NTFS volume, 64-bit. Figures are from one run and will vary with
the host; the methodology above is what makes them comparable.

| Measurement | Class | Completed | ns per operation | Operations per second |
| --- | --- | ---: | ---: | ---: |
| `journal_append_committed` | REAL | 200 | 1,396,555 | 716 |
| `journal_recovery_scan` | REAL | 200 | 24,961 | 40,061 |
| `evidence_admission` | REAL | 50,000 | 2,466 | 405,383 |
| `aspect_evaluation` | REAL | 20,000 | 1,362 | 734,103 |
| `divergence_full_scan` over 50 racks | REAL | 5 | 2,714,980 | 368 |
| `aggregate_sum_complete` over 200 racks | REAL | 2,000 | 742,911 | 1,346 |
| `json_parse_and_dump` | REAL | 20,000 | 4,110 | 243,298 |

The durable append figure is dominated by the flush, which is the point: each
observation reaches its commit point before it is reported as durable. Recovery
scan is roughly 56 times cheaper per record than a committed append, which is the
expected shape when durability is synchronous.

### Explicitly not measured

* Multi-node or clustered operation. This build was validated on a single host.
* Real BMS, DCIM, plant, electrical, cooling, generator or power-train telemetry.
  No such hardware is attached to this machine, and no such claim is made.
* Network partition, clock skew or byzantine behaviour across hosts.
* Throughput or latency on storage other than the local file system used here.

---

## REAL / SYNTHETIC / UNSUPPORTED

**REAL in this repository**

* Persistence: a real file system, real flushes, real CRCs, real recovery from
  real truncation.
* Locking: real kernel-enforced single-writer exclusion, proven with separate
  operating-system processes.
* Process death and restart: real process exit, real reopen, real replay.
* Packaging and installation: a real CMake package, a real clean prefix, a real
  out-of-tree consumer.
* Arithmetic and encoding: real checked integer arithmetic, real exact decimals,
  real UTF-8 and CRC validation.
* Concurrency: real threads, a real writer thread, a real lock-order audit.

**SYNTHETIC**

* Every item of facility evidence used in the tests and benchmarks. No physical
  facility hardware is attached, so plant, telemetry, power, cooling and economic
  evidence is modelled. This is stated in the data itself: such publishers are
  registered as synthetic and their records carry synthetic durability.

**UNSUPPORTED**

* Clustered or multi-node operation.
* Any claim about physical facility behaviour, electrical measurement, cooling
  performance or generator response.
* Any integration with a real BMS, DCIM or plant historian.
* Any network-level fault model.

---

## Limitations

* **Single node.** One process owns a journal at a time, by kernel lock. There is
  no replication and no distributed consensus.
* **Recovered evidence needs refreshing.** After a restart, recovered dynamic
  evidence is reported as stale until a live publication refreshes it. That is
  the intended contract, not an oversight, and it means a freshly restarted
  runtime answers stale until its authorities speak again.
* **Bounded by design.** Records, authorities, subjects, aspects per subject,
  payload size, JSON depth, record count and divergence results all have explicit
  bounds and refuse rather than grow without limit.
* **Aspect semantics are operator supplied.** Freshness windows, aspect classes
  for static identity versus dynamic measurement, canonical units and authority
  precedence default to sensible values but are not derived from the data.
* **No wall clock in results.** Evaluation instants are inputs. The command line
  defaults to the newest publication instant so results are reproducible; callers
  that want the current time must supply it.
* **Text values are bounded and single valued.** A list-valued aspect is modelled
  as one record per member, not as one record holding a list.
* **ASCII identifiers, UTF-8 entity names.** Entity identifiers accept UTF-8;
  authority, source and aspect identifiers are restricted ASCII by construction.

---

## Repository layout

```
include/facility_observatory/   public headers, one per layer
src/                            implementation
apps/                           the facility-observatory command
tests/                          dependency-free harness and the suite
bench/                          benchmark runner
examples/consumer/              independent out-of-tree consumer
cmake/                          installed package configuration
```

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

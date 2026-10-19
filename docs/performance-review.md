# Performance review and optimization tracker

Updated 19 October 2026. Implementation and measurements start from `81e1b2c72dbb629ba58fea70bfad5040c4f3bad7`. The [original source review](performance-review-baseline.md), based on `f8fbe4c62495126010e927343d7ca0ffdb186af2`, is preserved as the source of O1-O8 and the historical session counts.

The first optimization pass removes repeated bridge work, improves selected libc/heap operations, and adds repeatable measurements. The results below measure **the combined change set in synthetic workloads**. They establish lower cost in those paths; they do not establish a gameplay FPS increase, an emulator comparison, or each change's isolated contribution.

## Status ledger

| ID | Status | Implemented in this pass | Work still open |
| --- | --- | --- | --- |
| O1 | Implemented | Typed ANGLE function table per graphics instance; resolve required exports once; construct trap, guest-target and JNI diagnostics only on failure. | CPU sampling in warmed gameplay; broader allocation attribution. |
| O2 | Initial work implemented | Indexed guest/image/host symbols with heterogeneous lookup; preserved guest precedence and image order; cached clear/draw targets with linker and JNI registration invalidation; preclassified common libc/GL wrappers. | Specialized leaf bridges only if transition cost dominates a controlled profile. |
| O3 | Partially implemented | Explicit uninitialized malloc/memalign/posix_memalign paths; calloc/internal allocation still clears; in-place shrinking realloc; counters for zeroed/copied bytes and realloc outcomes. | Size/lifetime distributions, small-block pools, measured contention, safe page trimming and growth in place. |
| O4 | Partially implemented | Page-bounded length and comparison scans; bounded duplication; 32-bit Android wide comparisons; bulk validation/marshalling for the existing guest jump-buffer layout. | Profile remaining wide/string routines and bulk ABI handlers before extending the change. |
| O5 | Investigation open | Entry-point overhead is handled by O1. GL calls and presentation behavior are preserved. | Repeated-value telemetry, correctly invalidated state filtering, GPU duration, and presentation-query/clear analysis. |
| O6 | Telemetry added | Opt-in main-CPU JIT/handler wall time and read/write/code callback counts. | Active worker accounting, translation/cache occupancy/flush metrics, protected read-only fast paths; preserve faults and bounded execution. |
| O7 | Investigation open | Cached frame native lookup honors RegisterNatives, UnregisterNatives and newly loaded symbols. | Audio wakeups/PCM ownership, immutable JNI signature caches, thread/CPU reclamation and processor-ID reuse. |
| O8 | Investigation open | Symbol indexing also removes repeated relocation/report lookup scans. | Archive/ELF ownership, path costs, log buffering and unchanged-save coalescing, based on measured startup/stall cost. |
| O9 | Implemented; newly identified | Replace an unbounded vector of frame durations with a fixed-size streaming histogram. Profiling stage storage is allocated only when enabled. | Interval/window selection for warmed gameplay remains open. |
| O10 | Implemented; newly identified | Skip per-slice clock reads for explicitly infinite deadlines; cancellation/checkpoints and finite deadlines remain active. | Attribute the saving in sustained worker profiles; no isolated speedup claimed. |
| O11 | Newly identified; open | Each 8-byte import stub currently consumes a separate 4 KiB allocation. | Evaluate packed immutable trap pages, including permission sealing, dynamic registration and JIT invalidation. |
| O12 | Newly identified; open | JNI reference deletion scans every remaining local frame; frame cleanup repeats this for each expired reference. | Measure nested-frame cleanup, then consider ownership indexing without breaking recursive container release or global/local references. |

A status of implemented applies to the scope in the implementation column, not to every investigation proposed by the original review.

## Recorded measurements

The benchmark executes a real ARM64 loop calling guest import stubs, rather than timing a replacement C++ implementation. Allocation cases enter the guest for malloc/realloc/free. Linker cases use a deterministic ELF with 4,096 exports; the long bounded-comparison case compares 32 bytes of 65,535-byte strings. These deliberately isolate avoidable costs and are not a representative game workload distribution.

Each executable was run three times, with one warmup plus nine measured samples per case per process: **27 measured samples per case**. The table compares their pooled medians in nanoseconds per operation. No build ran during measurement. Profiling was disabled. Both builds used the pinned MSVC Release configuration with `/O2` and LTO, on the same Windows x64 host and Intel UHD D3D11 backend. The baseline loaded ANGLE from the SDK and the candidate from its packaged directory; both `libGLESv2.dll` and `libEGL.dll` hashes match exactly. The comparison tool now checks these hashes.

<!-- BEGIN MEASURED RESULTS -->
| Synthetic case | Before ns/op | After ns/op | Time reduction |
| --- | ---: | ---: | ---: |
| glTexParameteri | 403.7 | 162.5 | 59.7% |
| import_dispatch | 172.7 | 103.2 | 40.2% |
| linker_host_symbol_4096 | 2,340.6 | 10.8 | 99.5% |
| linker_last_symbol_4096 | 9,849.4 | 9.6 | 99.9% |
| malloc_free_4096 | 1,130.3 | 748.3 | 33.8% |
| memcpy_64 | 179.2 | 109.3 | 39.0% |
| realloc_shrink_4096 | 1,965.8 | 1,037.4 | 47.2% |
| setjmp | 211.0 | 127.6 | 39.5% |
| strlen_32 | 229.4 | 107.8 | 53.0% |
| strlen_4095 | 6,306.1 | 189.3 | 97.0% |
| strncmp_prefix_32 | 18,519.0 | 115.6 | 99.4% |
<!-- END MEASURED RESULTS -->

Artifacts: [baseline](performance/2026-10-19-baseline.json), [optimized](performance/2026-10-19-optimized.json), [comparison](performance/2026-10-19-comparison.json), [validation summary](performance/2026-10-19-validation.json). Raw sample arrays, operation counts, compiler/CPU/renderer identity, fixture and executable hashes are retained. Changes are measured as a package; shared dispatch improvements also benefit libc and GL cases. Very large linker/prefix-comparison ratios describe the synthetic worst cases, not overall runtime speedups. Realloc retains its previous usable capacity when shrinking; lower copy/bookkeeping cost does not imply lower live capacity for that block.

The earlier review's 8,500,066 memcpy and 3,725,663 malloc calls in `out/reports/audio-focus-after.json`, and its 12,000-frame gameplay counters, remain valid historical optimization leads. They do not measure this implementation. See the [archived analysis and measurement boundaries](performance-review-baseline.md#2-what-the-existing-measurements-establish).

## What changed and what must stay correct

- **Graphics:** [Graphics::Impl and forwarding](../src/android/graphics.cpp) use the typed [entry-point list](../src/android/graphics_functions.inc). Loading resolves required functions before use; optional platform-display lookup remains at initialization. Tables belong to their DLL-owning instance, with no process-global cached pointer. Missing exports and calls before initialization still fail clearly. Object deletion now reads its input ID array through a const pointer. No state filtering, draw reordering, render-resolution change or presentation removal is included.
- **Dispatch and symbols:** [TrapTable::at / Cpu::call](../src/guest/cpu.cpp) keep checks and diagnostics while removing successful-path formatting. [Linker::load / symbol / image_symbol](../src/guest/linker.cpp) build indexes once, retaining the first definition within an image, guest-first resolution, absolute symbols and image load order. New host symbols and loaded images advance the revision. [Jni::native_cached](../src/android/jni.cpp) invalidates on registration/unregistration or linker changes; [Pvz2::frame](../src/games/pvz2.cpp) keeps its deadline and JNI local frame while using fixed-size draw arguments.
- **Heap:** [Memory::allocate / reallocate](../src/guest/memory.cpp) default to zero initialization for internal callers. Only explicit malloc-style paths select uninitialized memory. `calloc`, mappings and string terminators retain their contracts. Realloc shrinking retains capacity and contents; growth still allocates, copies the old usable bytes and frees the old block. Allocation size/alignment validation prevents rounding overflow and invalid alignment on a reused range. This is not a pool allocator or a reclamation redesign.
- **Strings and guest ABI:** [compare_strings / compare_wide and libc bindings](../src/android/bionic.cpp) validate page-bounded chunks, stop at NUL/limits, preserve unsigned-byte ordering and keep Android wide units at 32 bits. Zero-length bounded calls need no source read; unterminated prefixes at a protected page boundary no longer require reading the entire string. Setjmp/longjmp validate 176 bytes once, retain the reserved 104-111-byte hole on save, and preserve the existing integer/SP/FP offsets. Host CRT jump buffers are not used.
- **Measurement:** [TimingSummary / FrameTimings](../include/acp/performance.hpp), the [frame loop](../src/launcher/main.cpp), and [Cpu::statistics](../src/guest/cpu.cpp) provide bounded timing storage and sum counters with duplicate trap names. The CPU report explicitly labels its main-CPU scope. Per-frame profiling remains opt-in.

## Reproduce and track another pass

Build and record the baseline **before** changing runtime code. Preserve its executable for later repeated comparisons. Run from the repository root, with PowerShell 7:

```powershell
.\tools\build.ps1 -Configuration release -Test
.\tools\run-performance.ps1 -Out out/performance/before.json -Label before -Graphics
# Implement a focused change, then rebuild with the same toolchain and flags.
.\tools\build.ps1 -Configuration release -Test
.\tools\run-performance.ps1 -Out out/performance/after.json -Label after -Graphics
.\tools\compare-performance.ps1 -Baseline out/performance/before.json `
    -Candidate out/performance/after.json -Out out/performance/comparison.json
```

`run-performance.ps1` defaults to three separate runs, retains each raw report/log, hashes the executable and ANGLE DLLs, and pools the sample arrays. Omit `-Graphics` for CPU-only measurements. `compare-performance.ps1` rejects mismatched case sets, operation counts, compiler/CPU/fixture metadata, sample counts and graphics identities. It reports reductions and regressions without flaky timing thresholds in CTest. Keep machine/power/driver/build settings fixed and record any environmental changes; metadata checks cannot make an uncontrolled workload comparable.

For CPU sampling with optimized symbols:

```powershell
.\tools\build.ps1 -Configuration release -ProfileSymbols -Test
```

This adds `/Zi` and `/DEBUG:FULL` to first-party Release targets while keeping their optimization/LTO flags. A normal build invocation resets `ACP_PROFILE_SYMBOLS` to OFF. Do not compare builds with different optimization configurations.

For game-stage diagnostics, select an **isolated prepared profile** with `--profile NAME`, then add `--performance-profile` to the normal `run` command. The JSON adds:

| Field | Meaning and boundary |
| --- | --- |
| `frame_stages_ms.events` | Host event/input work in completed render-loop iterations. |
| `pacing` | Adapter sleep, preserving the game's requested rate. |
| `native_draw` | Guest clear plus the native draw/JNI call; includes lazy loading and callbacks on this CPU. |
| `presentation`, `swap` | Host preparation and EGL swap separately; these are wall times, not GPU durations. |
| `capture`, `callback_drain` | Optional readback/PNG writing and final bounded-frame callback drain. |
| `startup` | Command entry through native adapter startup; separate from frame-loop timings. |
| `guest_cpu.jit_wall_ms`, `import_handler_wall_ms` | Main-CPU lifetime wall time while profiling, including startup/shutdown. Nested guest work is subtracted from enclosing handler time to avoid double counting. These are not OS thread CPU times. |
| `guest_cpu.memory_callbacks` | Checked data read/write and code-fetch callback counts while profiling; direct page-table accesses are not counted. Code fetch describes translation work, not every translated instruction's execution. |
| `guest_memory` | Allocation/live-capacity statistics plus `zeroed_bytes`, `realloc_in_place`, `realloc_moved`, `realloc_copied_bytes`. These are lifetime totals. |

Profiling reads clocks around every JIT run and import handler and changes the workload's cost. Use it for diagnosis, then measure final performance with it disabled. Active pthread and callback CPU statistics remain excluded; the report states that boundary. GPU duration, OS CPU time/working set, cache occupancy and warmed interval snapshots are still missing.

Frame/stage means, maxima and totals retain the recorded durations. Quantiles use nearest-rank histogram upper bounds with at most **3.125% relative bucket width**; the method is included in JSON. This replaces the old exact sorted frame history to bound memory in long sessions. Do not interpret a small percentile difference within that quantization as a regression or a win.

## Validation and limits

The Release contract suites cover launcher settings, runtime/ABI contracts, offline APIs, audio, the independent ARM64 Android application, PVZ2 input and disabled billing. Added contracts exercise allocation reuse/zeroing/realloc, protected-page and zero-limit string behavior, unsigned and 32-bit wide comparisons, real ARM64 setjmp/longjmp register restoration, duplicate-name counters, nested profiling, JNI cache invalidation and timing summaries. The separate graphics smoke validates shader/client-array rendering, pixels, ETC1, offscreen/default framebuffer handling, aspect fit, resize and state restoration.

A 360-frame profiled session and a 1,200-frame session with profiling disabled were run using a copied save profile. Both ended at their frame limits with no CPU fault or worker errors, and audio remained active with one engine/player/stream. Captures show startup/loading artwork. These are **startup/loading/lifecycle smoke checks**, not combat validation, a warmed frame-rate comparison, proof of perceptual audio quality, or full visual equivalence. Local raw reports/captures are under `out/performance/`; the tracked validation summary excludes save/device identifiers. No claim is made that the loading sessions represent the same game progress.

## Newly discovered opportunities and next acceptance work

1. **O9: session-duration-dependent timing memory.** The old frame vector grew with every frame, then sorted the full history at shutdown. It is replaced with a 16 KiB summary; opt-in stage summaries allocate only when enabled. Validate quantile consumers against the stated approximation instead of preserving an unbounded history.
2. **O10: redundant infinite-deadline checks.** The current long-lived pthread path explicitly opts out of deadlines. Reading the clock and comparing against `time_point::max()` at every slice/import adds no enforcement. The check is now conditional on a finite deadline. Existing instruction-quota, timeout, cancellation and worker-failure tests remain the acceptance boundary.
3. **O11: trap code allocation density.** [TrapTable::add](../src/guest/cpu.cpp) allocates a 4 KiB page for an SVC/RET pair. Packing stubs could reduce startup commits/page metadata and instruction-fetch locality costs. Measure trap count, committed bytes and translation first; immutable-page sealing, registration after startup and stale translations make this more than changing an alignment constant.
4. **O12: JNI cleanup search multiplication.** [Jni::pop_frame / delete_ref](../src/android/jni.cpp) release each expired handle through a path that visits all remaining local frames. Deep nesting and large outer frames can multiply work. An ownership index or batched frame cleanup is a candidate; prove cross-thread/global/local handle semantics and recursive container deletion before changing it. Its gameplay cost is not established.

Next acceptance tasks:

- [ ] Record three warmed, repeatable combat intervals from the same copied save, APK/data hashes, resolution, ANGLE/driver, power mode, audio and frame policy; keep captures and input-file polling outside measured intervals.
- [ ] Add interval selection/snapshot deltas and safe accounting for all active guest CPUs, OS CPU/private-memory metrics and GPU duration. Reconcile nested callbacks and distinguish waits from useful CPU work.
- [ ] Measure allocation distributions/contention and redundant GL values; choose pools or state filtering only from the results. Preserve object reuse, invalid-call errors, presentation state, guest permissions and cross-thread frees.
- [ ] Collect translator compilation/cache/invalidation evidence before modifying read-only memory paths, halt checks or code-cache sizes.
- [ ] Check O11/O12 with targeted startup and nested-JNI workloads before implementing them.
- [ ] Compare against an emulator only with equivalent game/content/save, effective graphics quality, audio/offline behavior and a recorded ABI/translation/acceleration path. No such comparison exists yet.

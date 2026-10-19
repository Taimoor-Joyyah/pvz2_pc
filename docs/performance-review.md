# Performance review and optimization roadmap

Reviewed 19 October 2026 against commit `f8fbe4c62495126010e927343d7ca0ffdb186af2`.

This is a source review supported by existing local session reports, not a new performance benchmark. No runtime code, settings or profiles were changed, and no build or gameplay session was run for this review. Historical reports came from earlier implementations and different interactions; they cannot establish the current bottleneck or an improvement between versions. Source locations below refer to this revision; function names are the durable lookup keys.

## 1. Assessment

The best initial opportunities are to remove repeated work around guest imports and graphics calls, then improve guest heap allocation and frequently used libc handlers. JIT memory access is a potentially important but higher-risk area. The actual division of gameplay time between these areas is **not measured yet**.

This runtime already avoids booting an Android system and implements selected Android/Java services directly on Windows. That gives it a plausible route to lower startup cost, background CPU use and memory consumption. It still translates the original game's ARM64 instructions to x64 and translates GLES through ANGLE/D3D11. A native Windows source port's performance is not an appropriate expectation for this architecture.

Outperforming an Android emulator is a benchmark goal, not an established result. Modern emulators can use hardware graphics acceleration and run matching-architecture guest code through a hypervisor; they are not necessarily interpreting an entire device. Their handling of this game's ARM64 code must be recorded in any comparison. See [Android's acceleration documentation](https://developer.android.com/studio/run/emulator-acceleration). The opportunity to win by removing system overhead is an architectural inference, not a measured speedup.

Release optimization is already enabled: `/O2 /Ob3 /Oi /Ot`, `/GL /LTCG`, `/OPT:REF` and `/OPT:ICF`. The existing Release build files confirm these settings. The pinned Dynarmic configuration also defaults to its safe optimizations. Enabling unsafe floating-point or memory-ordering shortcuts is not a suitable first step. References: [CMakeLists.txt](../CMakeLists.txt), lines 13–41; [dependency pin](../cmake/Dependencies.cmake), lines 17–23; pinned Dynarmic `A64/config.h`, `optimizations` and `unsafe_optimizations`.

## 2. What the existing measurements establish

The earlier review's **8.5 million `memcpy` and 3.7 million `malloc` calls are verified** in `out/reports/audio-focus-after.json`: exactly 8,500,066 and 3,725,663, respectively, over a 1,200-frame session. These are import counts, not CPU-time measurements.

Selected historical evidence:

| Report | Frames | Mean / p50 / p99 loop time | Main guest CPU `memcpy` / `malloc` |
| --- | ---: | --- | --- |
| `out/reports/audio-focus-after.json` | 1,200 | 46.55 / 34.51 / 202.33 ms | 8,500,066 / 3,725,663 |
| `out/reports/offline-installed.json` | 1,200 | 44.91 / 34.45 / 86.84 ms | 8,172,391 / 2,828,826 |
| `out/reports/offline-gameplay.json` | 12,000 | 35.81 / 34.41 / 37.65 ms | 9,062,669 / 6,434,761 |

The 12,000-frame report includes 8,324,039 main-CPU `gl*` calls. Other important counts are `memcmp` 16,338,283; `memset` 6,367,307; `setjmp` 5,015,083; `wcscmp` 4,604,003; and `strlen` 3,105,324. Prioritizing only `memcpy` would miss other frequently exercised paths. Large buffers, long comparisons and short operations have different costs; counts alone cannot rank them.

There are important measurement boundaries:

- Import/instruction counts cover the main guest CPU's lifetime, including constructors, initialization and shutdown. `Runtime::report()` reports only `cpu.statistics()`, omitting the separate guest CPUs for pthreads and managed callbacks. The audio callback workload is therefore absent from those CPU totals.
- `Cpu::statistics()` assigns counters by trap name rather than accumulating them. Repeated names such as the JNI `CallMethod` variants overwrite earlier entries, so published JNI import counts and their summed total undercount actual transitions. Report unique trap IDs or sum matching names. The named libc/GLES counts above do not have that collision.
- Frame timing begins **after** library initialization and `adapter->start()`. It includes event processing, adapter sleep, lazy loading performed during draw calls, presentation preparation, capture/readback/PNG writing when requested, and `eglSwapBuffers`. The last bounded frame also waits for callbacks. It excludes pre-loop initialization and shutdown.
- The installed/gameplay reports have VSync enabled and `fps_limit=0`. Zero means **game controlled**, not unlimited: `Pvz2::frame()` still sleeps at the rate requested by the game. A median near 34 ms does not prove that useful CPU work takes 34 ms, nor that 60 FPS is attainable without changing game behavior.
- Reports have no per-handler time, copy-size histogram, GPU duration, code-cache occupancy/flush count, or read-callback count. Whole-session percentiles mix loading, menus and gameplay. These reports are compatibility evidence and optimization leads, not controlled gameplay baselines.

References: [frame loop/reporting](../src/launcher/main.cpp), lines 275–366; [frame pacing](../src/games/pvz2.cpp), lines 572–575 and 685–693; [CPU counters](../src/guest/cpu.cpp), lines 245–265 and 367–376; [runtime report](../src/android/runtime.cpp), lines 318–374. Reports under `out/` are ignored local artifacts; the essential values are preserved here.

## 3. Optimization candidates

Priorities below express the recommended investigation order and implementation risk, **not measured percentages of frame time**.

### O1 — Cache graphics entry points and remove successful-path diagnostics

**First implementation candidates; relatively small changes.** Every forwarded GLES/EGL call goes through `Graphics::Impl::call/ecall`, which calls `Library::symbol`; that constructs a string and calls `GetProcAddress` each time. Graphics wrappers also construct the unavailable-function error string on successful calls. Both guest graphics calls and host presentation calls pay this cost. The earlier graphics-caching finding is confirmed.

Resolve required functions once into correctly typed, per-graphics-instance pointers after loading the selected ANGLE DLLs. Resolve optional extensions at the appropriate initialization stage. Rebuild the table if DLLs/context ownership changes; fail clearly for missing required functions. Avoid replacing DLL lookup with another string-keyed lookup on every draw.

There is a second, independent cost in **all ordinary import dispatches**: `TrapTable::at()` passes `std::format("Unknown guest trap {}", id)` to `require()`. C++ evaluates that message before `require()` even when the ID is valid. Construct it only on failure. Apply the same treatment to graphics lookup errors and hot JNI reference checks while preserving their checks and diagnostics. Measure the dispatch case separately from the graphics case.

Verify with CPU samples/host allocation samples, then compare the same gameplay interval before and after each change. Existing graphics smoke tests must still pass. References: [graphics helpers](../src/android/graphics.cpp), lines 67–77; [DLL lookup](../src/host/windows.cpp), lines 385–387; [trap lookup](../src/guest/cpu.cpp), lines 19–21; [JNI checks](../src/android/jni.cpp), lines 168–176; [`require`](../include/acp/common.hpp), lines 37–39.

### O2 — Reduce guest-to-host dispatch and repeated entry-point lookup

**High potential reach; measure before invasive translator work.** An imported function executes an ARM64 SVC stub, halts Dynarmic, returns through `Cpu::call()`'s loop, updates counters and `current_import`, invokes a `std::function` handler, and resumes the JIT. The loop also checks cancellation, worker failure, budget and deadline. The actual memory operations already execute natively: both guest `memcpy` and `memmove` use host `std::memmove`, and `memset`/`memcmp` use host CRT routines after range validation. Replacing them with another native routine does not remove the boundary.

Start with cheap changes: preclassify handlers instead of repeatedly comparing import-name strings; avoid copying names except where required; measure counter/checkpoint costs. Benchmark dispatch with a minimal guest loop and a minimal handler, then with realistic copy sizes. Separate transition cost, validation cost and useful handler work.

There is also avoidable **host-to-guest** work once per frame: `Pvz2::frame()` resolves `glClear` with `Linker::symbol()` and enters the guest just to call that bridge. Symbol lookup scans loaded ELF symbol vectors before checking host-provided symbols. Cache the address after initialization, or expose a host graphics helper preserving the same semantics. Cache the resolved native draw entry point too, with a defined invalidation rule for changed JNI registrations. An indexed linker symbol table can also reduce relocation/startup cost while preserving guest-definition precedence and load order.

If transitions remain dominant, investigate specialized leaf bridges for handlers guaranteed not to reenter guest code. This requires translator integration and ABI/state tests; it is not simply calling a Windows function from an ARM64 address. The current halt-before-handler design deliberately permits callbacks. Register/SIMD snapshots in `Cpu::call()` occur at host-to-guest call entry, **not afresh for every import**; removing them globally would break nested calls.

References: [SVC/dispatch/call state](../src/guest/cpu.cpp), lines 165–168 and 192–266; [Bionic memory handlers](../src/android/bionic.cpp), lines 354–374; [adapter call/frame](../src/games/pvz2.cpp), lines 85–100 and 685–693; [symbol lookup](../src/guest/linker.cpp), lines 40–52; [native registration lookup](../src/android/jni.cpp), lines 251–267.

### O3 — Replace expensive small-allocation bookkeeping

**Likely CPU and memory opportunity; medium implementation risk.** `Memory::allocate/release` share one recursive mutex across guest CPUs. Allocation uses ordered maps/sets for live blocks and free-range/size indexes, splits/coalesces ranges, updates page metadata, and clears every returned block. `realloc` always allocates, copies and frees for a nonzero requested size, even when an existing block could be retained. The previous review's allocation concern is confirmed; the lock is a shared **recursive mutex**, not a shared-reader lock.

Measure allocation-size/lifetime histograms, bytes cleared/copied, lock wait time, metadata memory and peak committed memory. Introduce size-class pools/free lists for common small blocks inside the guest arena; retain an appropriate path for large/page-aligned mappings. Consider in-place realloc and batch metadata management. Thread-local caches may help only if contention is measured, and must support cross-thread free and bounded retention.

`malloc` need not clear reused memory, whereas `calloc`, anonymous mappings, and multiple internal callers require zero initialization. Introduce an explicit uninitialized heap path; do not remove clearing globally. For example, `Memory::string(text)` currently relies on allocation clearing for its trailing NUL. A normal host heap pointer cannot replace a guest address outside the arena contract.

Memory deserves its own acceptance metric: the gameplay report ends with 1,309,353 live guest allocations and 731,517,696 live bytes (697.6 MiB), excluding host metadata, archives, ANGLE and JITs. Freed heap ranges remain committed/reusable rather than being decommitted. This is retention, not proof of a leak. The 4 GiB arena is a virtual reservation, not 4 GiB of resident RAM. Consider trimming wholly unused pages only after accounting for neighboring live suballocations, permissions and stale translations.

References: [allocator](../src/guest/memory.cpp), lines 13–127 and 173–182; [allocator metadata/lock](../include/acp/guest.hpp), lines 53–63; [malloc/realloc](../src/android/bionic.cpp), lines 323–350.

### O4 — Optimize validated string scans and bulk ABI state access

**Focused follow-up to profiling.** Guest `strlen/strnlen/__strlen_chk` still call `m.load<char>` for every byte. Wide-string comparison similarly validates/loads individual 32-bit units. By contrast, `Memory::string()` already scans validated page chunks using `memchr`; it should not be described as byte-by-byte.

Use page-bounded scans/comparisons that validate each chunk once, stop at NUL or the requested limit, and avoid allocating host strings just to return a length or comparison. The current bounded comparison/duplication handlers first materialize full strings and then truncate them; scanning only the permitted prefix can reduce work and improve boundary behavior. Preserve fortified limits and fault behavior. Android wide characters are 32 bits; Windows `wcs*` functions cannot be substituted directly.

`setjmp` is another significant candidate: its handler validates/stores each saved register separately, despite receiving a contiguous jump buffer. Validate the required region once and marshal the same Android layout in bulk. Preserve register widths, saved floating-point state, stack/return addresses and `longjmp` behavior. Do not replace guest jump buffers with host CRT jump buffers.

References: [string handlers](../src/android/bionic.cpp), lines 390–420 and 549–569; [chunked string reader](../src/guest/memory.cpp), lines 159–171; [jump-buffer handlers](../src/android/bionic.cpp), lines 1829–1848.

### O5 — Reduce graphics state traffic and presentation overhead

**Potential submission/GPU savings; establish which side is expensive.** The gameplay report contains 2,370,682 `glTexParameteri` calls, 1,598,220 attribute disables, 958,932 attribute enables, 958,932 attribute-pointer updates and 319,644 draws. Repeated state values are plausible, but the report does not show values and cannot prove redundancy.

After O1, instrument repeated values. A small state cache can skip proven redundant host calls, keyed correctly by texture object/unit/target, VAO/attribute and context. Texture deletion/name reuse, invalid-call error behavior, and host presentation changes must not make it stale. Filtering inside a handler saves ANGLE submission work but **still pays the guest SVC transition**. Reducing that transition requires a guest/translator change; do not promise it from host-side filtering.

`finish_frame()` queries five pieces of GL state, clears the desktop target, blits the stable guest framebuffer, then restores state every frame. Shadowing the state changed by the bridge may avoid queries; skipping unnecessary clears may help when every destination pixel is overwritten. Validate both full coverage and letterboxing. These queries are extra CPU/driver work; a GPU synchronization cost has not been demonstrated. The extra surface/blit supports resizing and correct input mapping and cannot simply be deleted.

Measure GPU work separately from graphics submission and swap waiting. Lowering **startup render resolution and relaunching** can reduce pixel/render-target work if GPU bound. Resizing the window alone keeps the guest surface stable, and the game can retain its own intermediate target dimensions. Confirm effective `graphics.guest_width/height` and viewports, not just configured custom dimensions. Reduced resolution will not remove CPU translation or per-call overhead. Preserve the mutable native view-scale behavior.

References: [graphics state forwarding](../src/android/graphics.cpp), lines 159–189 and 283–307; [presentation](../src/android/graphics.cpp), lines 418–492; [stable surface policy](../src/games/pvz2.cpp), lines 648–656 and 698–705; [graphics regression checks](../tests/graphics_smoke.cpp).

### O6 — Investigate JIT memory paths and code-cache behavior

**Potentially broad impact; higher risk and currently unquantified.** Each guest CPU has a 32 MiB configured code cache and an 8 MiB guest stack. Read/write pages already use Dynarmic's direct page table; read-only pages have null entries and use checked data callbacks. Instruction fetch through `MemoryReadCode` is part of block translation, not a callback for every execution of an already translated instruction. Callback, compilation and cache-flush costs are separate questions.

Measure per-CPU read/write/code-fetch callbacks, compilation time, invalidations, cache occupancy/flushes and guest PCs for expensive translated blocks. Some cache instrumentation may require a small change to the pinned Dynarmic dependency. Increase cache size only if recompilation is material: larger per-thread caches consume memory and do not automatically fix host instruction-cache misses. Sharing translated code between CPUs is a substantial translator-design change, not a safe configuration toggle.

Investigate cheaper checked callbacks and, only with evidence, an appropriately protected fast-memory path. `Memory::protect()` currently changes logical metadata; the committed Windows backing remains `PAGE_READWRITE`. Populating read-only entries in the existing read/write page table, or using the arena as unchecked fast memory, would bypass guest write protections. Immutable-data optimizations also need invalidation when permissions/mappings change.

`check_halt_on_memory_access=true` adds halt checks after guest data accesses, and cycle counting supports instruction budgets. These may cost time, but removing them requires another way to guarantee fault propagation, cancellation and bounded execution. Preserve the exclusive monitor and cross-page access behavior. References: [JIT configuration/callbacks](../src/guest/cpu.cpp), lines 58–159 and 235–249; [page-table/protection logic](../src/guest/memory.cpp), lines 44–50 and 117–157; pinned Dynarmic `A64/config.h`.

### O7 — Reduce background callback, JNI and thread-lifetime costs

**Secondary until worker CPU/lock measurements exist.** Audio polls consumption on a managed callback worker with a 2 ms delay while playing. Enqueue copies PCM into a host buffer, then SDL accepts the submitted data. The gameplay report records 71,386 completed audio callbacks, outside the main CPU counters. Muting sets gain; it does not disable decoding, submission or callbacks.

Measure callback CPU, wakeups, underruns and lock waits. Consider pooled PCM buffers and consumption-driven or adaptive scheduling while preserving queue completion timing, callback order, pause/clear and buffer ownership. Do not call guest code directly from an SDL real-time audio callback. Audio-focus stream recreation was already repaired; the later reports record one engine/player/stream, so it is not a remaining optimization proposal.

JNI invocation copies method metadata, reparses argument signatures, constructs dispatch keys and allocates argument vectors; several operations share `state_mutex`. Cache immutable signature layouts and resolved dispatch where profiling justifies it. Respect receiver overrides and updated registrations, local-reference lifetime and reentrant callbacks. Narrow lock scope only after identifying contention.

Thread churn has a concrete retention issue: `pthread_join` joins but does not erase the guest thread/CPU; detach only marks it. Their JIT caches remain until runtime destruction. Stack/TLS/return-stub allocations also need explicit reclamation when CPUs are retired. Add completion-safe reclamation and processor-ID reuse if creation counts justify it; current reports do not establish churn as a gameplay bottleneck. A host thread count includes SDL/driver threads and cannot be multiplied by the per-guest-CPU cache size.

References: [audio scheduling/queue/pump](../src/android/audio.cpp), lines 12–24, 317–326 and 379–430; [SDL submission](../src/host/windows.cpp), lines 90–110; [JNI invoke](../src/android/jni.cpp), lines 382–448; [pthread lifecycle](../src/android/bionic.cpp), lines 1474–1538; [CPU resources](../src/guest/cpu.cpp), lines 70–79 and 189–191.

### O8 — Optimize startup, storage and diagnostics separately from frame throughput

**Conditional startup/memory/stutter work.** `Archive` reads an entire APK into a retained host byte vector; the runtime retains asset archives after opening them. ELF source bytes also remain alongside mapped guest segments. Evaluate file-backed archive access/mapping and releasing unnecessary raw image data after parsing, while retaining metadata and precomputed hashes needed by diagnostics. Do not assume compressed APK entries permit zero-copy extraction.

Path resolution rebuilds mount descriptions and canonicalizes both target and root per request. Cache stable mount metadata/root information and reuse descriptor paths, while retaining traversal/reparse-point checks. Persisted assets are cached within a session already. Native decompression is already used; moving it to host code is not a new optimization.

`Runtime::message()` writes synchronously while holding the shared state mutex and erases the first entry of a vector when its 1,000-entry log fills. A bounded ring and controlled log verbosity can reduce overhead if logs are hot. Persistence performs synchronous JSON/encryption and atomic write-through replacement. Dirty tracking/coalescing may reduce repeated unchanged saves, but required synchronization and crash-safe publication must remain. Hashing/import validation and final report generation mostly affect startup/shutdown, not steady frame time.

References: [archive ownership](../src/common/archive.cpp), lines 6–29; [ELF loading](../src/guest/linker.cpp), lines 15–27; [paths/assets/logging](../src/android/runtime.cpp), lines 112–201 and 208–213; [persistence](../src/games/pvz2_persistence.cpp), `Persistence::save`; [atomic JSON publication](../src/common/io.cpp), lines 113–133.

## 4. Costs that remain necessary

| Cost | What can change | What must remain for this architecture |
| --- | --- | --- |
| ARM64 execution on an x64 host | Faster translation, fewer transitions, optional native replacements for proven hot routines | Some translation/reimplementation is necessary. Eliminating it requires a compatible native game build or a substantially different port. |
| Android-to-Windows ABI adaptation | Cache metadata, batch validation, specialize safe leaf operations | Correct LP64/LLP64 layouts, 32-bit guest wide characters, JNI handles, register/stack arguments and callbacks. Guest libc++ objects cannot be passed to Windows C++ libraries. |
| Memory access and synchronization | Cheaper checks, pools, narrower locks, protected fast paths | Guest range/permission behavior, atomic operations, cross-thread ordering and invalidation. Callbacks themselves are an implementation choice; correctness is the requirement. |
| GLES rendering and desktop presentation | Cached entry points/state, reduced redundant work, measured backend/resolution changes | Correct draw order, uploads, render-target lifetime, aspect fit and state restoration. Replacing ANGLE is a separate backend project and not automatically faster. |
| Frame pacing and audio buffering | Better scheduling and lower redundant waits | Intended simulation timing and enough audio buffering to avoid underruns. Removing a sleep is not evidence of cheaper gameplay. |
| Content decoding and save durability | Cache immutable work, trim copies, avoid unchanged writes | Required data must be decoded and saves must reach their promised synchronization points. Losing content integrity or saves is not a performance improvement. |

The fixed Android surface is a current game-compatibility constraint, rather than an unavoidable law of all compatibility runtimes. Revisit it only if native intermediate-target rebuilding can be demonstrated reliable. Likewise, skipping Android services saves work only when observable game behavior remains correct; billing/network suppression changes functionality and cannot be counted as a like-for-like emulator speedup.

## 5. Measurement and implementation sequence

1. **Create a repeatable baseline before changing hot paths.** Use an isolated copy of a known save, the same APK/library hashes, ANGLE/driver, CPU/GPU, power mode, client and effective render dimensions, refresh rate, audio settings and frame policy. Record build ID/settings. Measure launch-to-play separately. Warm a repeatable combat scene, then collect at least three 60-second intervals; keep screenshots, live-input polling and console tracing outside measured intervals. Replay frames alone do not guarantee identical scene progress.
2. **Add low-overhead, opt-in stage measurements.** Separate events/input, adapter sleep, native draw call, presentation preparation, swap blocking, captures and final callback drain. Inside native draw, distinguish time in `jit.Run()` from import handlers; the former includes translation and memory callbacks. Collect CPU/wall time for every guest CPU, fix counter-name collisions, and add size histograms, sampled handler costs, allocation/lock metrics, working set/private commit and GPU/presentation data. Account for nested callbacks to avoid double counting. Snapshot/diff counters at interval boundaries; avoid timing every tiny call in the shipping path.
3. **Capture a CPU profile of the warmed interval.** Use optimized Release code with matching PDBs; the current Release setup has no explicit `/Zi` or `/DEBUG`, so add a profiling build variant that preserves its optimization flags. Show external code so CRT/ANGLE/driver costs are visible. Generated JIT code may require additional guest-PC mapping to attribute samples. Visual Studio supports selected time ranges and per-thread analysis; see [Microsoft's CPU profiler guide](https://learn.microsoft.com/en-us/visualstudio/profiling/cpu-usage). CPU samples alone do not establish GPU duration or distinguish every blocked wait.
4. **Implement O1 and O2's simple caching changes individually**, then select O3/O4/O5 according to measured time. Investigate O6 if translated execution/memory callbacks dominate; O7/O8 when worker CPU, memory, startup or stalls dominate. Do not preselect a translator rewrite or graphics backend replacement without evidence. Further host compiler/PGO tuning is secondary and cannot optimize the original ARM64 binary as if its source had been rebuilt.
5. **Accept changes against useful metrics and correctness.** Track active-work p50/p95/p99, deadline misses at the intended game rate, CPU time per gameplay second, GPU time, working set/private commit, startup and input response. At a capped rate, lower CPU/memory or fewer missed deadlines can be a success even when FPS stays constant. Run the relevant Release contract suites and independent ARM64 fixture; graphics changes also need the separate `graphics_smoke` executable and visual/gameplay checks. Memory/JIT changes must preserve cross-page faults, read-only/PROT_NONE behavior, mapping invalidation, atomics, nested callbacks, exceptions and cancellation. Verify saves and audio after lifecycle/thread changes.
6. **Compare against a properly configured emulator.** Run the same game/version/content, equivalent save and combat workload, effective render quality, audio and offline behavior on the same machine. Record emulator version, guest ABI/ARM translation path, graphics backend, VM acceleration and frame policy. Compare cold/warm startup, complete emulator process-tree CPU and memory, frame pacing, input response and sustained gameplay. An x86-native game build is a different comparison and should be labeled. No current repository report provides this comparison.

**First optimization work package:** baseline/stage telemetry, typed ANGLE entry points, lazy diagnostic construction, and cached frame entry points. Keep each change separately measurable. This is enough to decide whether allocator/libc work or deeper JIT/GPU work should follow; speedup estimates would be premature.

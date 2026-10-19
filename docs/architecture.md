# Runtime structure

The launcher imports a package, selects a game adapter, creates Android services, loads the guest's native libraries, resolves relocations, runs constructors and JNI initialization, and dispatches lifecycle and frame calls. Each layer has a separate CMake target.

| Target | Owns | Dependency direction |
| --- | --- | --- |
| `runtime_common` | Checked binary readers, ZIP access, hashing, JSON persistence | Vendor utilities and Windows hashing |
| `guest_core` | Guest address space, ARM64 JIT, ELF linker, import traps | `runtime_common`, Dynarmic |
| `host_windows` | Native window, events, replay decoding, audio output, web views, DLL loading | `runtime_common`, SDL3, WebView2 loader |
| `android_compat` | Android package formats, JNI, Bionic shims, GLES/OpenSL bridges, storage | `guest_core`, `host_windows` |
| `pvz2_adapter` | PopCap Java behavior, lifecycle sequence, event serialization | `android_compat` |
| `launcher_settings` | Shared JSON settings, validation, Windows argument quoting and process launch | `host_windows` |
| `game_application` | Console/GUI-independent commands and game startup | `pvz2_adapter`, `launcher_settings` |

`native_sample` and the generic runtime tests do not link `pvz2_adapter`.

## Guest/host ABI boundary

Guest addresses are 64-bit Android addresses in a separate 4 GiB virtual arena. They are never callable Windows pointers. Memory access translates and checks the requested range. Writable pages have a Dynarmic page table entry; executable/read-only pages use checked callbacks. ELF executable permissions are sealed after relocation.

Imports point at generated ARM64 `svc`/`ret` stubs. An SVC halts the JIT; host dispatch runs after the JIT returns. This permits a host service to call back into guest code without recursively entering a running JIT. CPU calls preserve registers and SIMD state, marshal eight integer arguments into registers and remaining arguments onto the guest stack, and enforce execution budgets. Guest faults and unimplemented instructions become diagnostics.

Windows uses LLP64, while Android AArch64 uses LP64. `long`, `wchar_t`, `FILE`, `va_list`, JNI handles, pthread objects, and zlib state require explicit translation. The APK's ARM64 libc++ executes as guest code, retaining its C++ ABI. Host files, synchronization objects, and compression state stay behind guest handles. This does not provide a security sandbox for adversarial native code.

## Android services and adapters

`Runtime` provides the service registry and isolated storage. `Jni` manages guest vtables, references, Java member handlers, native registrations, and copied arrays/strings. `Graphics` marshals GLES calls to ANGLE. `Audio` implements the required OpenSL ES object interfaces and queues PCM to host audio.

JNI local frames, pending exceptions, and VM attachments are associated with each guest thread. A managed callback worker has its own guest CPU and JNI environment, and schedules delayed SDK timers without requiring a frame-loop poll. Native callbacks do not hold the JNI registry lock while guest code executes. Encoded final scalar and string constants can be read from the APK's DEX files on demand; this does not execute Java methods or class initializers. Java shell methods still need explicit implementations. A worker failure is retained as the primary error and checked at CPU checkpoints and interruptible synchronization waits so other threads can produce a report and shut down.

Private `/data/data/PACKAGE` and `/data/user/0/PACKAGE` map to the package's userdata directory. External `/sdcard/Android/data/PACKAGE` maps under its separate `external/Android/data/PACKAGE` tree. OBB paths and `/data/app/PACKAGE` map to the immutable installation. Nimble SDK settings use an adapter-owned `pc-runtime/nimble` namespace with typed values, leaving the native game's files intact. Its encrypted settings use host data protection bound to the Windows user, not Android's serialized file format; transferring these encrypted records to another user or computer may require migration.

OpenSL ES players each own a host audio stream. Queue completion follows consumption by that stream, and callbacks execute outside the audio registry lock. Android music activity reflects the runtime's playing PCM streams. Bluetooth and headset routing APIs report unavailable because playback uses SDL's default desktop output, without Android-managed routes. Audio reports record actual submitted PCM and unsupported interface requests independently of graphics or game initialization.

The PVZ2 adapter supplies PopCap classes and native lifecycle bindings. It uses JNI registrations or exported JNI names, rather than fixed offsets in the supplied game binary. Its input codec is derived from the supplied APK's `AndroidUIEventManager` serialization and has separate protocol tests. Touch identifiers use Android pointer ID plus one, and timestamps use monotonic seconds despite the Java field's `MS` suffix. The adapter preserves the native-owned bytes following the event count in the input-buffer header.

The launcher can run a static input replay or accept appended live input for regression investigation. Both produce ordinary host events and pass through the adapter's normal input codec; neither edits native game state. Reports record the absolute frame schedule. Named profiles provide separate saves while reusing immutable installed content. Optional `GameAdapter::quit_requested()` lets Java shell exit requests leave the host frame loop and run the normal background/save sequence.

Google Play Games operations stay signed out in `pvz2_play`. `pvz2_billing` implements only unavailable-billing responses: catalog queries return null, transaction lists are empty, and purchase/grant/finalize/recovery requests return the SDK's NOT_AVAILABLE error. Refresh and restore notifications complete asynchronously with failure. There is no transaction simulator, receipt generator, offline catalog loader, INI parser or billing-ledger access.

The adapter also reproduces work done outside native code in the original Java renderer: clearing the color buffer before each native draw and respecting the requested frame rate. The reusable Bionic formatter handles ARM64 variadic arguments and translates Android's 32-bit wide characters explicitly, including Unicode code points and truncation errors.

Desktop focus leaves the EGL surface and native application alive. The supplied Java shell's `TryGainFocus`/`TryLoseFocus` paths notify audio focus separately from application activation. The adapter therefore calls `Native_NotifyAudioLostFocus`/`Native_NotifyAudioGainedFocus` only on actual focus transitions. Application activation runs during startup and deactivation during shutdown; ordinary refocus must not recreate Wwise playback or replay the application's music-start behavior.

The launcher GUI and both game entry points share the same configuration codec. Startup applies the window mode before EGL creation and reads the client dimensions before `Native_GameAppInitialize`. ANGLE owns a child HWND sized to the parent client area so its image cannot overlap the nonclient caption. The compatibility renderer maps guest framebuffer zero to a host-owned render target and presents it to that child while restoring guest GL state. A fixed custom resolution or the startup client resolution stays stable for the session, because this APK caches intermediate targets across surface-change callbacks. Presentation and touch mapping share an aspect-fit viewport.

`AndroidSurfaceView` exposes the native renderer's mutable view-scale factor, matching the supplied DEX (initial value 0.5, setter/getter and `Graphics_CanSetGLViewScaleFactor=true`). Ignoring that setter caused cropped native UI and incorrect scaling even when the desktop framebuffer dimensions were correct. The bridge preserves the JNI float bits and lets the native game choose its internal render targets.

The initial loader handles little-endian AArch64 ELF libraries with section tables and the RELA relocation types used by the supplied package. Emulated TLS is covered by the native fixture; native ELF TLS descriptors still require implementation. Packed relocations, dynamic guest library loading after startup, and additional Android services also require further implementation. Required missing behavior must stop with a useful diagnostic rather than report fabricated success.

## Qualification

Tests distinguish CPU/ABI behavior, Android service contracts, adapter protocols, real-library constructors, graphics initialization, rendered frames, and completed gameplay. None substitutes for the next. An imported package is unverified until its required stages pass. No future APK version is automatically declared supported.

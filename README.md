# Android PC compatibility runtime / PVZ2

A C++23 Windows x64 runtime for loading ARM64 Android native games. The Android compatibility code and host services are independent of the PVZ2 adapter. This repository contains no game binaries or assets in its source distribution.

**Development status:** the runtime is experimental. A successful build or CPU test is not proof that PVZ2 is playable. See [the implementation status](docs/status.md) for the current validation boundary and remaining work.

## AI Disclosure

This project is developed with substantial assistance from OpenAI Codex under the maintainer's direction. AI assistance has been used to write and modify first-party runtime code, tooling, tests and documentation, investigate bugs, run builds and automated tests, and perform limited interactive gameplay checks.

Validation results apply to the specific tests and sessions documented in [the implementation status](docs/status.md) and [the issue tracker](docs/issues.md). They do not establish complete Android compatibility or verification of every gameplay path. Third-party libraries and original game content have separate provenance; see [the third-party notices](THIRD_PARTY_NOTICES.md).

## Download

The [GitHub releases](https://github.com/Taimoor-Joyyah/pvz2_pc/releases) provide source archives and a portable Windows x64 distribution. The first release, `v0.1.0-alpha.1`, is an experimental prerelease. Extract the entire Windows ZIP and open `pvz2pc-launcher.exe`. You must supply your own matching game APK/OBB and install the Microsoft Edge WebView2 Runtime for HTML dialogs. See the [release guide](docs/releases/v0.1.0-alpha.1.md) for setup and limitations.

## Build

Install Visual Studio's C++ desktop tools (including the Windows SDK), **MSVC toolset 14.51.36231 / compiler 19.51.36257.0**, CMake 3.30 or newer, and Git. The validated host build uses CMake 4.2.1. From PowerShell:

```powershell
.\tools\build.ps1 -Configuration release -Test
```

The helper locates MSVC and Ninja through `vswhere`. First-party targets require C++23 and use MSVC's dedicated `/std:c++23preview` mode. Dependencies retain their own language settings. The first configure downloads pinned Dynarmic, Boost headers, SDL3, miniz, and nlohmann/json sources. Subsequent builds reuse the download cache.

For CLion, select the Visual Studio x64 toolchain and the `windows-release` CMake preset. Release enables `/O2 /Ob3 /Oi /Ot`, whole-program optimization (`/GL /LTCG`), reference removal and identical-code folding. Floating-point semantics remain precise.

The build produces three executables in `out/build/release`:

- `pvz2pc-launcher.exe`: native Windows configuration and launch GUI.
- `pvz2pc-game.exe`: Windows-subsystem game executable, with no console window.
- `pvz2pc.exe`: console executable with the same game implementation and diagnostic commands.

The compiler version is checked during configuration. An explicit `-DACP_ALLOW_UNPINNED_TOOLCHAIN=ON` CMake override permits testing another compiler version; that build needs separate validation.

The current issue list and validation sessions are tracked in [docs/issues.md](docs/issues.md).

Implemented optimizations, recorded before/after measurements, validation limits and remaining opportunities are tracked in [docs/performance-review.md](docs/performance-review.md). Use `tools/run-performance.ps1` and `tools/compare-performance.ps1` to record another pass. Add `--performance-profile` to a game run for stage/JIT/import wall timings, or build with `tools/build.ps1 -ProfileSymbols` for optimized profiling symbols.

## Install

After the Release build, package the graphics DLLs once, then install the portable application:

```powershell
.\tools\package-angle.ps1
cmake --install out/build/release --config Release --prefix out/install
```

Open `out/install/pvz2pc-launcher.exe`. The install contains all three executables, ANGLE, app-local MSVC redistributable DLLs, documentation and dependency notices. The `windows-release` preset also sets `out/install` as the default prefix, so `cmake --install out/build/release --config Release` works directly. Use an absolute `--prefix` to choose another directory. `--component Runtime` optionally limits installation to the application component; dependency SDKs, headers and static libraries are excluded from the default install as well.

Existing `launcher.json`, game content, profiles and reports are not copied or overwritten. Import your own APK/OBB separately and select its runtime root in the launcher. The WebView2 Evergreen Runtime is still required for HTML dialogs. ANGLE is verified against the recorded build before installation; configure `ACP_ANGLE_DIRECTORY` to use a different packaged directory, and explicitly set `ACP_VERIFY_ANGLE_INSTALL=OFF` when installing another ANGLE build.

## Google Play Games and purchases

Google Play Games and billing are disabled. Gameplay uses the selected local profile without a Google account. Purchase, grant, finalize and recovery requests return billing-unavailable errors. No simulated purchases, receipts, offline product catalog or billing ledger are implemented.

Legacy `game.ini` files and old billing records are ignored and left untouched. All supported application settings are in the launcher's JSON configuration.

## Inspect and import your game

```powershell
$runner = '.\out\build\release\pvz2pc.exe'
& $runner inspect --apk '.\com.ea.game.pvz2_row.apk' --out '.\out\inspection.json'
& $runner import --root '.runtime' --apk '.\com.ea.game.pvz2_row.apk' --obb '.\Android\obb\com.ea.game.pvz2_row\main.1063.com.ea.game.pvz2_row.obb'
& $runner list --root '.runtime'
& $runner run --root '.runtime' --probe --out '.\out\probe.json'
```

`--probe` loads and relocates the real ARM64 libraries, executes their static constructors, and invokes JNI initialization in the adapter's specified library order. It uses isolated probe storage and does not start a gameplay session. Unsupported imports become diagnostic traps that stop execution when called. Reports list these gaps even when a particular probe does not exercise them.

Repeat `--apk` for matching split APKs during inspection or import. `--data DIR` accepts the **contents of the game's Android/data/PACKAGE directory** (usually a directory containing `files/`). It maps to `userdata/PACKAGE/external/Android/data/PACKAGE`, separately from private Android files. Re-importing the same installation can add downloaded content; identical files are accepted and conflicting files are rejected before copying. Use a new `--root` to inspect conflicting content without replacing saves. Imports preserve the original APK and OBB, verify copied hashes, and only activate a completed installation. OBBs are optional for other native applications; PVZ2 requires its matching OBB at launch.

Without `--root`, the saved launcher configuration selects storage. Before configuration exists, executables inside a source checkout locate its imported `.runtime`; otherwise storage defaults to `%LOCALAPPDATA%/AndroidPcRuntime`.

## Desktop launcher

Open `out/build/release/pvz2pc-launcher.exe`. Select the game executable and imported installation directory, then choose the profile/version, window mode, client dimensions, render resolution, vertical sync, frame limit, mute and volume. An optional directory selects a different ANGLE build. **Save settings** persists the configuration; **Save & Play** also starts the selected executable. **Open game folder** opens its containing directory.

Settings are stored in `launcher.json` beside the executables. Both game executables read that file when opened directly, including with no arguments. The game does not require the launcher process to stay open. Paths saved by the GUI are absolute; hand-written relative paths resolve against the JSON file's directory. `--launcher-config FILE` selects a different configuration for either the launcher or the game. An executable copied to another folder needs the same JSON beside it or that explicit argument. Command-line options override settings for that invocation without rewriting the saved configuration.

Window modes are Windowed, Maximized and Fullscreen. **Client resolution at launch** measures the actual client pixels after applying the selected window mode, then initializes Android at that resolution. **Custom resolution** initializes Android at the specified render width/height independently of the desktop window. These choices are applied before native game initialization. Select Maximized or Fullscreen before launch to prepare rendering for the larger display.

The game's intermediate render targets are cached at startup. Live native surface reinitialization did not rebuild them reliably in this APK, so the game keeps its chosen render resolution until relaunch. Resizing and F11 still work through aspect-preserving presentation with matching input. To change render quality, save the new resolution and relaunch. The native game's asset resolution policy can still choose different intermediate dimensions; a higher desktop resolution does not create higher-resolution artwork.

The window-subsystem game writes logs to `ROOT/reports/game-TIMESTAMP-PID.log` and displays startup errors in a message box. The normal session JSON includes the effective settings and client/render geometry. Inspect the settings without starting a game with:

```powershell
.\out\build\release\pvz2pc.exe settings
.\out\build\release\pvz2pc.exe settings --launcher-config '.\my-launcher.json'
# One-run overrides also work with pvz2pc-game.exe:
.\out\build\release\pvz2pc.exe run --maximized --render-mode native --fps 60
.\out\build\release\pvz2pc.exe run --fullscreen --render-width 1920 --render-height 1080
```

## Graphics and session launch

The graphics bridge loads standalone x64 ANGLE DLLs. It does not start an Android emulator. To package the tested graphics DLLs from an installed Google Android SDK:

```powershell
.\tools\package-angle.ps1
& $runner run --root '.runtime' --frames 120 --out '.\out\session.json'
# Capture the runtime's final rendered framebuffer for a bounded session:
& $runner run --root '.runtime' --frames 120 --capture '.\out\frame.png' --out '.\out\session.json'
# Keep intermediate captures too, for startup and regression investigations:
& $runner run --root '.runtime' --frames 6000 --capture '.\out\frame.png' --capture-every 600 --out '.\out\session.json'
```

The packaging helper verifies the recorded hashes before copying. Another ANGLE build can be selected explicitly with `--angle-dir DIR`; `-AllowDifferentBuild` permits packaging an alternative for testing. A development fallback also locates ANGLE in the installed Android SDK. Distribute the packaged `angle` directory with the executable, together with the applicable dependency notices.

During a working game session, left click maps to touch, dragging maps to touch movement, Escape maps to Android Back, and F11 toggles fullscreen. Keyboard text events use the game's Java event protocol. These integrations require gameplay validation before a release is qualified.

Window focus changes use the native audio-focus lost/gained callbacks without restarting the active game application. Repeated focus notifications are ignored. The launcher resizes its fields and columns together, repaints child controls, and preserves its layout through minimize/restore and DPI changes.

The desktop window can resize freely. PVZ2 renders at the resolution selected before launch, scaled to fit the current drawable while preserving its aspect ratio; unused space is black. Mouse input follows the displayed image, clicks in the presentation bars are ignored, and resizing or losing focus cancels an active drag. Use `--width 720 --height 960` to choose the initial client size. ANGLE renders to a separate child window positioned at client origin, below the native caption and borders. Per-monitor DPI awareness keeps render sizes in physical pixels. Presentation requires ANGLE OpenGL ES 3, provided by the packaged build. Windows ghosting is disabled for both the launcher and game; long synchronous guest loading calls can still delay game input.

Use `--profile NAME` for separate save storage under `ROOT/profiles/PACKAGE/NAME`. The default profile stays under `ROOT/userdata/PACKAGE`. To reproduce input in a bounded regression run, pass `--input replay.json`:

```json
{
  "schema": 1,
  "events": [
    {"frame": 900, "type": "down", "x": 0.5, "y": 0.94},
    {"frame": 905, "type": "up", "x": 0.5, "y": 0.94}
  ]
}
```

Frames start at zero in the render loop, after native startup. Pointer coordinates range from 0 to 1 and follow the drawable size. Events must be ordered by frame; supported types are `down`, `move`, `up`, `back`, `text` (with `text`), `key` (with Android `key_code` and optional `released`), `focus` (with `focused`) and `quit`. Session reports include delivered and remaining replay events. Replays depend on the game's current screen and save state; they do not assert that a level was completed. They cannot enter choices into the separate HTML consent dialog.

For interactive regression investigation, add `--live-input`. Start with `{"schema":1,"events":[]}`, then append events to the array by atomically replacing the document. Existing entries must remain unchanged. Each appended batch uses frame offsets from when the runtime reads it; an omitted `frame` means zero. For a tap, append a `down` with `frame:0` and an `up` with `frame:5`. The runtime polls the document every 100 ms and records the resulting absolute frame schedule in `input_replay.events` in its report. This diagnostic mode uses the same input path as desktop controls and does not alter game state directly.

## Updates and other games

Import each new version separately, then compare it with a previous installation:

```powershell
& $runner inspect --root '.runtime' --apk '.\new.apk' --compare 'PREVIOUS_INSTALLATION_ID' --out '.\out\update-report.json'
& $runner run --root '.runtime' --version 'INSTALLATION_ID' --probe
```

The comparison reports changed native-library hashes, added and removed imports, library dependencies, exported JNI names, and DEX native method signatures. APK sets receive an order-independent fingerprint. These are contract changes to review, not proof of binary compatibility. Older installations remain available. The first session under a different version backs up existing saves before that version opens them. New versions remain unverified until they pass runtime and gameplay regression testing.

For a new native game, implement `GameAdapter`, register its Java shell behavior, and select its native entry library. Add Android APIs to `android_compat` as reusable behavior, and keep title-specific classes and bindings in that game's adapter. See [architecture](docs/architecture.md) and the independent `native_sample` executable.

## Independent Android test application

The [offline compatibility audit](docs/offline-compatibility.md) lists the 27
formerly unresolved imports, their implementations and validation boundaries.
The [suppressed-function inventory](docs/suppressed-functions.md) lists disabled
online and unrelated APIs. Runtime JSON reports include importing libraries,
binding status, and suppression behavior/call counts.

```powershell
.\tools\build-android-fixture.ps1
.\tools\build.ps1 -Configuration release -Test
.\out\build\release\graphics_smoke.exe
.\out\build\release\webview_smoke.exe
```

The fixture helper obtains the pinned Android NDK r29, verifies its official checksum, and builds/signs an ARM64 NativeActivity APK from `examples/android_native`. An installed Android SDK with build tools 36.0.0, platform 36 or 36.1, and a JDK are required. `native_sample` imports this APK and runs its ARM64 code using `android_compat`, with no link to `pvz2_adapter`. It exercises actual guest libc++ exceptions, constructors, JNI, callbacks, TLS, threading, and persistent file writes. CTest reports the sample as skipped if its APK has not been built; a skipped test does not count as runtime validation.

The separately installed **Microsoft Edge WebView2 Runtime** is required for HTML dialogs. The build downloads the pinned WebView2 SDK and links its loader statically. The host web-view service renders the APK's original consent form, passes its messages back to the native SDK, and leaves age and agreement choices to the user. The embedded document stays offline; policy links open only when selected. `webview_smoke` checks HTML, JavaScript, message callbacks, and PNG preview capture. It is a separate interactive smoke test, outside CTest.

# Android PC compatibility runtime / PVZ2

A C++23 Windows x64 runtime for loading ARM64 Android native games. The Android compatibility code and host services are independent of the PVZ2 adapter. This repository contains no game binaries or assets in its source distribution.

**Development status:** the runtime is experimental. A successful build or CPU test is not proof that PVZ2 is playable. See [the implementation status](docs/status.md) for the current validation boundary and remaining work.

## Build

Install Visual Studio's C++ desktop tools (including the Windows SDK), **MSVC toolset 14.51.36231 / compiler 19.51.36257.0**, CMake 3.30 or newer, and Git. The validated host build uses CMake 4.2.1. From PowerShell:

```powershell
.\tools\build.ps1 -Configuration release -Test
```

The helper locates MSVC and Ninja through `vswhere`. First-party targets require C++23 and use MSVC's dedicated `/std:c++23preview` mode. Dependencies retain their own language settings. The first configure downloads pinned Dynarmic, Boost headers, SDL3, miniz, and nlohmann/json sources. Subsequent builds reuse the download cache.

For CLion, select the Visual Studio x64 toolchain and the `windows-release` CMake preset. The executable is `out/build/release/pvz2pc.exe`.

The compiler version is checked during configuration. An explicit `-DACP_ALLOW_UNPINNED_TOOLCHAIN=ON` CMake override permits testing another compiler version; that build needs separate validation.

## Google Play Games and purchases

Google Play Games is disabled. Gameplay and saves use the local profile; achievements and leaderboard calls follow the APK's signed-out behavior without requiring a Google account or Play Services. In-game achievement tracking still belongs to the original game.

The first regular launch creates `ROOT/game.ini` (`.runtime/game.ini` when using `--root .runtime`). Edit it, then restart:

```ini
[purchases]
mode=disabled
catalog_file=
```

`mode=disabled` rejects purchase attempts and reports billing unavailable. `mode=auto_complete` enables a **local purchase simulation**: requested purchases complete through the native game's purchase, grant and finalize callbacks. It charges no money, creates no Google or EA transaction, and labels its local records and receipts `local-test`. The ledger stays in the selected profile's `preferences/pc-runtime/billing.json`. Grant acknowledgements survive restarts, and completed consumables are excluded from restore. Existing installed game files and ordinary saves are not rewritten by this provider.

Use `--config PATH` to select a different INI. The optional `catalog_file` is a JSON array of offline product metadata, resolved relative to the INI:

```json
[
  {"sku": "MATCHING_GAME_PRODUCT_ID", "type": "consumable", "title": "Local test item"}
]
```

Types are `consumable`, `nonconsumable` or `subscription`; `title` and `description` are optional. Simulated prices are zero and labeled `Local test`. Product IDs must match the game's offers. This catalog supplies platform metadata; it does not download missing shop offers or create new game content. Direct purchase requests can also introduce a previously unknown SKU. Purchase callbacks and recovery have dedicated JNI tests; the complete in-game shop flow still needs validation with matching offer data.

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

Without `--root`, storage defaults to `%LOCALAPPDATA%/AndroidPcRuntime`.

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

```powershell
.\tools\build-android-fixture.ps1
.\tools\build.ps1 -Configuration release -Test
.\out\build\release\graphics_smoke.exe
.\out\build\release\webview_smoke.exe
```

The fixture helper obtains the pinned Android NDK r29, verifies its official checksum, and builds/signs an ARM64 NativeActivity APK from `examples/android_native`. An installed Android SDK with build tools 36.0.0, platform 36 or 36.1, and a JDK are required. `native_sample` imports this APK and runs its ARM64 code using `android_compat`, with no link to `pvz2_adapter`. It exercises actual guest libc++ exceptions, constructors, JNI, callbacks, TLS, threading, and persistent file writes. CTest reports the sample as skipped if its APK has not been built; a skipped test does not count as runtime validation.

The separately installed **Microsoft Edge WebView2 Runtime** is required for HTML dialogs. The build downloads the pinned WebView2 SDK and links its loader statically. The host web-view service renders the APK's original consent form, passes its messages back to the native SDK, and leaves age and agreement choices to the user. The embedded document stays offline; policy links open only when selected. `webview_smoke` checks HTML, JavaScript, message callbacks, and PNG preview capture. It is a separate interactive smoke test, outside CTest.

# Offline compatibility audit

Audit date: 2026-10-19. Supplied installation: PVZ2 13.5.1, version code 1063,
`com.ea.game.pvz2_row-1063-018621c34c07`.

The previous runtime reported **27 unresolved native imports**. These were normal
C/POSIX functions, including dependencies of the game's Android C++ library.
Suppressing them indiscriminately would break parsing, file streams or storage.
All 27 now resolve to explicit host implementations. The real-library probe and
the 2,400-frame game regression both report `unsupported_imports: []`.

An unresolved import count is different from a count of executed failures.
Many imports are only used on particular code paths. The independent NDK fixture
and `offline_compatibility` tests exercise the added behavior even when the game
session does not call it.

## The 27 imports

`Game` means `libPVZ2.so`; `C++` means `libc++_shared.so`. Library ownership comes
from the ELF imports, rather than a guess based on function names.

| Function | Required by | Implemented behavior |
| --- | --- | --- |
| `__cmsg_nxthdr` | Game | Bounds-checked ARM64 ancillary-buffer iteration; this pure helper sends no network traffic. |
| `chdir` | C++ | Changes the guest working directory within mounted storage; host cwd is untouched. |
| `fchmodat` | C++ | Directory-relative metadata operation; maps write permission to Windows' read-only attribute. |
| `fdopen` | Game, C++ | Wraps an existing virtual descriptor; checks access/mode and preserves ownership. |
| `fputwc` | C++ | Encodes a 32-bit Android wide character as UTF-8. |
| `fscanf` | Game | Reads formatted fields from a guest stream while preserving unread input. |
| `getwc` | C++ | Decodes UTF-8 to an Android UTF32 character; handles EOF and invalid sequences. |
| `mremap` | Game | In-place shrinking and movable anonymous growth; preserves data and protection. |
| `openat` | C++ | `AT_FDCWD`, absolute paths and virtual directory descriptors; Android open flags and errors. |
| `realpath` | C++ | Checks an existing mounted path and returns a normalized Android path, with optional allocation. |
| `sendfile` | C++ | Copies file data, handles partial output, updates explicit offsets and preserves source position. |
| `sscanf` | Game, C++ | Integers, floating fields, characters, strings, scansets, widths, suppression and `%n`. |
| `strtold` | C++ | Exact decimal/hexadecimal conversion to IEEE binary128, including rounding, infinity and subnormals. |
| `strtold_l` | C++ | Same binary128 conversion under the runtime's C numeric-locale policy. |
| `swscanf` | Game | Formatted scanning with Android's 32-bit `wchar_t` and LP64 destinations. |
| `truncate` | C++ | Shrinks or extends an existing writable file, without creating a missing file. |
| `ungetwc` | C++ | UTF-8 stream pushback, including non-BMP characters and clearing EOF. |
| `unlinkat` | C++ | Directory-relative file/empty-directory removal with `AT_REMOVEDIR` checks. |
| `utimensat` | C++ | Android timespecs, `UTIME_NOW`/`UTIME_OMIT`, and directory-relative file timestamps. |
| `vsscanf` | C++ | Scanning through AAPCS64 `va_list` register-save and stack areas. |
| `wcstod` | C++ | Wide-to-double conversion, SIMD return value and UTF32 end pointer. |
| `wcstof` | C++ | Wide-to-float conversion, SIMD return value and UTF32 end pointer. |
| `wcstol` | C++ | Android's 64-bit signed `long`, base selection, overflow and end pointer. |
| `wcstold` | C++ | Wide input with a full binary128 result. |
| `wcstoll` | C++ | Signed 64-bit conversion and end pointer. |
| `wcstoul` | C++ | Android's 64-bit unsigned `long`, including its full unsigned range. |
| `wcstoull` | C++ | Unsigned 64-bit conversion and end pointer. |

## Additional offline fixes

- `fcntl(F_GETLK/F_SETLK/F_SETLKW)` now acquires real cross-instance byte-range
  locks. POSIX locks are owned by the guest process; closing any descriptor for
  that file releases that guest's locks. Shared locks, failed upgrades and
  interruptible waits have regression coverage.
- Locks use a hidden `<filename>.acp-lock` companion and Windows `LockFileEx`,
  coordinated by a named mutex. This keeps locks advisory: other descriptors in
  the same guest may still access the database. Runtime instances using this
  bridge coordinate with each other. External native database tools do not use
  this companion-lock protocol; stop the game before opening its database in one.
- `stat` and `fstat` report matching real file identities, sizes and timestamps.
  SQLite no longer receives the same zero inode for every file.
- `fsync` flushes the CRT stream and commits the descriptor. `ftruncate` returns
  `-1` and Android errno on failure instead of returning a Windows error number as
  a success-like positive result. `O_EXCL` protects existing files.
- Stream lookahead/pushback is shared with `fread`, `fgetc`, `fgets`, descriptor
  reads and seek/tell. Closing a file invalidates and frees its owned opaque
  handles. Invalid `fflush` handles return `EBADF`.
- Android `fopen` modes always select binary Windows streams, even for `r`, `w`
  and `a` without `b`. This preserves Ctrl-Z/CRLF bytes in streamed Wwise media
  and prevents newline insertion into guest-written files. Read, seek, write
  and append regressions cover all byte values and an explicit CRLF sequence.
- Binary128 printf formatting and mixed integer/floating stack arguments work;
  `%Lf` no longer raises the former unsupported-conversion exception.
- `PROT_NONE` retains its mapping so access can be restored. Remapping and
  unmapping invalidate translated code for retired pages.
- Numeric `inet_pton`/`inet_ntop` conversions remain available without networking.
  Pointer-returning failure paths return null, rather than an all-ones pointer.

## Disabled services

The complete per-function list is in [suppressed-functions.md](suppressed-functions.md).
Every runtime report includes `suppressed_functions`, with its category, return
behavior and call count. A count of zero means that disabled binding was installed
but not used during that session. `native_imports` separately records each native
symbol's importing libraries and whether it resolves to a host bridge, guest
library, explicit suppression or absent weak optional symbol.

The final installed-runtime inventory contains **168 disabled native/Java bindings**
(including distinct interface and implementation signatures). Refresh it from a
new session with:

```powershell
.\tools\report-suppressions.ps1 -Report out/reports/offline-installed.json -Output docs/suppressed-functions.md
```

| Category | Offline behavior |
| --- | --- |
| Socket transport | `-1`, `ENETUNREACH`; no socket or network request is created. |
| DNS/name resolution | Bionic `EAI_NONAME` **8**, null address results; host lookup returns null. The old negative Linux/glibc-style EAI value was incorrect for bionic. |
| Network status / proxy | Nimble status `NONE`, no Wi-Fi, null proxy; status redetection is a no-op. |
| PopCap HTTP transactions | One asynchronous native error callback per started transaction; release still performs native cleanup. |
| Billing | Permanently unavailable: empty transaction lists, absent catalog, NOT_AVAILABLE **103** for purchase/grant/finalize/recovery, failed catalog/restore notifications. No purchase completion or item grant is fabricated. |
| Advertising | The supplied SDK's DummyAdvertising behavior: not loaded, no reward, unavailable placement callback. |
| Analytics delivery | Disabled SDK backend. Existing local preferences and device/save identity still work; generic SQLite/storage is not disabled. |
| Google Play Games | Signed out; connection, achievements, score submission and leaderboard UI do not send requests. |
| Unrelated process/device APIs | Fork/exec/system, device/FIFO creation, process tracing/waiting, links and pipes return `ENOSYS`. Signal installation/raising also returns an explicit unsupported result. |
| Optional legacy metadata APIs | `chmod`, `fchmod`, `fchown`, `lchown`, `utime`, `utimes` return `ENOSYS`; supported metadata paths are `fchmodat` and `utimensat`. |

`poll` and `select` handle guest regular-file descriptors and timeout-only waits;
they are not blanket network stubs. No successful online connection or purchase
is reported to the game.

## Compatibility boundaries

This is an offline Windows compatibility layer, not a complete POSIX system.
Unsupported valid operations fail through their API contract rather than pretending
to succeed: changing append mode on an existing CRT descriptor (`fdopen("a")`
without an existing append flag, or `F_SETFL` append changes), fixed-address and
writable shared file mappings, file-backed mapping growth, and partial-range
`mprotect`/`munmap` are outside the current implementation. `F_GETLK` reports an
unknown remote owner as pid -1 and conservatively describes a conflicting lock
as a write lock. C numeric parsing uses a dot decimal separator; alternate numeric
locales and POSIX allocating/positional scanf extensions are not implemented.

Guest paths remain restricted to mounted storage; parent-traversal components and
host drive paths are rejected. Host-created symlink aliases are not a supported
guest `realpath` feature. Windows timestamp precision is 100ns, and permission
mapping does not reproduce Unix owners, groups, execute bits or ACLs.

Fatal guest assertions, invalid memory and unknown APIs in a different APK version
retain diagnostic failures. They are not turned into arbitrary successful return
values. Passing the recorded tests establishes the exercised paths; it does not
prove that every level or future version can never fail.

## Verification

- `out/build-offline-final.log`: final optimized Release build and seven CTest suites.
- `offline_compatibility`: all 27 bindings, scanf and varargs, Unicode, binary128,
  directory-relative file I/O, real file identity/times, cross-instance locks,
  remapping/protection, disabled-network results and suppression counters.
- Independent Android NDK r29 fixture: actual ARM64 calls on the main thread and
  callback worker, UTF32 file streams, scanning, binary128 parsing/formatting,
  register/stack argument mixing, native file operations and `mremap`.
- `out/reports/offline-game.json`: 2,400-frame isolated-save session; zero
  unresolved imports, no CPU fault, no worker error, one audio engine/stream
  through the focus replay. Menu and offline mode selection were visually checked.
  The earlier `TrackerPinDb` / `sqlite3_prepare_v2` disk-I/O failure is absent.
- Further gameplay observations and final validation results are recorded in
  [issues.md](issues.md).
- `out/reports/offline-gameplay.json`: 12,000 frames, restored Adventure / Ancient
  Egypt Day 3, native earned-coin Plant Food purchase, drag-to-use during combat,
  windowed/fullscreen transitions and planting in a chosen cell. Zero unresolved
  imports, no CPU fault or worker error, and one audio engine/stream. This session
  exercised the level; it did not establish level completion or broader worlds.
- `out/reports/offline-installed.json`: the final portable Release installation,
  using its own ANGLE DLLs, completes 1,200 frames with zero unresolved imports,
  no CPU fault or worker error, and all 168 suppression policies recorded.

ABI references: the pinned NDK r29 headers in `out/tools/android-ndk-r29`;
[bionic socket headers](https://android.googlesource.com/platform/bionic/+/a1112fd88/libc/include/sys/socket.h);
[SQLite POSIX locking hazards](https://www.sqlite.org/howtocorrupt.html);
[SQLite locking protocol](https://www.sqlite.org/lockingv3.html).

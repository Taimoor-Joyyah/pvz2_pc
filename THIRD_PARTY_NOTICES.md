# Third-party components

Pinned source revisions and releases are declared in `cmake/Dependencies.cmake`. Preserve the original dependency licenses when packaging binaries.

- [Dynarmic](https://github.com/lioncash/dynarmic): 0BSD, with its bundled dependencies' individual notices.
- [Boost](https://www.boost.org/users/license.html): Boost Software License 1.0.
- [SDL3](https://github.com/libsdl-org/SDL): zlib license and included third-party notices.
- [miniz](https://github.com/richgel999/miniz): its included MIT/public-domain notices.
- [nlohmann/json](https://github.com/nlohmann/json): MIT license.
- [ANGLE](https://chromium.googlesource.com/angle/angle/): BSD-style license and its bundled third-party notices. Development graphics DLLs can come from the installed Google Android SDK; their exact file hashes are recorded separately. Microsoft D3D compiler DLLs retain Microsoft's applicable redistribution terms.
- [Microsoft WebView2 SDK 1.0.3537.50](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3537.50): Microsoft's package license, including the statically linked loader. HTML dialogs use the separately installed Microsoft Edge WebView2 Runtime. The game supplies its own HTML and policies.

OpenSL ES interface identifier values and interface layouts follow the Khronos/Android headers. Reference: [Android Wilhelm](https://android.googlesource.com/platform/frameworks/wilhelm/). Their original Khronos permission notice is available in that source tree.

PVZ2 and its assets belong to their respective owners. The runtime source distribution does not include the supplied game APK, native game libraries, OBB, or save data.

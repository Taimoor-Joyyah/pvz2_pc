# First-party targets select C++23 locally; dependency language modes remain theirs.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(json)

set(MINIZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz URL https://github.com/richgel999/miniz/archive/2948eebafc5b9f1c69bebed80333e403a09bd62d.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(miniz)

# Dynarmic uses Boost headers only. Use an immutable Boost release archive.
FetchContent_Declare(boost_headers URL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2 SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(boost_headers)
set(BOOST_ROOT "${boost_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS A64 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dynarmic URL https://github.com/lioncash/dynarmic/archive/a41c380246d3d9f9874f0f792d234dc0cc17c180.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(dynarmic)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 URL https://github.com/libsdl-org/SDL/archive/0ae55ed462d62d661e3051f9ae0f2107d7652c26.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(SDL3)

# The SDK loader links statically; the separately installed Evergreen runtime renders HTML.
FetchContent_Declare(webview2
  URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3537.50/microsoft.web.webview2.1.0.3537.50.nupkg
  URL_HASH SHA256=5ea526bbd728adda0da4d31219267e96460494a427e4894c4e09d9f320f4b9aa
  DOWNLOAD_NAME webview2.zip
  SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)
add_library(WebView2Loader STATIC IMPORTED GLOBAL)
set_target_properties(WebView2Loader PROPERTIES
  IMPORTED_LOCATION "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${webview2_SOURCE_DIR}/build/native/include")

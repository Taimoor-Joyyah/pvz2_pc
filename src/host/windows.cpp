#include <acp/host.hpp>

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <SDL3/SDL.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <wincrypt.h>
namespace acp::host {
namespace {
Bytes transform_protected_data(std::span<const std::byte> input, bool protect) {
    require(input.size() <= MAXDWORD, "Protected data exceeds Windows size limit");
    DATA_BLOB source{static_cast<DWORD>(input.size()),
                     reinterpret_cast<BYTE *>(const_cast<std::byte *>(input.data()))};
    DATA_BLOB result{};
    auto success = protect ? CryptProtectData(&source, L"Android PC runtime", nullptr, nullptr, nullptr,
                                              CRYPTPROTECT_UI_FORBIDDEN, &result)
                           : CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr,
                                                CRYPTPROTECT_UI_FORBIDDEN, &result);
    require(success != FALSE, "Windows user data protection failed: " + std::to_string(GetLastError()));
    Bytes bytes(result.cbData);
    std::memcpy(bytes.data(), result.pbData, result.cbData);
    LocalFree(result.pbData);
    return bytes;
}
} // namespace
Bytes protect_data(std::span<const std::byte> input) {
    return transform_protected_data(input, true);
}
Bytes unprotect_data(std::span<const std::byte> input) {
    return transform_protected_data(input, false);
}
std::expected<CalendarTime, int> calendar_time(std::int64_t unix_seconds, bool utc) {
    using namespace std::chrono;
    if (unix_seconds < -62167219200ll || unix_seconds > 253402300799ll)
        return std::unexpected(75);
    try {
        auto instant = sys_seconds(seconds(unix_seconds));
        auto information = utc ? sys_info{} : current_zone()->get_info(instant);
        auto local = local_seconds(instant.time_since_epoch() + information.offset);
        auto day = floor<days>(local);
        year_month_day date(day);
        hh_mm_ss clock(local - day);
        auto first = local_days(date.year() / January / 1);
        CalendarTime result;
        result.fields = {
            static_cast<int>(clock.seconds().count()),    static_cast<int>(clock.minutes().count()),
            static_cast<int>(clock.hours().count()),      static_cast<int>(unsigned(date.day())),
            static_cast<int>(unsigned(date.month())) - 1, int(date.year()) - 1900,
            static_cast<int>(weekday(day).c_encoding()),  static_cast<int>((day - first).count()),
            information.save != minutes::zero()};
        result.utc_offset = information.offset.count();
        result.zone = utc ? "UTC" : information.abbrev;
        return result;
    } catch (const std::exception &) {
        return std::unexpected(75);
    }
}
FILE *null_stream() {
    FILE *stream{};
    return fopen_s(&stream, "NUL", "w+b") == 0 ? stream : nullptr;
}
float Viewport::guest_x(float value) const {
    return width ? (value - x) / width : 0;
}
float Viewport::guest_y(float value) const {
    return height ? (value - y) / height : 0;
}
bool Viewport::contains(float px, float py) const {
    return px >= x && py >= y && px < x + width && py < y + height;
}
Viewport fit_viewport(int dw, int dh, int gw, int gh) {
    require(dw > 0 && dh > 0 && gw > 0 && gh > 0, "Invalid viewport dimensions");
    auto scale = std::min(double(dw) / gw, double(dh) / gh);
    auto w = std::max(1, static_cast<int>(std::lround(gw * scale))),
         h = std::max(1, static_cast<int>(std::lround(gh * scale)));
    return {(dw - w) / 2, (dh - h) / 2, w, h};
}
namespace {
LRESULT CALLBACK render_surface_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}
class WindowsAudio final : public AudioStream {
  public:
    WindowsAudio(int frequency, int channels, float application_gain)
        : application_gain_(application_gain) {
        SDL_AudioSpec spec{SDL_AUDIO_S16LE, channels, frequency};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        require(stream_ != nullptr, SDL_GetError());
        if (!SDL_SetAudioStreamGain(stream_, application_gain)) {
            SDL_DestroyAudioStream(stream_);
            throw Error(SDL_GetError());
        }
    }
    ~WindowsAudio() override {
        SDL_DestroyAudioStream(stream_);
    }
    void submit(std::span<const std::byte> pcm) override {
        require(pcm.size() <= INT_MAX, "Audio buffer too large");
        require(SDL_PutAudioStreamData(stream_, pcm.data(), static_cast<int>(pcm.size())), SDL_GetError());
    }
    size_t queued_bytes() const override {
        return static_cast<size_t>(std::max(0, SDL_GetAudioStreamQueued(stream_)));
    }
    void pause(bool paused) override {
        require(paused ? SDL_PauseAudioStreamDevice(stream_) : SDL_ResumeAudioStreamDevice(stream_),
                SDL_GetError());
    }
    void clear() override {
        require(SDL_ClearAudioStream(stream_), SDL_GetError());
    }
    void gain(float gain) override {
        require(std::isfinite(gain) && gain >= 0.f && gain <= 1.f, "Invalid stream audio gain");
        gain_ = gain;
        require(SDL_SetAudioStreamGain(stream_, gain_ * application_gain_), SDL_GetError());
    }
    void application_gain(float gain) {
        application_gain_ = gain;
        require(SDL_SetAudioStreamGain(stream_, gain_ * application_gain_), SDL_GetError());
    }

  private:
    SDL_AudioStream *stream_{};
    float gain_ = 1.f, application_gain_ = 1.f;
};
class Windows final : public Services {
  public:
    ~Windows() override {
        close_webviews();
        if (window_)
            SDL_DestroyWindow(window_);
        if (initialized_)
            SDL_Quit();
    }
    void open_window(std::string_view title, int width, int height) override {
        require(!window_, "Window already exists");
        // Guest initialization can spend seconds in a single native call. Keep Windows
        // from substituting a ghost window while the runtime owns the actual surface.
        DisableProcessWindowsGhosting();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS),
                std::string("SDL initialization: ") + SDL_GetError());
        initialized_ = true;
        window_ = SDL_CreateWindow(std::string(title).c_str(), width, height,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        require(window_ != nullptr, std::string("Window creation: ") + SDL_GetError());
        static std::once_flag surface_class;
        std::call_once(surface_class, [] {
            WNDCLASSW type{};
            type.lpfnWndProc = render_surface_proc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"ACP_RenderSurface";
            require(RegisterClassW(&type) != 0, "Cannot register the render surface class");
        });
        auto parent = static_cast<HWND>(native_window());
        SetWindowLongPtrW(parent, GWL_STYLE, GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);
        render_window_ = CreateWindowExW(0, L"ACP_RenderSurface", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1,
                                         parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(render_window_ != nullptr, "Cannot create the client render surface");
        sync_render_window();
    }
    void *native_window() const override {
        require(window_ != nullptr, "No host window");
        return SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                      nullptr);
    }
    std::pair<int, int> drawable_size() const override {
        RECT client{};
        require(window_ && GetClientRect(static_cast<HWND>(native_window()), &client),
                "Cannot query the window client area");
        return {client.right, client.bottom};
    }
    void *render_window() const override {
        return render_window_;
    }
    Json describe_window() const override {
        RECT outer{}, client{}, render{};
        POINT origin{};
        auto parent = static_cast<HWND>(native_window());
        require(GetWindowRect(parent, &outer) && GetClientRect(parent, &client) &&
                    ClientToScreen(parent, &origin) && GetWindowRect(render_window_, &render),
                "Cannot describe the client render area");
        return {{"client_size", {client.right, client.bottom}},
                {"window_size", {outer.right - outer.left, outer.bottom - outer.top}},
                {"client_origin_in_window", {origin.x - outer.left, origin.y - outer.top}},
                {"render_origin_in_client", {render.left - origin.x, render.top - origin.y}},
                {"render_size", {render.right - render.left, render.bottom - render.top}},
                {"fullscreen", fullscreen_}};
    }
    void window_mode(WindowMode mode) override {
        if (mode == WindowMode::Fullscreen) {
            if (!fullscreen_)
                fullscreen();
        } else {
            if (fullscreen_)
                fullscreen();
            require(mode == WindowMode::Maximized ? SDL_MaximizeWindow(window_)
                                                  : SDL_RestoreWindow(window_),
                    SDL_GetError());
            require(SDL_SyncWindow(window_), SDL_GetError());
        }
        sync_render_window();
    }
    std::vector<Event> poll() override {
        std::vector<WebViewRequest> requests;
        {
            std::lock_guard lock(web_mutex_);
            requests.swap(web_requests_);
        }
        for (auto &request : requests)
            web_views_.push_back(open_webview(native_window(), std::move(request)));
        std::erase_if(web_views_, [](const auto &view) { return view->closed(); });
        std::vector<Event> out;
        SDL_Event e{};
        // Pump native resize/DPI changes before deriving the mouse-to-pixel scale.
        SDL_PumpEvents();
        sync_render_window();
        int logical_w = 1, logical_h = 1, pixel_w = 1, pixel_h = 1;
        if (window_) {
            SDL_GetWindowSize(window_, &logical_w, &logical_h);
            SDL_GetWindowSizeInPixels(window_, &pixel_w, &pixel_h);
        }
        auto sx = logical_w ? float(pixel_w) / logical_w : 1.f,
             sy = logical_h ? float(pixel_h) / logical_h : 1.f;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                out.push_back({EventType::Quit});
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                out.push_back({EventType::Resize, 0, 0, e.window.data1, e.window.data2});
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                out.push_back({EventType::Focus, 0, 0, 0, 0, true});
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                out.push_back({EventType::Focus, 0, 0, 0, 0, false});
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT)
                    out.push_back({e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EventType::PointerDown
                                                                         : EventType::PointerUp,
                                   e.button.x * sx, e.button.y * sy});
                break;
            case SDL_EVENT_MOUSE_MOTION:
                out.push_back({EventType::PointerMove, e.motion.x * sx, e.motion.y * sy});
                break;
            case SDL_EVENT_TEXT_INPUT:
                out.push_back({EventType::Text, 0, 0, 0, 0, false, e.text.text});
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool released = e.type == SDL_EVENT_KEY_UP;
                if (e.key.key == SDLK_ESCAPE && !e.key.repeat && !released)
                    out.push_back({EventType::Back});
                if (e.key.key == SDLK_BACKSPACE && !released) {
                    if (text_input_enabled_)
                        out.push_back({EventType::Text, 0, 0, 0, 0, false, "\b"});
                    else if (!e.key.repeat)
                        out.push_back({EventType::Back});
                }
                if (e.key.key == SDLK_F11 && !e.key.repeat && !released)
                    fullscreen();
                int code = e.key.key == SDLK_RETURN  ? 66
                           : e.key.key == SDLK_LEFT  ? 21
                           : e.key.key == SDLK_RIGHT ? 22
                           : e.key.key == SDLK_UP    ? 19
                           : e.key.key == SDLK_DOWN  ? 20
                           : e.key.key == SDLK_TAB   ? 61
                                                     : 0;
                if (code)
                    out.push_back({EventType::Key, 0, 0, 0, 0, false, {}, code, released});
                break;
            }
            default:
                break;
            }
        }
        return out;
    }
    void fullscreen() override {
        fullscreen_ = !fullscreen_;
        require(SDL_SetWindowFullscreen(window_, fullscreen_), SDL_GetError());
        require(SDL_SyncWindow(window_), SDL_GetError());
        sync_render_window();
    }
    void start_text(bool enabled) override {
        text_input_enabled_ = enabled;
        if (window_) {
            if (enabled)
                SDL_StartTextInput(window_);
            else
                SDL_StopTextInput(window_);
        }
    }
    std::shared_ptr<AudioStream> create_audio(int frequency, int channels) override {
        if (!initialized_) {
            require(SDL_Init(SDL_INIT_AUDIO), SDL_GetError());
            initialized_ = true;
        }
        auto stream = std::make_shared<WindowsAudio>(frequency, channels, gain_);
        std::erase_if(streams_, [](const auto &value) { return value.expired(); });
        streams_.push_back(stream);
        return stream;
    }
    void audio_gain(float gain) override {
        require(gain >= 0.f && gain <= 1.f, "Invalid application audio gain");
        gain_ = gain;
        for (auto &value : streams_)
            if (auto stream = value.lock())
                stream->application_gain(gain);
    }
    void show_webview(WebViewRequest request) override {
        std::lock_guard lock(web_mutex_);
        web_requests_.push_back(std::move(request));
    }
    void close_webviews() override {
        std::lock_guard lock(web_mutex_);
        web_requests_.clear();
        for (auto &view : web_views_)
            view->close();
        web_views_.clear();
    }

  private:
    void sync_render_window() {
        if (!render_window_)
            return;
        auto [width, height] = drawable_size();
        RECT current{};
        GetClientRect(render_window_, &current);
        if (width > 0 && height > 0 && (current.right != width || current.bottom != height))
            require(SetWindowPos(render_window_, nullptr, 0, 0, width, height,
                                 SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                    "Cannot resize the client render surface");
    }
    SDL_Window *window_{};
    HWND render_window_{};
    std::vector<std::weak_ptr<WindowsAudio>> streams_;
    bool initialized_{}, fullscreen_{}, text_input_enabled_{};
    float gain_ = 1.f;
    std::mutex web_mutex_;
    std::vector<WebViewRequest> web_requests_;
    std::vector<std::shared_ptr<WebView>> web_views_;
};
} // namespace
std::unique_ptr<Services> windows() {
    return std::make_unique<Windows>();
}
bool open_url(std::string_view url) {
    if (!url.starts_with("https://") || url.find_first_of("\r\n") != url.npos)
        return false;
    auto target = widen(url);
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
fs::path default_data_root() {
    PWSTR folder{};
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)),
            "Cannot locate LocalAppData");
    fs::path path(folder);
    CoTaskMemFree(folder);
    return path / L"AndroidPcRuntime";
}
fs::path executable_directory() {
    std::wstring buffer(32768, 0);
    auto n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(n && n < buffer.size(), "Cannot locate executable");
    buffer.resize(n);
    return fs::path(buffer).parent_path();
}
Library::Library(const fs::path &path) {
    handle_ = LoadLibraryExW(fs::absolute(path).c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(handle_ != nullptr, "Cannot load native library: " + utf8(path));
}
Library::~Library() {
    if (handle_)
        FreeLibrary(static_cast<HMODULE>(handle_));
}
void *Library::symbol(std::string_view name) const {
    return reinterpret_cast<void *>(
        GetProcAddress(static_cast<HMODULE>(handle_), std::string(name).c_str()));
}
std::optional<fs::path> development_angle_directory() {
    std::vector<fs::path> candidates{executable_directory() / "angle", executable_directory()};
    wchar_t buffer[32768];
    for (const auto *name : {L"ANDROID_SDK_ROOT", L"ANDROID_HOME"})
        if (auto count = GetEnvironmentVariableW(name, buffer, 32768); count && count < 32768)
            candidates.emplace_back(fs::path(buffer) / L"emulator/lib64/gles_angle");
    if (auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768); count && count < 32768)
        candidates.emplace_back(fs::path(buffer) / L"Android/Sdk/emulator/lib64/gles_angle");
    for (const auto &path : candidates)
        if (fs::is_regular_file(path / "libEGL.dll") && fs::is_regular_file(path / "libGLESv2.dll"))
            return path;
    return {};
}
} // namespace acp::host

#pragma once
#include <acp/common.hpp>
namespace acp::host {
enum class WindowMode { Windowed, Maximized, Fullscreen };
enum class EventType {
    Quit,
    Resize,
    Focus,
    PointerDown,
    PointerUp,
    PointerMove,
    Back,
    Text,
    Fullscreen,
    Key
};
struct Event {
    EventType type;
    float x{}, y{};
    int width{}, height{};
    bool focused{};
    std::string text;
    int key_code{};
    bool released{};
};
// Frame numbers start at zero. Pointer coordinates in the document are normalized.
class InputReplay {
  public:
    explicit InputReplay(const Json &document);
    std::vector<Event> take(std::uint64_t frame, int width, int height);
    size_t delivered() const {
        return next_;
    }
    size_t remaining() const {
        return events_.size() - next_;
    }

  private:
    std::vector<std::pair<std::uint64_t, Event>> events_;
    size_t next_{};
};
struct Viewport {
    int x{}, y{}, width{}, height{};
    float guest_x(float x) const;
    float guest_y(float y) const;
    bool contains(float px, float py) const;
};
Viewport fit_viewport(int drawable_width, int drawable_height, int guest_width, int guest_height);
struct WebViewRequest {
    std::string title, html, page_script, callback_scheme;
    fs::path data_directory, preview;
    std::function<bool(const Json &)> message;
    std::function<void(std::string)> failure;
};
class WebView {
  public:
    virtual ~WebView() = default;
    virtual bool closed() const = 0;
    virtual void close() = 0;
};
std::shared_ptr<WebView> open_webview(void *owner, WebViewRequest request);
class AudioStream {
  public:
    virtual ~AudioStream() = default;
    virtual void submit(std::span<const std::byte> pcm) = 0;
    virtual size_t queued_bytes() const = 0;
    virtual void pause(bool paused) = 0;
    virtual void clear() = 0;
    virtual void gain(float gain) = 0;
};
class Services {
  public:
    virtual ~Services() = default;
    virtual void open_window(std::string_view title, int width, int height) = 0;
    virtual void *native_window() const = 0;
    virtual void *render_window() const {
        return native_window();
    }
    virtual void window_mode(WindowMode mode) {
        if (mode == WindowMode::Fullscreen)
            fullscreen();
    }
    virtual Json describe_window() const {
        return Json::object();
    }
    virtual std::pair<int, int> drawable_size() const {
        return {1280, 720};
    }
    virtual std::vector<Event> poll() = 0;
    virtual void fullscreen() = 0;
    virtual void start_text(bool enabled) = 0;
    virtual std::shared_ptr<AudioStream> create_audio(int frequency, int channels) = 0;
    virtual void audio_gain(float gain) = 0;
    virtual void show_webview(WebViewRequest) {
        throw Error("This host has no web view service");
    }
    virtual void close_webviews() {}
};
bool open_url(std::string_view url);
std::unique_ptr<Services> windows();
fs::path default_data_root();
fs::path executable_directory();
struct CalendarTime {
    std::array<std::int32_t, 9> fields{};
    std::int64_t utc_offset{};
    std::string zone;
};
std::expected<CalendarTime, int> calendar_time(std::int64_t unix_seconds, bool utc);
FILE *null_stream();
Bytes protect_data(std::span<const std::byte> plaintext);
Bytes unprotect_data(std::span<const std::byte> ciphertext);
class Library {
  public:
    explicit Library(const fs::path &path);
    ~Library();
    Library(const Library &) = delete;
    void *symbol(std::string_view name) const;

  private:
    void *handle_{};
};
std::optional<fs::path> development_angle_directory();
} // namespace acp::host

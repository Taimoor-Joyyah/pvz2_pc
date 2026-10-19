#pragma once
#include <acp/guest.hpp>
#include <acp/host.hpp>
#include <acp/package.hpp>
#include <acp/performance.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>

namespace acp::android {
class Runtime;
struct Object {
    std::string class_name;
    std::u16string text;
    Bytes bytes;
    std::vector<Address> elements;
    Json fields = Json::object();
};
struct JavaCall {
    Runtime &runtime;
    guest::Cpu &cpu;
    Address receiver{};
    std::string class_name, name, signature;
    std::vector<std::uint64_t> arguments;
};
using JavaHandler = std::function<std::uint64_t(JavaCall &)>;
class Jni {
  public:
    // Keep one cache per fixed native signature. Registration and image changes invalidate it.
    struct NativeBinding {
        Address address{};
        std::uint64_t registration_revision{}, linker_revision{};
    };
    explicit Jni(Runtime &runtime);
    ~Jni();
    Address environment() const {
        return environment_;
    }
    Address environment(guest::Cpu &cpu);
    Address vm() const {
        return vm_;
    }
    Address object(std::string class_name);
    Address local_ref(Address reference);
    Address global_ref(Address reference);
    void delete_ref(Address reference);
    Address string(std::string_view value);
    std::string text(Address reference) const;
    Object &get(Address reference);
    const Object &get(Address reference) const;
    Address class_ref(std::string name);
    void set_superclass(std::string child, std::string parent);
    void set_interfaces(std::string type, std::vector<std::string> interfaces);
    Json serialize(Address value) const;
    Address deserialize(const Json &value);
    std::uint64_t with_local_frame(guest::Cpu &cpu, const std::function<std::uint64_t()> &work);
    std::uint64_t call_native(guest::Cpu &cpu, Address function, std::span<const Address> arguments,
                              std::uint64_t budget = 200'000'000,
                              std::chrono::milliseconds timeout = std::chrono::seconds(30));
    void hook(std::string class_name, std::string name, std::string signature, JavaHandler handler);
    void hook_suppressed(std::string class_name, std::string name, std::string signature,
                         std::string category, std::string behavior, JavaHandler handler);
    void field(std::string class_name, std::string name, std::string signature, std::uint64_t value);
    Address native(std::string_view class_name, std::string_view name, std::string_view signature) const;
    Address native_cached(NativeBinding &binding, std::string_view class_name, std::string_view name,
                          std::string_view signature) const;
    void register_native(std::string class_name, std::string name, std::string signature, Address target);
    Json describe() const;

  private:
    struct Method {
        std::string class_name, name, signature;
        bool is_static{};
    };
    Runtime &runtime_;
    Address environment_{}, vm_{};
    std::unordered_map<Address, std::shared_ptr<Object>> objects_;
    std::unordered_map<std::string, Address> classes_;
    std::unordered_map<Address, Method> methods_;
    std::unordered_map<std::string, JavaHandler> hooks_;
    std::unordered_map<std::string, std::uint64_t> fields_;
    std::unordered_map<std::string, Address> natives_;
    std::uint64_t native_revision_ = 1;
    std::map<std::string, std::uint64_t> calls_;
    std::map<Address, std::vector<std::vector<Address>>> frames_;
    std::map<Address, Address> exceptions_;
    std::map<std::string, std::string> parents_;
    std::map<std::string, std::vector<std::string>> interfaces_;
    std::unordered_map<Address, std::pair<Address, bool>> pinned_;
    bool dex_constants_loaded_{};
    bool destroying_{};
    std::shared_ptr<Object> make_object();
    void load_dex_constants();
    std::map<Address, Address> environments_;
    Address reference(std::shared_ptr<Object> object, bool local = true);
    Address current_thread() const;
    void push_frame();
    Address pop_frame(Address preserve = 0);
    std::string parent(std::string_view child) const;
    bool assignable(std::string child, std::string_view ancestor) const;
    Address method(std::string class_name, std::string name, std::string signature, bool is_static);
    std::uint64_t invoke(guest::Cpu &cpu, bool is_static, int variant, size_t result_kind,
                         bool nonvirtual = false);
    std::uint64_t field_access(guest::Cpu &cpu, bool is_static, bool write, size_t result_kind);
};
class Graphics {
  public:
    explicit Graphics(Runtime &runtime);
    ~Graphics();
    void open(void *window, const std::optional<fs::path> &angle_directory);
    void present();
    void resize(int width, int height);
    // Keep an Android surface stable while the desktop drawable changes size.
    void set_guest_size(int width, int height);
    void vsync(bool enabled);
    void finish_frame();
    Bytes read_rgba(int width, int height);
    Json describe() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Audio {
  public:
    explicit Audio(Runtime &runtime);
    ~Audio();
    void pump(guest::Cpu &cpu);
    Json describe() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct RuntimeOptions {
    std::string package, version_name, locale = "en_US";
    std::uint64_t version_code{};
    fs::path data, obb, apks;
    Json application_metadata = Json::object();
    unsigned target_sdk = 1;
    std::uint32_t application_flags = 4;
    bool automatic_audio_callbacks = true;
    bool performance_profile = false;
};
class Runtime {
  public:
    struct GuestThread;
    explicit Runtime(RuntimeOptions options, std::unique_ptr<host::Services> host = host::windows());
    ~Runtime();
    Runtime(const Runtime &) = delete;
    guest::Memory memory;
    guest::TrapTable traps;
    guest::Linker linker;
    guest::Cpu cpu;
    RuntimeOptions options;
    FrameTimings timings;
    std::unique_ptr<host::Services> host;
    std::unique_ptr<Jni> jni;
    std::unique_ptr<Graphics> graphics;
    std::unique_ptr<Audio> audio;
    std::string phase = "created";
    std::set<std::string> unsupported;
    std::vector<std::string> log;
    void bind(std::string name, std::function<std::uint64_t(guest::Cpu &)> handler);
    void bind_void(std::string name, guest::Handler handler);
    void bind_suppressed(std::string name, std::string category, std::string behavior,
                         std::function<std::uint64_t(guest::Cpu &)> handler);
    void record_suppression(std::string name, std::string category, std::string behavior);
    void suppressed_call(std::string_view name);
    void unavailable_imports();
    void initialize_libraries();
    std::vector<std::string> java_libraries;
    std::set<std::string> java_initialized;
    std::multimap<std::string, Address> broadcast_receivers;
    fs::path path(std::string_view guest_path, bool writing = false) const;
    std::expected<fs::path, int> resolve_path(std::string_view guest_path, bool writing = false) const;
    struct AssetFile {
        std::string android_path;
        size_t size{};
    };
    std::optional<AssetFile> asset(std::string_view name);
    Address errno_address(guest::Cpu &cpu) const;
    std::uint64_t failure(guest::Cpu &cpu, int error) const;
    void message(std::string text);
    void file_failure(std::string_view path, int error);
    void post_callback(std::function<void(guest::Cpu &)> callback, std::chrono::milliseconds delay = {});
    void check_threads() const;
    void fail_thread(GuestThread &worker, std::string error);
    void wait_callbacks(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    Json report() const;
    int open_file(const fs::path &path, std::string_view mode);
    int adopt_file(FILE *file, const fs::path &path, int flags);
    int open_directory(const fs::path &path);
    std::optional<fs::path> descriptor_path(int descriptor) const;
    std::expected<std::string, int> absolute_guest_path(std::string_view path, int directory = -100) const;
    std::string working_directory() const;
    void working_directory(std::string value);
    FILE *file(int descriptor) const;
    int close_file(int descriptor);
    int open_device(std::string name);
    std::string device(int descriptor) const;
    Address file_handle(int descriptor);
    int descriptor(Address handle) const;
    size_t read_stream(int descriptor, std::span<std::byte> bytes);
    void unread_stream(int descriptor, std::span<const std::byte> bytes);
    std::int64_t stream_position(int descriptor) const;
    int seek_stream(int descriptor, std::int64_t offset, int origin);
    bool stream_eof(int descriptor) const;
    mutable std::recursive_mutex state_mutex;
    std::map<std::uint64_t, std::uint64_t> syscall_counts;
    std::map<std::string, std::map<int, std::uint64_t>> file_failures;
    struct GuestThread {
        std::unique_ptr<guest::Cpu> cpu;
        std::thread host;
        Address result{};
        std::string error;
        bool detached{};
        std::atomic_bool completed{};
    };
    std::map<Address, std::unique_ptr<GuestThread>> threads;
    std::atomic_size_t next_thread_id{1};
    std::atomic_bool stopping{};
    std::map<Address, std::shared_ptr<std::recursive_timed_mutex>> mutexes;
    std::map<Address, std::shared_ptr<std::condition_variable_any>> conditions;
    std::map<unsigned, std::map<Address, Address>> thread_keys;
    std::map<unsigned, Address> key_destructors;
    std::map<Address, Address> emutls;
    std::map<Address, Address> file_handles;
    std::map<int, int> descriptor_flags;
    std::function<void(int)> closing_file;

  private:
    std::atomic_bool thread_failure_{};
    std::string first_thread_failure_;
    std::condition_variable_any callback_ready_;
    std::deque<std::function<void(guest::Cpu &)>> callbacks_;
    std::multimap<std::chrono::steady_clock::time_point, std::function<void(guest::Cpu &)>>
        scheduled_callbacks_;
    GuestThread *callback_worker_{};
    bool callback_active_{};
    std::map<int, FILE *> files_;
    std::map<int, fs::path> file_paths_, directory_paths_;
    std::map<int, std::deque<std::byte>> unread_bytes_;
    std::set<Address> owned_file_handles_;
    std::string working_directory_;
    struct Suppression { std::string category, behavior; std::uint64_t calls{}; };
    std::map<std::string, Suppression> suppressed_;
    std::vector<std::unique_ptr<Archive>> asset_packages_;
    std::map<std::string, AssetFile> assets_;
    std::map<int, std::string> devices_;
    int next_file_ = 3;
};
void install_bionic(Runtime &runtime);
void install_bionic_offline(Runtime &runtime);
void install_bionic_scan(Runtime &runtime);
void install_java_platform(Runtime &runtime);
void install_java_values(Runtime &runtime);
std::string format_guest(guest::Cpu &cpu, Address format, size_t first_argument, Address va_list = 0);
std::string format_binary128(std::array<std::uint64_t, 2> bits, std::string_view spec, char conversion);
class GameAdapter {
  public:
    virtual ~GameAdapter() = default;
    virtual bool quit_requested() const {
        return false;
    }
    virtual bool accepts(const Manifest &) const = 0;
    virtual std::string_view main_library() const = 0;
    virtual void configure(Runtime &runtime, const Installation &installation) = 0;
    virtual void start(Runtime &runtime) = 0;
    virtual void frame(Runtime &runtime) = 0;
    virtual void event(Runtime &runtime, const host::Event &event) = 0;
    virtual void stop(Runtime &runtime) = 0;
};
} // namespace acp::android

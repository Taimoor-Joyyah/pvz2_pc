#include <acp/android.hpp>

#include <iostream>
namespace acp::android {
Runtime::Runtime(RuntimeOptions opts, std::unique_ptr<host::Services> services)
    : traps(memory), linker(memory, traps), cpu(memory, traps), options(std::move(opts)),
      host(std::move(services)) {
    require(!options.package.empty(), "A package identity is required");
    options.data = fs::absolute(options.data).lexically_normal();
    if (!options.obb.empty())
        options.obb = fs::absolute(options.obb).lexically_normal();
    for (const auto *folder : {"files", "cache", "preferences", "external"})
        fs::create_directories(options.data / folder);
    fs::create_directories(options.data / "external/Android/data" / path_from_utf8(options.package) /
                           "files");
    for (const auto *name : {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libandroid.so",
                             "libGLESv2.so", "libGLESv1_CM.so", "libEGL.so", "libOpenSLES.so"})
        linker.system_library(name);
    install_bionic(*this);
    jni = std::make_unique<Jni>(*this);
    graphics = std::make_unique<Graphics>(*this);
    audio = std::make_unique<Audio>(*this);
    cpu.checkpoint = [this] {
        if (thread_failure_.load(std::memory_order_relaxed))
            check_threads();
    };
    install_java_values(*this);
    install_java_platform(*this);
}
Runtime::~Runtime() {
    stopping = true;
    host->close_webviews();
    callback_ready_.notify_all();
    std::vector<GuestThread *> workers;
    std::vector<std::shared_ptr<std::condition_variable_any>> wakeups;
    {
        std::lock_guard lock(state_mutex);
        for (auto &[_, thread] : threads)
            workers.push_back(thread.get());
        for (auto &[_, condition] : conditions)
            wakeups.push_back(condition);
    }
    for (auto *thread : workers)
        thread->cpu->request_stop();
    for (auto &condition : wakeups)
        condition->notify_all();
    for (auto *thread : workers)
        if (thread->host.joinable())
            thread->host.join();
    for (auto &[id, file] : files_)
        if (id >= 3 && file)
            std::fclose(file);
}
void Runtime::bind(std::string name, std::function<std::uint64_t(guest::Cpu &)> handler) {
    linker.provide(std::move(name),
                   [handler = std::move(handler)](guest::Cpu &cpu) { cpu.x(0, handler(cpu)); });
}
void Runtime::bind_void(std::string name, guest::Handler handler) {
    linker.provide(std::move(name), std::move(handler));
}
void Runtime::unavailable_imports() {
    for (const auto &name : linker.unresolved()) {
        unsupported.insert(name);
        bind(name, [this, name](guest::Cpu &cpu) -> std::uint64_t {
            throw Error(std::format("Unimplemented Android import '{}' called from {}", name,
                                    linker.locate(cpu.x(30))));
        });
    }
}
void Runtime::initialize_libraries() {
    for (const auto &name : java_libraries) {
        guest::LoadedImage *image = nullptr;
        for (const auto &item : linker.images())
            if (utf8(item->path.filename()) == name)
                image = item.get();
        require(image != nullptr, "Java native library was not loaded: " + name);
        if (java_initialized.contains(name))
            continue;
        phase = "constructors:" + name;
        linker.initialize(cpu, image);
        if (auto onload = linker.image_symbol(*image, "JNI_OnLoad")) {
            phase = "JNI_OnLoad:" + name;
            message(phase);
            auto version = jni->call_native(cpu, onload, std::array{jni->vm(), Address{0}});
            require(version == 0x10004 || version == 0x10006 || version == 0x10008,
                    "Unsupported JNI_OnLoad result from " + name);
        }
        java_initialized.insert(name);
    }
    phase = "native-libraries-initialized";
}
fs::path Runtime::path(std::string_view input, bool writing) const {
    require(!input.empty() && input.find('\0') == input.npos, "Empty or invalid guest path");
    std::string value(input);
    std::replace(value.begin(), value.end(), '\\', '/');
    require(value.find(':') == value.npos, "Host paths are inaccessible to guest code");
    fs::path root;
    std::string suffix;
    struct Mount {
        std::string prefix;
        fs::path directory;
        bool writable;
    };
    const auto external = options.data / "external";
    const auto external_app = external / "Android/data" / path_from_utf8(options.package);
    const std::vector<Mount> mappings{
        {"/data/data/" + options.package, options.data, true},
        {"/data/user/0/" + options.package, options.data, true},
        {"/data/app/" + options.package, options.apks.parent_path(), false},
        {"/sdcard/Android/data/" + options.package, external_app, true},
        {"/storage/emulated/0/Android/data/" + options.package, external_app, true},
        {"/sdcard/Android/obb/" + options.package, options.obb.parent_path(), false},
        {"/storage/emulated/0/Android/obb/" + options.package, options.obb.parent_path(), false},
        {"/sdcard", external, true},
        {"/storage/emulated/0", external, true}};
    bool matched = false;
    for (const auto &[prefix, directory, writable] : mappings)
        if (value == prefix || value.starts_with(prefix + "/")) {
            require(!directory.empty(), "Requested content is not mounted");
            root = directory;
            suffix = value.substr(prefix.size());
            if (!suffix.empty())
                suffix.erase(0, 1);
            if (writing)
                require(writable, "Expansion content is read-only");
            matched = true;
            break;
        }
    if (!matched) {
        require(value.front() != '/', "Unmapped Android path: " + value);
        root = options.data / "files";
        suffix = value;
    }
    require(suffix.empty() || is_safe_relative(suffix), "Guest path traversal rejected");
    auto target = fs::weakly_canonical(root / path_from_utf8(suffix));
    auto canonical_root = fs::weakly_canonical(root);
    auto rel = target.lexically_relative(canonical_root);
    require(rel.empty() || *rel.begin() != "..", "Guest path escaped its mounted storage");
    return target;
}
Address Runtime::errno_address(guest::Cpu &cpu) const {
    return cpu.tls() + 16;
}
std::expected<fs::path, int> Runtime::resolve_path(std::string_view value, bool writing) const {
    if (value.empty())
        return std::unexpected(2);
    try {
        return path(value, writing);
    } catch (const Error &) {
        return std::unexpected(13);
    } catch (const fs::filesystem_error &) {
        return std::unexpected(2);
    }
}
std::optional<Runtime::AssetFile> Runtime::asset(std::string_view name) {
    std::lock_guard lock(state_mutex);
    require(is_safe_relative(name), "Invalid APK asset path");
    auto cached = assets_.find(std::string(name));
    if (cached != assets_.end())
        return cached->second;
    if (options.apks.empty() || !fs::is_directory(options.apks))
        return {};
    if (asset_packages_.empty()) {
        std::vector<fs::path> files;
        for (const auto &item : fs::directory_iterator(options.apks))
            if (item.is_regular_file() && item.path().extension() == ".apk")
                files.push_back(item.path());
        std::ranges::sort(files);
        for (const auto &file : files)
            asset_packages_.push_back(std::make_unique<Archive>(file));
    }
    for (const auto &package : asset_packages_)
        if (package->contains("assets/" + std::string(name))) {
            auto bytes = package->read("assets/" + std::string(name));
            auto relative =
                "cache/apk-assets/" + utf8(options.apks.parent_path().filename()) + "/" + std::string(name);
            write_bytes(options.data / path_from_utf8(relative), bytes);
            AssetFile file{"/data/data/" + options.package + "/" + relative, bytes.size()};
            assets_.emplace(name, file);
            return file;
        }
    return {};
}
std::uint64_t Runtime::failure(guest::Cpu &cpu, int error) const {
    cpu.memory().store(errno_address(cpu), static_cast<std::int32_t>(error));
    return UINT64_MAX;
}
void Runtime::message(std::string text) {
    std::lock_guard lock(state_mutex);
    if (log.size() >= 1000)
        log.erase(log.begin());
    log.push_back(text);
    std::cerr << text << '\n';
}
void Runtime::file_failure(std::string_view path, int error) {
    std::lock_guard lock(state_mutex);
    if (file_failures.size() < 256 || file_failures.contains(std::string(path)))
        ++file_failures[std::string(path)][error];
}
void Runtime::post_callback(std::function<void(guest::Cpu &)> callback, std::chrono::milliseconds delay) {
    std::lock_guard lock(state_mutex);
    require(!stopping, "Posting a callback during shutdown");
    auto now = std::chrono::steady_clock::now();
    require(delay.count() >= 0 && delay <= std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::time_point::max() - now),
            "Invalid callback delay");
    if (delay.count())
        scheduled_callbacks_.emplace(now + delay, std::move(callback));
    else
        callbacks_.push_back(std::move(callback));
    if (!callback_worker_) {
        auto id = next_thread_id++;
        require(id < 256, "Maximum guest thread count exceeded");
        auto worker = std::make_unique<GuestThread>();
        worker->cpu = std::make_unique<guest::Cpu>(memory, traps, id);
        worker->cpu->syscall = cpu.syscall;
        worker->cpu->checkpoint = cpu.checkpoint;
        auto *ptr = worker.get();
        threads.emplace(ptr->cpu->tls(), std::move(worker));
        callback_worker_ = ptr;
        ptr->host = std::thread([this, ptr] {
            jni->environment(*ptr->cpu);
            for (;;) {
                std::function<void(guest::Cpu &)> work;
                {
                    std::unique_lock lock(state_mutex);
                    for (;;) {
                        if (stopping)
                            return;
                        auto now = std::chrono::steady_clock::now();
                        while (!scheduled_callbacks_.empty() &&
                               scheduled_callbacks_.begin()->first <= now) {
                            auto task = scheduled_callbacks_.extract(scheduled_callbacks_.begin());
                            callbacks_.push_back(std::move(task.mapped()));
                        }
                        if (!callbacks_.empty())
                            break;
                        if (scheduled_callbacks_.empty())
                            callback_ready_.wait(lock);
                        else
                            callback_ready_.wait_until(lock, scheduled_callbacks_.begin()->first);
                    }
                    work = std::move(callbacks_.front());
                    callbacks_.pop_front();
                    callback_active_ = true;
                }
                try {
                    jni->with_local_frame(*ptr->cpu, [&] {
                        work(*ptr->cpu);
                        return std::uint64_t{};
                    });
                } catch (const std::exception &error) {
                    if (!stopping)
                        fail_thread(*ptr, error.what());
                } catch (...) {
                    if (!stopping)
                        fail_thread(*ptr, "Java callback terminated unexpectedly");
                }
                {
                    std::lock_guard lock(state_mutex);
                    callback_active_ = false;
                    callback_ready_.notify_all();
                    if (!ptr->error.empty()) {
                        message("Java callback worker failed: " + ptr->error);
                        return;
                    }
                }
            }
        });
    }
    callback_ready_.notify_one();
}
void Runtime::fail_thread(GuestThread &worker, std::string error) {
    std::lock_guard lock(state_mutex);
    worker.error = std::move(error);
    if (!thread_failure_.load(std::memory_order_relaxed))
        first_thread_failure_ =
            std::format("Guest thread {:#x} failed: {}", worker.cpu->tls(), worker.error);
    thread_failure_.store(true, std::memory_order_relaxed);
    for (auto &[_, condition] : conditions)
        condition->notify_all();
    callback_ready_.notify_all();
}
void Runtime::check_threads() const {
    std::lock_guard lock(state_mutex);
    require(first_thread_failure_.empty(), first_thread_failure_);
}
void Runtime::wait_callbacks(std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_mutex);
    require(callback_ready_.wait_for(lock, timeout,
                                     [&] {
                                         return (!callback_active_ && callbacks_.empty()) ||
                                                (callback_worker_ && !callback_worker_->error.empty());
                                     }),
            "Timed out waiting for Java callbacks");
    check_threads();
}
Json Runtime::report() const {
    std::lock_guard lock(state_mutex);
    Json images = Json::array();
    for (const auto &image : linker.images())
        images.push_back({{"file", utf8(image->path.filename())},
                          {"base", image->base},
                          {"initialized", image->initialized},
                          {"sha256", sha256(image->elf.bytes())}});
    Json workers = Json::array();
    for (const auto &[thread, worker] : threads)
        if (!worker->error.empty())
            workers.push_back(
                {{"thread", thread}, {"error", worker->error}, {"fault", worker->cpu->last_fault}});
    return {{"schema", 1},
            {"phase", phase},
            {"package", options.package},
            {"version", options.version_name},
            {"images", images},
            {"unsupported_imports", unsupported},
            {"jni", jni->describe()},
            {"graphics", graphics->describe()},
            {"audio", audio->describe()},
            {"log", log},
            {"syscalls", syscall_counts},
            {"file_failures", file_failures},
            {"guest_memory", memory.statistics()},
            {"guest_cpu", cpu.statistics()},
            {"worker_errors", workers},
            {"pending_callbacks", callbacks_.size()},
            {"scheduled_callbacks", scheduled_callbacks_.size()},
            {"cpu_fault", cpu.last_fault},
            {"initializer", linker.current_initializer}};
}
int Runtime::open_file(const fs::path &path, std::string_view mode) {
    std::lock_guard lock(state_mutex);
    auto fp = _wfopen(path.c_str(), widen(mode).c_str());
    if (!fp)
        return -1;
    auto id = next_file_++;
    files_[id] = fp;
    return id;
}
FILE *Runtime::file(int descriptor) const {
    std::lock_guard lock(state_mutex);
    if (descriptor == 0)
        return stdin;
    if (descriptor == 1)
        return stdout;
    if (descriptor == 2)
        return stderr;
    auto it = files_.find(descriptor);
    return it == files_.end() ? nullptr : it->second;
}
int Runtime::close_file(int descriptor) {
    std::lock_guard lock(state_mutex);
    auto device = devices_.erase(descriptor) != 0;
    auto it = files_.find(descriptor);
    if (it == files_.end())
        return device ? 0 : -1;
    auto result = std::fclose(it->second);
    files_.erase(it);
    return result;
}
int Runtime::open_device(std::string name) {
    std::lock_guard lock(state_mutex);
    auto fd = next_file_++;
    if (name == "/dev/null") {
        auto stream = host::null_stream();
        if (!stream)
            return -1;
        files_[fd] = stream;
    }
    devices_[fd] = std::move(name);
    return fd;
}
std::string Runtime::device(int descriptor) const {
    std::lock_guard lock(state_mutex);
    auto it = devices_.find(descriptor);
    return it == devices_.end() ? "" : it->second;
}
Address Runtime::file_handle(int descriptor) {
    std::lock_guard lock(state_mutex);
    auto address = memory.allocate(256);
    memory.store(address, static_cast<std::int32_t>(descriptor));
    file_handles[address] = descriptor;
    return address;
}
int Runtime::descriptor(Address handle) const {
    std::lock_guard lock(state_mutex);
    auto it = file_handles.find(handle);
    return it == file_handles.end() ? -1 : static_cast<int>(it->second);
}
} // namespace acp::android

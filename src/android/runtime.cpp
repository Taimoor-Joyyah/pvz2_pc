#include <acp/android.hpp>

#include <iostream>
namespace acp::android {
Runtime::Runtime(RuntimeOptions opts, std::unique_ptr<host::Services> services)
    : traps(memory), linker(memory, traps), cpu(memory, traps), options(std::move(opts)),
      host(std::move(services)) {
    require(!options.package.empty(), "A package identity is required");
    options.data = fs::absolute(options.data).lexically_normal();
    working_directory_ = "/data/data/" + options.package + "/files";
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
    install_bionic_offline(*this);
    install_bionic_scan(*this);
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
    while (!files_.empty())
        close_file(files_.begin()->first);
}
void Runtime::bind(std::string name, std::function<std::uint64_t(guest::Cpu &)> handler) {
    linker.provide(std::move(name),
                   [handler = std::move(handler)](guest::Cpu &cpu) { cpu.x(0, handler(cpu)); });
}
void Runtime::bind_void(std::string name, guest::Handler handler) {
    linker.provide(std::move(name), std::move(handler));
}
void Runtime::record_suppression(std::string name, std::string category, std::string behavior) {
    std::lock_guard lock(state_mutex);
    suppressed_.try_emplace(std::move(name), Suppression{std::move(category), std::move(behavior)});
}
void Runtime::suppressed_call(std::string_view name) {
    std::lock_guard lock(state_mutex);
    auto it = suppressed_.find(std::string(name));
    if (it != suppressed_.end())
        ++it->second.calls;
}
void Runtime::bind_suppressed(std::string name, std::string category, std::string behavior,
                             std::function<std::uint64_t(guest::Cpu &)> handler) {
    record_suppression(name, std::move(category), std::move(behavior));
    bind(name, [this, name, handler = std::move(handler)](guest::Cpu &cpu) {
        suppressed_call(name);
        return handler(cpu);
    });
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
        require(is_safe_relative(value), "Guest path traversal rejected");
        return path(working_directory() + "/" + value, writing);
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
    Json suppressions = Json::array();
    for (const auto &[name, policy] : suppressed_)
        suppressions.push_back({{"name", name}, {"category", policy.category},
                                {"behavior", policy.behavior}, {"calls", policy.calls}});
    Json imports = Json::object();
    for (const auto &image : linker.images())
        for (const auto &symbol : image->elf.symbols()) {
            if (symbol.defined() || symbol.name.empty() || !symbol.binding) continue;
            auto &entry = imports[symbol.name];
            if (entry.is_null()) {
                auto target = linker.symbol(symbol.name);
                auto bridge = linker.provided().find(symbol.name);
                std::string status = unsupported.contains(symbol.name) ? "unimplemented"
                                     : !target ? "weak-optional"
                                     : suppressed_.contains(symbol.name) ? "suppressed"
                                     : bridge != linker.provided().end() && bridge->second == target ? "host-bridge"
                                                                                                   : "guest-library";
                entry = {{"status", status}, {"required_by", Json::array()}};
            }
            auto file = utf8(image->path.filename());
            auto &required = entry["required_by"];
            if (std::find(required.begin(), required.end(), file) == required.end()) required.push_back(file);
        }
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
            {"native_imports", imports},
            {"suppressed_functions", suppressions},
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
    // Android stdio is byte preserving even when the caller omits "b". Windows
    // text mode would translate CRLF and treat 0x1a inside streamed media as EOF.
    std::string host_mode(mode);
    if (host_mode.find('b') == host_mode.npos)
        host_mode += 'b';
    auto fp = _wfopen(path.c_str(), widen(host_mode).c_str());
    if (!fp)
        return -1;
    int flags = mode.find('+') != mode.npos ? 2 : mode.starts_with('r') ? 0 : 1;
    if (mode.starts_with('a'))
        flags |= 0x400;
    return adopt_file(fp, path, flags);
}
int Runtime::adopt_file(FILE *fp, const fs::path &path, int flags) {
    std::lock_guard lock(state_mutex);
    auto id = next_file_++;
    files_[id] = fp;
    file_paths_[id] = path;
    descriptor_flags[id] = flags;
    return id;
}
int Runtime::open_directory(const fs::path &path) {
    std::lock_guard lock(state_mutex);
    auto id = next_file_++;
    directory_paths_[id] = path;
    descriptor_flags[id] = 0x10000;
    return id;
}
std::optional<fs::path> Runtime::descriptor_path(int descriptor) const {
    std::lock_guard lock(state_mutex);
    if (auto it = file_paths_.find(descriptor); it != file_paths_.end())
        return it->second;
    if (auto it = directory_paths_.find(descriptor); it != directory_paths_.end())
        return it->second;
    return {};
}
std::string Runtime::working_directory() const {
    std::lock_guard lock(state_mutex);
    return working_directory_;
}
void Runtime::working_directory(std::string value) {
    std::lock_guard lock(state_mutex);
    working_directory_ = std::move(value);
}
std::expected<std::string, int> Runtime::absolute_guest_path(std::string_view value, int directory) const {
    if (value.empty())
        return std::unexpected(2);
    if (value.front() == '/')
        return std::string(value);
    if (!is_safe_relative(value))
        return std::unexpected(13);
    if (directory == -100)
        return working_directory() + "/" + std::string(value);
    std::lock_guard lock(state_mutex);
    auto it = directory_paths_.find(directory);
    if (it == directory_paths_.end())
        return std::unexpected(file_paths_.contains(directory) ? 20 : 9);
    // Convert the host directory back through an existing virtual mount. Never expose a host path.
    for (auto prefix : {"/data/data/" + options.package, std::string("/sdcard"),
                        "/sdcard/Android/obb/" + options.package}) {
        auto mount = resolve_path(prefix);
        if (!mount)
            continue;
        auto relative = it->second.lexically_relative(*mount);
        if (!relative.empty() && *relative.begin() != "..")
            return prefix + "/" + utf8(relative) + "/" + std::string(value);
        if (it->second == *mount)
            return prefix + "/" + std::string(value);
    }
    return std::unexpected(13);
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
    if (closing_file)
        closing_file(descriptor);
    std::erase_if(file_handles, [this, descriptor](const auto &item) {
        if (item.second != descriptor) return false;
        if (owned_file_handles_.erase(item.first)) memory.release(item.first);
        return true;
    });
    file_paths_.erase(descriptor);
    unread_bytes_.erase(descriptor);
    descriptor_flags.erase(descriptor);
    if (directory_paths_.erase(descriptor))
        return 0;
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
    for (const auto &[address, fd] : file_handles)
        if (static_cast<int>(fd) == descriptor) return address;
    auto address = memory.allocate(256);
    memory.store(address, static_cast<std::int32_t>(descriptor));
    file_handles[address] = descriptor;
    owned_file_handles_.insert(address);
    return address;
}
int Runtime::descriptor(Address handle) const {
    std::lock_guard lock(state_mutex);
    auto it = file_handles.find(handle);
    return it == file_handles.end() ? -1 : static_cast<int>(it->second);
}
size_t Runtime::read_stream(int descriptor, std::span<std::byte> bytes) {
    std::lock_guard lock(state_mutex);
    auto fp = file(descriptor);
    if (!fp) return 0;
    auto &pending = unread_bytes_[descriptor];
    size_t count = 0;
    while (count < bytes.size() && !pending.empty()) {
        bytes[count++] = pending.front();
        pending.pop_front();
    }
    if (count < bytes.size()) count += std::fread(bytes.data() + count, 1, bytes.size() - count, fp);
    return count;
}
void Runtime::unread_stream(int descriptor, std::span<const std::byte> bytes) {
    std::lock_guard lock(state_mutex);
    auto &pending = unread_bytes_[descriptor];
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) pending.push_front(*it);
    if (auto fp = file(descriptor)) std::clearerr(fp);
}
std::int64_t Runtime::stream_position(int descriptor) const {
    std::lock_guard lock(state_mutex);
    auto fp = file(descriptor);
    if (!fp) return -1;
    auto position = _ftelli64(fp);
    auto it = unread_bytes_.find(descriptor);
    return position < 0 ? position : position - (it == unread_bytes_.end() ? 0 : it->second.size());
}
int Runtime::seek_stream(int descriptor, std::int64_t offset, int origin) {
    std::lock_guard lock(state_mutex);
    auto fp = file(descriptor);
    if (!fp) return -1;
    if (origin == SEEK_CUR) offset -= static_cast<std::int64_t>(unread_bytes_[descriptor].size());
    auto result = _fseeki64(fp, offset, origin);
    if (!result) unread_bytes_.erase(descriptor);
    return result;
}
bool Runtime::stream_eof(int descriptor) const {
    std::lock_guard lock(state_mutex);
    auto pending = unread_bytes_.find(descriptor);
    if (pending != unread_bytes_.end() && !pending->second.empty()) return false;
    auto fp = file(descriptor);
    return fp && std::feof(fp);
}
} // namespace acp::android

#include <acp/android.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <cerrno>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

namespace acp::android {
namespace {
int host_error(int error) {
    // Most CRT errno numbers agree with bionic; these do not.
    if (error == EACCES) return 13;
    if (error == ENOENT) return 2;
    if (error == EBADF) return 9;
    if (error == ENOSPC) return 28;
    if (error == EEXIST) return 17;
    if (error == EMFILE) return 24;
    if (error == EINVAL) return 22;
    return 5;
}
int windows_error() {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return 2;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return 13;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return 17;
    case ERROR_DISK_FULL: return 28;
    case ERROR_DIR_NOT_EMPTY: return 39;
    default: return 5;
    }
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle &) = delete;
};
void store_time(guest::Memory &m, Address out, FILETIME value) {
    auto ticks = (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    constexpr auto epoch = 116444736000000000ull;
    auto delta = static_cast<std::int64_t>(ticks - epoch);
    auto seconds = delta / 10'000'000, remainder = delta % 10'000'000;
    if (remainder < 0) { --seconds; remainder += 10'000'000; }
    m.store(out, seconds);
    m.store(out + 8, static_cast<std::uint64_t>(remainder * 100));
}
bool store_stat(guest::Memory &m, Address out, HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) return false;
    auto bytes = m.writable(out, 128);
    std::fill(bytes.begin(), bytes.end(), std::byte{});
    m.store(out, std::uint64_t(info.dwVolumeSerialNumber));
    m.store(out + 8, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
    bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    m.store(out + 16, std::uint32_t(directory ? 0040755 :
                                   0100000 | ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644)));
    m.store(out + 20, info.nNumberOfLinks);
    m.store(out + 24, std::uint32_t{10000});
    m.store(out + 28, std::uint32_t{10000});
    auto size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    m.store(out + 48, size);
    m.store(out + 56, std::int32_t{4096});
    m.store(out + 64, (size + 511) / 512);
    store_time(m, out + 72, info.ftLastAccessTime);
    store_time(m, out + 88, info.ftLastWriteTime);
    store_time(m, out + 104, info.ftCreationTime);
    return true;
}
// POSIX locks are advisory and belong to a guest process, not a Windows FILE handle.
// Lock a companion file so unrelated descriptors in the same guest can still do I/O.
// All runtime processes use the same companion and named mutex for atomic changes.
struct Range { std::uint64_t begin, end; short type; };
struct Locks {
    Handle file, mutex;
    std::vector<Range> ranges;
    explicit Locks(const fs::path &path)
        : file(CreateFileW((path.native() + L".acp-lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr)),
          mutex(nullptr) {
        auto key = utf8(fs::weakly_canonical(path));
        std::ranges::transform(key, key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        auto name = widen("Local\\ACP.FileLock." + sha256(std::as_bytes(std::span(key))));
        mutex.value = CreateMutexW(nullptr, FALSE, name.c_str());
    }
    bool apply(const Range &range, bool unlock) const {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(range.begin);
        position.OffsetHigh = static_cast<DWORD>(range.begin >> 32);
        auto size = range.end - range.begin;
        if (unlock)
            return UnlockFileEx(file.value, 0, static_cast<DWORD>(size), static_cast<DWORD>(size >> 32),
                                &position) != FALSE;
        return LockFileEx(file.value, LOCKFILE_FAIL_IMMEDIATELY |
                                          (range.type == 1 ? LOCKFILE_EXCLUSIVE_LOCK : 0),
                          0, static_cast<DWORD>(size), static_cast<DWORD>(size >> 32), &position) != FALSE;
    }
    bool replace(const std::vector<Range> &next) {
        for (const auto &range : ranges) apply(range, true);
        size_t done = 0;
        for (; done < next.size(); ++done)
            if (!apply(next[done], false)) break;
        if (done == next.size()) { ranges = next; return true; }
        for (size_t i = 0; i < done; ++i) apply(next[i], true);
        for (const auto &range : ranges)
            require(apply(range, false), "Cannot restore a guest file lock");
        return false;
    }
};
struct Files {
    std::recursive_mutex mutex;
    std::map<fs::path, std::shared_ptr<Locks>> locks;
    std::map<int, int> descriptor_bits;
    std::map<int, fs::path> locked_paths;
    void drop(const fs::path &path) {
        auto it = locks.find(path);
        if (it == locks.end()) return;
        for (const auto &range : it->second->ranges) it->second->apply(range, true);
        it->second->ranges.clear();
        locks.erase(it);
    }
    void close(int fd) {
        std::lock_guard guard(mutex);
        descriptor_bits.erase(fd);
        auto it = locked_paths.find(fd);
        if (it == locked_paths.end()) return;
        drop(it->second); // Closing ANY descriptor releases this guest's locks on the file.
        auto path = it->second;
        std::erase_if(locked_paths, [&](const auto &item) { return item.second == path; });
    }
};
struct Mapping { size_t size; unsigned permissions; bool anonymous; };
} // namespace

void install_bionic_offline(Runtime &r) {
    auto &m = r.memory;
    auto files = std::make_shared<Files>();
    r.bind("inet_pton", [&r, &m](auto &c) -> Address {
        int family = static_cast<int>(c.x(0));
        if (family != 2 && family != 10) return r.failure(c, 97);
        std::array<std::byte, 16> address{};
        auto text = m.string(c.x(1));
        auto result = InetPtonA(family == 2 ? AF_INET : AF_INET6, text.c_str(), address.data());
        if (result == 1) m.write(c.x(2), std::span(address).first(family == 2 ? 4 : 16));
        return result;
    });
    r.bind("inet_ntop", [&r, &m](auto &c) -> Address {
        int family = static_cast<int>(c.x(0));
        if (family != 2 && family != 10) { r.failure(c, 97); return 0; }
        auto address = m.readable(c.x(1), family == 2 ? 4 : 16);
        std::array<char, 64> text{};
        if (!InetNtopA(family == 2 ? AF_INET : AF_INET6, address.data(), text.data(), text.size())) {
            r.failure(c, 22); return 0;
        }
        auto size = std::strlen(text.data()) + 1;
        if (size > c.x(3)) { r.failure(c, 28); return 0; }
        m.write(c.x(2), std::as_bytes(std::span(text)).first(size));
        return c.x(2);
    });
    r.bind("poll", [&r, &m](auto &c) -> Address {
        if (c.x(1) > 4096) return r.failure(c, 22);
        unsigned ready = 0;
        for (size_t i = 0; i < c.x(1); ++i) {
            auto p = c.x(0) + i * 8;
            int fd = m.load<std::int32_t>(p);
            auto events = m.load<std::uint16_t>(p + 4);
            std::uint16_t result = fd < 0 ? 0 : r.file(fd) ? events & (1 | 4 | 0x40 | 0x100) : 0x20;
            m.store(p + 6, result);
            if (result) ++ready;
        }
        if (!ready) {
            auto timeout = static_cast<int>(c.x(2));
            auto start = std::chrono::steady_clock::now();
            while (!r.stopping && (timeout < 0 || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout))) {
                r.check_threads();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (r.stopping) return r.failure(c, 4);
        }
        return ready;
    });
    r.bind("select", [&r, &m](auto &c) -> Address {
        auto count = static_cast<int>(c.x(0));
        if (count < 0 || count > 1024) return r.failure(c, 22);
        unsigned ready = 0;
        for (unsigned set = 1; set <= 3; ++set) {
            if (!c.x(set)) continue;
            for (int fd = 0; fd < count; ++fd) {
                auto address = c.x(set) + (fd / 64) * 8;
                auto mask = 1ull << (fd % 64), value = m.load<std::uint64_t>(address);
                if (!(value & mask)) continue;
                if (!r.file(fd)) return r.failure(c, 9);
                if (set == 3) m.store(address, value & ~mask);
                else ++ready;
            }
        }
        if (!ready) {
            std::int64_t micros = -1;
            if (c.x(4)) {
                auto seconds = m.load<std::int64_t>(c.x(4)), fractional = m.load<std::int64_t>(c.x(4) + 8);
                if (seconds < 0 || seconds > INT64_MAX / 1'000'000 || fractional < 0 || fractional >= 1'000'000)
                    return r.failure(c, 22);
                micros = seconds * 1'000'000 + fractional;
            }
            auto start = std::chrono::steady_clock::now();
            while (!r.stopping && (micros < 0 || std::chrono::steady_clock::now() - start < std::chrono::microseconds(micros))) {
                r.check_threads();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (r.stopping) return r.failure(c, 4);
        }
        if (c.x(4)) { m.store(c.x(4), std::uint64_t{}); m.store(c.x(4) + 8, std::uint64_t{}); }
        return ready;
    });
    r.closing_file = [files, &r](int fd) {
        // Even a descriptor that did not acquire a lock cancels process locks on the same file.
        auto path = r.descriptor_path(fd);
        if (path) {
            std::lock_guard guard(files->mutex);
            files->drop(*path);
        }
        files->close(fd);
    };
    auto open = [&r](guest::Cpu &c, int directory, Address pointer, int flags) -> std::uint64_t {
        auto absolute = r.absolute_guest_path(r.memory.string(pointer), directory);
        if (!absolute) return r.failure(c, absolute.error());
        if (absolute->starts_with("/dev/")) {
            for (const auto *device : {"/dev/null", "/dev/random", "/dev/urandom", "/dev/zero"})
                if (*absolute == device) return r.open_device(*absolute);
            return r.failure(c, 2);
        }
        bool writing = (flags & 3) != 0 || (flags & (0x40 | 0x200)) != 0;
        auto path = r.resolve_path(*absolute, writing);
        if (!path) return r.failure(c, path.error());
        std::error_code error;
        if (fs::is_directory(*path, error)) {
            if (writing) return r.failure(c, 21);
            return r.open_directory(*path);
        }
        if (flags & 0x10000) return r.failure(c, fs::exists(*path) ? 20 : 2);
        int access = flags & 3;
        if (access == 3) return r.failure(c, 22);
        int host_flags = _O_BINARY | (access == 2 ? _O_RDWR : access == 1 ? _O_WRONLY : _O_RDONLY);
        if (flags & 0x40) host_flags |= _O_CREAT;
        if (flags & 0x80) host_flags |= _O_EXCL;
        if (flags & 0x200) host_flags |= _O_TRUNC;
        if (flags & 0x400) host_flags |= _O_APPEND;
        int host_fd = -1;
        auto result = _wsopen_s(&host_fd, path->c_str(), host_flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (result) { r.file_failure(*absolute, host_error(result)); return r.failure(c, host_error(result)); }
        auto fp = _wfdopen(host_fd, access == 2 ? L"r+b" : access == 1 ? L"wb" : L"rb");
        if (!fp) { _close(host_fd); return r.failure(c, host_error(errno)); }
        return r.adopt_file(fp, *path, flags);
    };
    for (const auto *name : {"open", "__open_2"})
        r.bind(name, [open](auto &c) { return open(c, -100, c.x(0), static_cast<int>(c.x(1))); });
    r.bind("openat", [open](auto &c) { return open(c, static_cast<int>(c.x(0)), c.x(1), static_cast<int>(c.x(2))); });
    r.bind("fdopen", [&r, files](auto &c) -> Address {
        int fd = static_cast<int>(c.x(0));
        if (!r.file(fd)) { r.failure(c, 9); return 0; }
        auto mode = r.memory.string(c.x(1));
        if (mode.empty() || std::string_view("rwa").find(mode.front()) == std::string_view::npos ||
            mode.substr(1).find_first_not_of("+be") != mode.npos || std::ranges::count(mode, '+') > 1 ||
            std::ranges::count(mode, 'b') > 1 || std::ranges::count(mode, 'e') > 1) { r.failure(c, 22); return 0; }
        int requested = mode.find('+') != mode.npos ? 2 : mode.front() == 'r' ? 0 : 1;
        std::lock_guard guard(r.state_mutex);
        auto actual = r.descriptor_flags.contains(fd) ? r.descriptor_flags.at(fd) : fd == 0 ? 0 : 1;
        if ((actual & 3) != 2 && requested != (actual & 3)) { r.failure(c, 22); return 0; }
        // Windows cannot change an existing CRT descriptor to O_APPEND without reopening it.
        if (mode.front() == 'a' && !(actual & 0x400)) { r.failure(c, 95); return 0; }
        return r.file_handle(fd);
    });
    r.bind("chdir", [&r](auto &c) -> Address {
        auto value = r.absolute_guest_path(r.memory.string(c.x(0)));
        if (!value) return r.failure(c, value.error());
        auto path = r.resolve_path(*value);
        if (!path) return r.failure(c, path.error());
        std::error_code error;
        if (!fs::exists(*path, error)) return r.failure(c, 2);
        if (!fs::is_directory(*path, error)) return r.failure(c, 20);
        auto normalized = utf8(path_from_utf8(*value).lexically_normal());
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        r.working_directory(std::move(normalized));
        return 0;
    });
    r.bind("getcwd", [&r, &m](auto &c) -> Address {
        auto text = r.working_directory();
        if (c.x(0) && !c.x(1)) { r.failure(c, 22); return 0; }
        if (c.x(1) && c.x(1) <= text.size()) { r.failure(c, 34); return 0; }
        auto out = c.x(0) ? c.x(0) : m.allocate(c.x(1) ? c.x(1) : text.size() + 1);
        m.write(out, std::as_bytes(std::span(text)));
        m.store(out + text.size(), char{});
        return out;
    });
    r.bind("realpath", [&r, &m](auto &c) -> Address {
        auto absolute = r.absolute_guest_path(m.string(c.x(0)));
        auto path = absolute ? r.resolve_path(*absolute) : std::expected<fs::path, int>(std::unexpected(absolute.error()));
        if (!path) { r.failure(c, path.error()); return 0; }
        std::error_code error;
        if (!fs::exists(*path, error)) { r.failure(c, 2); return 0; }
        auto text = utf8(path_from_utf8(*absolute).lexically_normal());
        std::replace(text.begin(), text.end(), '\\', '/');
        if (text.size() >= 4096) { r.failure(c, 36); return 0; }
        auto out = c.x(1) ? c.x(1) : m.allocate(text.size() + 1);
        m.write(out, std::as_bytes(std::span(text)));
        m.store(out + text.size(), char{});
        return out;
    });
    r.bind("unlinkat", [&r, &m](auto &c) -> Address {
        if (c.x(2) & ~0x200ull) return r.failure(c, 22);
        auto value = r.absolute_guest_path(m.string(c.x(1)), static_cast<int>(c.x(0)));
        if (!value) return r.failure(c, value.error());
        auto path = r.resolve_path(*value, true);
        if (!path) return r.failure(c, path.error());
        auto attributes = GetFileAttributesW(path->c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return r.failure(c, windows_error());
        bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directory != bool(c.x(2) & 0x200)) return r.failure(c, directory ? 21 : 20);
        bool removed = directory ? RemoveDirectoryW(path->c_str()) != FALSE : DeleteFileW(path->c_str()) != FALSE;
        return removed ? 0 : r.failure(c, windows_error());
    });
    r.bind("truncate", [&r, &m](auto &c) -> Address {
        if (static_cast<std::int64_t>(c.x(1)) < 0) return r.failure(c, 22);
        auto path = r.resolve_path(m.string(c.x(0)), true);
        if (!path) return r.failure(c, path.error());
        auto fd = r.open_file(*path, "r+b");
        if (fd < 0) return r.failure(c, host_error(errno));
        auto error = _chsize_s(_fileno(r.file(fd)), c.x(1));
        r.close_file(fd);
        return error ? r.failure(c, host_error(error)) : 0;
    });
    r.bind("ftruncate", [&r](auto &c) -> Address {
        auto fp = r.file(static_cast<int>(c.x(0)));
        if (!fp) return r.failure(c, 9);
        if (static_cast<std::int64_t>(c.x(1)) < 0) return r.failure(c, 22);
        if (std::fflush(fp)) return r.failure(c, host_error(errno));
        auto error = _chsize_s(_fileno(fp), c.x(1));
        return error ? r.failure(c, host_error(error)) : 0;
    });
    r.bind("fsync", [&r](auto &c) -> Address {
        auto fp = r.file(static_cast<int>(c.x(0)));
        if (!fp) return r.failure(c, 9);
        if (std::fflush(fp) || _commit(_fileno(fp))) return r.failure(c, host_error(errno));
        return 0;
    });
    r.bind("sendfile", [&r, &m](auto &c) -> Address {
        auto out = r.file(static_cast<int>(c.x(0))), in = r.file(static_cast<int>(c.x(1)));
        if (!in || !out) return r.failure(c, 9);
        auto input_path = r.descriptor_path(static_cast<int>(c.x(1)));
        auto output_path = r.descriptor_path(static_cast<int>(c.x(0)));
        if (in == out || (input_path && output_path && *input_path == *output_path)) return r.failure(c, 22);
        auto position = r.stream_position(static_cast<int>(c.x(1)));
        if (c.x(2)) {
            auto offset = m.load<std::int64_t>(c.x(2));
            if (offset < 0 || r.seek_stream(static_cast<int>(c.x(1)), offset, SEEK_SET)) return r.failure(c, 22);
        }
        size_t total = 0, count = static_cast<size_t>(std::min<std::uint64_t>(c.x(3), 0x7ffff000));
        std::array<std::byte, 65536> buffer;
        bool failed = false;
        while (total < count) {
            auto read = r.read_stream(static_cast<int>(c.x(1)), std::span(buffer).first(std::min(buffer.size(), count - total)));
            auto written = std::fwrite(buffer.data(), 1, read, out);
            total += written;
            if (written < read) { r.seek_stream(static_cast<int>(c.x(1)), static_cast<std::int64_t>(written) - static_cast<std::int64_t>(read), SEEK_CUR); failed = true; break; }
            if (!read) { failed = std::ferror(in) != 0; break; }
        }
        if (c.x(2)) { m.store(c.x(2), r.stream_position(static_cast<int>(c.x(1)))); r.seek_stream(static_cast<int>(c.x(1)), position, SEEK_SET); }
        return failed && !total ? r.failure(c, 5) : total;
    });
    r.bind("fchmodat", [&r, &m](auto &c) -> Address {
        if (c.x(3) & ~0x100ull) return r.failure(c, 22);
        auto value = r.absolute_guest_path(m.string(c.x(1)), static_cast<int>(c.x(0)));
        if (!value) return r.failure(c, value.error());
        auto path = r.resolve_path(*value, true);
        if (!path) return r.failure(c, path.error());
        auto mode = _S_IREAD | ((c.x(2) & 0222) ? _S_IWRITE : 0);
        return _wchmod(path->c_str(), mode) ? r.failure(c, host_error(errno)) : 0;
    });
    r.bind("utimensat", [&r, &m](auto &c) -> Address {
        if (c.x(3) & ~0x100ull) return r.failure(c, 22);
        auto value = r.absolute_guest_path(m.string(c.x(1)), static_cast<int>(c.x(0)));
        if (!value) return r.failure(c, value.error());
        auto path = r.resolve_path(*value, true);
        if (!path) return r.failure(c, path.error());
        Handle handle(CreateFileW(path->c_str(), FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) return r.failure(c, windows_error());
        FILETIME times[2]{}, now{};
        GetSystemTimeAsFileTime(&now);
        bool omit[2]{};
        for (unsigned i = 0; i < 2; ++i) {
            auto nanoseconds = c.x(2) ? m.load<std::int64_t>(c.x(2) + i * 16 + 8) : 0x3fffffff;
            omit[i] = nanoseconds == 0x3ffffffe;
            if (omit[i]) continue;
            if (nanoseconds == 0x3fffffff) { times[i] = now; continue; }
            if (nanoseconds < 0 || nanoseconds >= 1'000'000'000) return r.failure(c, 22);
            auto seconds = m.load<std::int64_t>(c.x(2) + i * 16);
            if (seconds < -11644473600ll || seconds > 910692730085ll) return r.failure(c, 75);
            auto ticks = std::uint64_t(seconds + 11644473600ll) * 10'000'000 + nanoseconds / 100;
            times[i] = {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        }
        return SetFileTime(handle.value, nullptr, omit[0] ? nullptr : &times[0], omit[1] ? nullptr : &times[1])
                   ? 0 : r.failure(c, windows_error());
    });
    for (const auto *name : {"stat", "stat64", "lstat", "lstat64"})
        r.bind(name, [&r, &m](auto &c) -> Address {
            auto path = r.resolve_path(m.string(c.x(0)));
            if (!path) return r.failure(c, path.error());
            Handle handle(CreateFileW(path->c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (handle.value == INVALID_HANDLE_VALUE) return r.failure(c, windows_error());
            return store_stat(m, c.x(1), handle.value) ? 0 : r.failure(c, windows_error());
        });
    r.bind("fstat", [&r, &m](auto &c) -> Address {
        auto fp = r.file(static_cast<int>(c.x(0)));
        if (fp) {
            std::fflush(fp);
            return store_stat(m, c.x(1), reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(fp))))
                       ? 0 : r.failure(c, windows_error());
        }
        auto path = r.descriptor_path(static_cast<int>(c.x(0)));
        if (!path) return r.failure(c, 9);
        Handle handle(CreateFileW(path->c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        return handle.value != INVALID_HANDLE_VALUE && store_stat(m, c.x(1), handle.value)
                   ? 0 : r.failure(c, windows_error());
    });
    r.bind("fcntl", [&r, &m, files](auto &c) -> Address {
        int fd = static_cast<int>(c.x(0)), command = static_cast<int>(c.x(1));
        auto fp = r.file(fd);
        auto path = r.descriptor_path(fd);
        if (!fp && !path) return r.failure(c, 9);
        int flags;
        { std::lock_guard guard(r.state_mutex); flags = r.descriptor_flags[fd]; }
        auto current_position = fp ? r.stream_position(fd) : 0;
        std::unique_lock guard(files->mutex);
        if (command == 1) return files->descriptor_bits[fd];
        if (command == 2) { files->descriptor_bits[fd] = static_cast<int>(c.x(2)) & 1; return 0; }
        if (command == 3) return flags & (3 | 0x400 | 0x800 | 0x10000);
        if (command == 4) {
            if ((c.x(2) & 0x400) != (flags & 0x400)) return r.failure(c, 95);
            return 0; // O_NONBLOCK has no effect on regular files.
        }
        if (command < 5 || command > 7) return r.failure(c, 22);
        if (!path || !fp || !c.x(2)) return r.failure(c, !c.x(2) ? 14 : 9);
        auto p = c.x(2);
        auto type = m.load<short>(p), whence = m.load<short>(p + 2);
        if (type < 0 || type > 2 || whence < 0 || whence > 2) return r.failure(c, 22);
        if (command == 5 && type == 2) return r.failure(c, 22);
        if (command != 5 && ((type == 0 && (flags & 3) == 1) || (type == 1 && (flags & 3) == 0)))
            return r.failure(c, 9);
        auto begin = m.load<std::int64_t>(p + 8), length = m.load<std::int64_t>(p + 16);
        std::int64_t base = whence == 1 ? current_position : whence == 2 ? _filelengthi64(_fileno(fp)) : 0;
        if ((base > 0 && begin > INT64_MAX - base) || (base < 0 && begin < INT64_MIN - base)) return r.failure(c, 75);
        begin += base;
        if (length < 0) {
            if (length == INT64_MIN || begin < -length) return r.failure(c, 22);
            begin += length; length = -length;
        }
        if (begin < 0 || (length > 0 && begin > INT64_MAX - length)) return r.failure(c, 22);
        Range requested{static_cast<std::uint64_t>(begin), length ? static_cast<std::uint64_t>(begin + length) : UINT64_MAX, type};
        auto &entry = files->locks[*path];
        if (!entry) entry = std::make_shared<Locks>(*path);
        auto retained = entry;
        auto &locks = *retained;
        if (locks.file.value == INVALID_HANDLE_VALUE || !locks.mutex.value) return r.failure(c, 13);
        files->locked_paths[fd] = *path;
        while (true) {
            auto wait = WaitForSingleObject(locks.mutex.value, 50);
            if (wait == WAIT_TIMEOUT) {
                if (r.stopping) return r.failure(c, 4);
                if (command != 7) return r.failure(c, 11);
                guard.unlock();
                r.check_threads();
                guard.lock();
                auto active = files->locks.find(*path);
                if (active == files->locks.end() || active->second != retained) return r.failure(c, 9);
                continue;
            }
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return r.failure(c, 5);
            auto previous = locks.ranges;
            if (command == 5) {
                locks.replace({});
                bool available = locks.apply(requested, false);
                if (available) locks.apply(requested, true);
                require(locks.replace(previous), "Cannot restore locks after F_GETLK");
                ReleaseMutex(locks.mutex.value);
                m.store(p, static_cast<short>(available ? 2 : 1));
                if (!available) { m.store(p + 2, short{0}); m.store(p + 8, begin); m.store(p + 16, length); m.store(p + 24, std::int32_t{-1}); }
                return 0;
            }
            std::vector<Range> next;
            for (const auto &range : previous) {
                if (range.end <= requested.begin || range.begin >= requested.end) next.push_back(range);
                else {
                    if (range.begin < requested.begin) next.push_back({range.begin, requested.begin, range.type});
                    if (range.end > requested.end) next.push_back({requested.end, range.end, range.type});
                }
            }
            if (type != 2) next.push_back(requested);
            bool acquired = locks.replace(next);
            ReleaseMutex(locks.mutex.value);
            if (acquired) return 0;
            if (command == 6) return r.failure(c, 11);
            if (r.stopping) return r.failure(c, 4);
            // F_SETLKW waits interruptibly and never sleeps with the cross-process mutex held.
            guard.unlock();
            r.check_threads();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            guard.lock();
            // Another guest thread may have closed the file while we waited.
            auto still_open = files->locks.find(*path);
            if (still_open == files->locks.end() || still_open->second != retained) return r.failure(c, 9);
        }
    });
    auto mappings = std::make_shared<std::map<Address, Mapping>>();
    for (const auto *name : {"mmap", "mmap64"})
        r.bind(name, [&r, &m, mappings](auto &c) -> Address {
            if (!c.x(1) || c.x(1) > (1ull << 30) || (c.x(3) & 0x10)) return r.failure(c, 22);
            if (!(c.x(3) & 0x20) && (c.x(3) & 1) && (c.x(2) & 2)) return r.failure(c, 95);
            auto size = align_up(c.x(1), 4096);
            unsigned permissions = static_cast<unsigned>(c.x(2) & 7);
            auto fp = (c.x(3) & 0x20) ? nullptr : r.file(static_cast<int>(c.x(4)));
            if (!(c.x(3) & 0x20) && !fp) return r.failure(c, 9);
            if (c.x(5) % 4096) return r.failure(c, 22);
            auto address = m.allocate(size, guest::Read | guest::Write, 4096);
            if (fp) {
                auto position = _ftelli64(fp);
                if (_fseeki64(fp, static_cast<std::int64_t>(c.x(5)), SEEK_SET)) { m.release(address); return r.failure(c, 22); }
                auto bytes = m.writable(address, c.x(1));
                std::fread(bytes.data(), 1, bytes.size(), fp);
                _fseeki64(fp, position, SEEK_SET);
            }
            m.protect(address, size, permissions);
            std::lock_guard guard(r.state_mutex);
            mappings->emplace(address, Mapping{size, permissions, fp == nullptr});
            return address;
        });
    r.bind("mremap", [&r, &m, mappings](auto &c) -> Address {
        std::lock_guard guard(r.state_mutex);
        auto it = mappings->find(c.x(0));
        if (it == mappings->end() || !c.x(1) || !c.x(2) || c.x(2) > (1ull << 30) ||
            align_up(c.x(1), 4096) != it->second.size || (c.x(3) & ~1ull)) return r.failure(c, 22);
        auto size = align_up(c.x(2), 4096);
        if (size == it->second.size) return it->first;
        if (size < it->second.size) {
            c.invalidate(it->first + size, it->second.size - size);
            m.shrink_pages(it->first, size);
            it->second.size = size;
            return it->first;
        }
        if (!it->second.anonymous) return r.failure(c, 95);
        if (!(c.x(3) & 1)) return r.failure(c, 12);
        auto out = m.allocate(size, guest::Read | guest::Write, 4096);
        // Temporarily grant read access even to a PROT_NONE source, then retire the old mapping.
        m.protect(it->first, it->second.size, guest::Read);
        m.write(out, m.readable(it->first, std::min(size, it->second.size)));
        m.protect(out, size, it->second.permissions);
        m.protect(it->first, it->second.size, 0);
        c.invalidate(it->first, it->second.size);
        m.release(it->first);
        auto permissions = it->second.permissions;
        mappings->erase(it);
        mappings->emplace(out, Mapping{size, permissions, true});
        return out;
    });
    r.bind("munmap", [&r, &m, mappings](auto &c) -> Address {
        std::lock_guard guard(r.state_mutex);
        auto it = mappings->find(c.x(0));
        if (it == mappings->end() || !c.x(1) || align_up(c.x(1), 4096) != it->second.size) return r.failure(c, 22);
        m.protect(it->first, it->second.size, 0);
        c.invalidate(it->first, it->second.size);
        m.release(it->first);
        mappings->erase(it);
        return 0;
    });
    r.bind("mprotect", [&r, &m, mappings](auto &c) -> Address {
        std::lock_guard guard(r.state_mutex);
        auto it = mappings->find(c.x(0));
        if (it == mappings->end() || !c.x(1) || align_up(c.x(1), 4096) != it->second.size || (c.x(2) & ~7ull))
            return r.failure(c, 22);
        it->second.permissions = static_cast<unsigned>(c.x(2));
        m.protect(c.x(0), it->second.size, it->second.permissions);
        c.invalidate(c.x(0), it->second.size);
        return 0;
    });
    // This is a pure ancillary-buffer iterator. It is useful even though socket I/O is disabled.
    r.bind("__cmsg_nxthdr", [&m](auto &c) -> Address {
        if (!m.valid(c.x(0), 56) || !m.valid(c.x(1), 16)) return 0;
        auto base = m.load<Address>(c.x(0) + 32), size = m.load<std::uint64_t>(c.x(0) + 40);
        auto current = c.x(1), length = m.load<std::uint64_t>(current);
        if (size > UINT64_MAX - base || length < 16 || current < base || current > base + size ||
            length > base + size - current || length > UINT64_MAX - 7) return 0;
        auto next = current + ((length + 7) & ~7ull);
        if (next > base + size || base + size - next < 16 || !m.valid(next, 16)) return 0;
        auto next_length = m.load<std::uint64_t>(next);
        return next_length >= 16 && next_length <= base + size - next ? next : 0;
    });
    for (const auto *name : {"chmod", "fchmod", "fchown", "lchown", "utime", "utimes"})
        r.bind_suppressed(name, "optional-posix", "-1 / ENOSYS; use fchmodat or utimensat for supported metadata",
                          [&r](auto &c) { return r.failure(c, 38); });
    r.bind_suppressed("ioctl", "unrelated-posix", "-1 / ENOTTY; Android device-control requests are unavailable",
                      [&r](auto &c) { return r.failure(c, 25); });
}
} // namespace acp::android

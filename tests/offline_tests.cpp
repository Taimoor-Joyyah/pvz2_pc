#include <acp/android.hpp>

#include <future>
#include <iostream>
namespace {
using namespace acp;
unsigned checks{};
void check(bool condition, std::string_view message) { ++checks; require(condition, message); }
Address call(android::Runtime &r, std::string_view name, std::initializer_list<Address> args) {
    return r.cpu.call(r.linker.symbol(name), std::span(args.begin(), args.size()));
}
Address wide(android::Runtime &r, std::u32string_view text) {
    auto p = r.memory.allocate((text.size() + 1) * 4);
    for (size_t i = 0; i < text.size(); ++i) r.memory.store(p + i * 4, std::uint32_t(text[i]));
    return p;
}
std::array<std::uint64_t, 2> floating(android::Runtime &r, std::string_view name, Address text, Address end) {
    auto &m = r.memory;
    auto code = m.allocate(4096, guest::Read | guest::Write, 4096), out = m.allocate(16);
    // Call the import from ARM64 and store q0 before Cpu::call restores the caller's registers.
    const std::array<std::uint32_t, 8> words{0xa9be7bfd, 0xf9000be2, 0x580000d0, 0xd63f0200,
                                            0xf9400be2, 0x3d800040, 0xa8c27bfd, 0xd65f03c0};
    m.write(code, std::as_bytes(std::span(words)));
    m.store(code + 32, r.linker.symbol(name));
    m.protect(code, 4096, guest::Read | guest::Execute);
    r.cpu.call(code, std::array{text, end, out});
    return m.load<std::array<std::uint64_t, 2>>(out);
}
void parsing(android::Runtime &r) {
    auto &m = r.memory;
    auto destination = m.allocate(128), end = m.allocate(8);
    auto input = m.string(" 0x2a 18446744073709551615 -7 1.25 abc 99 Z");
    auto format = m.string(" %i %llu %hhd %lf %3[a-z]%n %*d %c");
    auto assigned = call(r, "sscanf", {input, format, destination, destination + 8, destination + 16,
                             destination + 24, destination + 32, destination + 40, destination + 48});
    check(assigned == 6 &&
              m.load<int>(destination) == 42 && m.load<std::uint64_t>(destination + 8) == UINT64_MAX &&
              m.load<std::int8_t>(destination + 16) == -7 && m.load<double>(destination + 24) == 1.25 &&
              m.string(destination + 32) == "abc" && m.load<int>(destination + 40) == 38 &&
              m.load<char>(destination + 48) == 'Z',
          "scanf preserves ARM64 stack arguments, LP64, counts, scansets, suppression and floating destinations");
    check(call(r, "sscanf", {m.string(""), m.string("%d"), destination}) == UINT64_MAX &&
              call(r, "sscanf", {m.string("word"), m.string("%d"), destination}) == 0,
          "scanf distinguishes EOF from a matching failure");
    auto va = m.allocate(32), registers = m.allocate(16), stack = m.allocate(8);
    m.store(registers, destination); m.store(registers + 8, destination + 8); m.store(stack, destination + 16);
    m.store(va, stack); m.store(va + 8, registers + 16); m.store(va + 24, std::int32_t{-16});
    check(call(r, "vsscanf", {m.string("7 8 9"), m.string("%ld %lld %zu"), va}) == 3 &&
              m.load<std::uint64_t>(destination) == 7 && m.load<std::uint64_t>(destination + 8) == 8 &&
              m.load<std::uint64_t>(destination + 16) == 9,
          "vsscanf reads ARM64 register-save and stack areas");
    check(call(r, "swscanf", {wide(r, U"\U0001f331 4294967298 2.5"), wide(r, U"%lc %ld %lf"),
                              destination, destination + 8, destination + 16}) == 3 &&
              m.load<std::uint32_t>(destination) == 0x1f331 && m.load<std::int64_t>(destination + 8) == 4294967298ll &&
              m.load<double>(destination + 16) == 2.5,
          "swscanf uses Android UTF32 input and 64-bit long destinations");
    auto wide_integer = wide(r, U"  -4294967298tail");
    for (auto name : {"wcstol", "wcstoll"})
        check(call(r, name, {wide_integer, end, 10}) == static_cast<Address>(-4294967298ll) &&
                  m.load<Address>(end) == wide_integer + 13 * 4,
              "Wide signed conversions preserve LP64 values and UTF32 end pointers");
    for (auto name : {"wcstoul", "wcstoull"})
        check(call(r, name, {wide(r, U"18446744073709551615"), end, 10}) == UINT64_MAX,
              "Wide unsigned conversions preserve the full 64-bit range");
    auto wide_real = wide(r, U" 1.25x");
    check(std::bit_cast<double>(floating(r, "wcstod", wide_real, end)[0]) == 1.25 &&
              m.load<Address>(end) == wide_real + 5 * 4,
          "wcstod returns d0 and a UTF32 end pointer");
    check(std::bit_cast<float>(static_cast<std::uint32_t>(floating(r, "wcstof", wide_real, end)[0])) == 1.25f,
          "wcstof returns s0");
    const std::array<std::uint64_t, 2> quad_one_quarter{0, 0x3fff400000000000ull};
    for (auto name : {"strtold", "strtold_l", "wcstold"})
        check(floating(r, name, std::string_view(name) == "wcstold" ? wide(r, U"1.25") : m.string("1.25"), end) == quad_one_quarter,
              "Long-double conversions return actual IEEE binary128 in q0");
    check(floating(r, "strtold", m.string("0x1.0000000000000000000000000001p0"), end) ==
              std::array<std::uint64_t, 2>{1, 0x3fff000000000000ull},
          "Binary128 parsing retains the 113th significant bit");
    check(floating(r, "strtold", m.string("0x1p-16494"), end) == std::array<std::uint64_t, 2>{1, 0} &&
              m.load<int>(r.errno_address(r.cpu)) == 34,
          "Binary128 subnormals retain their bit pattern and report underflow");
    check(floating(r, "strtold", m.string("-1e99999"), end) ==
              std::array<std::uint64_t, 2>{0, 0xffff000000000000ull},
          "Binary128 overflow preserves the sign and returns infinity");
    check(call(r, "sscanf", {m.string("0x1.0000000000000000000000000001p0"), m.string("%Lf"), destination}) == 1 &&
              m.load<std::array<std::uint64_t, 2>>(destination) == std::array<std::uint64_t, 2>{1, 0x3fff000000000000ull},
          "scanf %Lf stores all 128 bits");
}
void files(android::Runtime &r) {
    auto &m = r.memory;
    const auto cwd = r.working_directory();
    auto path = m.string(cwd + "/offline.txt"), out = m.allocate(256);
    auto fd = call(r, "open", {path, 0x40 | 0x80 | 2, 0600});
    check(fd != UINT64_MAX, "open creates a new isolated file");
    check(call(r, "open", {path, 0x40 | 0x80 | 2, 0600}) == UINT64_MAX && m.load<int>(r.errno_address(r.cpu)) == 17,
          "O_EXCL does not overwrite an existing file");
    auto stream = call(r, "fdopen", {fd, m.string("r+")});
    check(stream && call(r, "fileno", {stream}) == fd, "fdopen retains descriptor ownership");
    check(call(r, "fputwc", {0x1f331, stream}) == 0x1f331 && call(r, "fseek", {stream, 0, 0}) == 0 &&
              call(r, "getwc", {stream}) == 0x1f331 && call(r, "ungetwc", {0x1f331, stream}) == 0x1f331 &&
              call(r, "getwc", {stream}) == 0x1f331,
          "Wide stream output, input and multibyte pushback round-trip non-BMP Unicode");
    check(call(r, "getwc", {stream}) == UINT32_MAX && call(r, "feof", {stream}) != 0 &&
              call(r, "ungetwc", {'A', stream}) == 'A' && call(r, "feof", {stream}) == 0 &&
              call(r, "getwc", {stream}) == 'A', "ungetwc clears EOF");
    check(call(r, "fseek", {stream, 0, 0}) == 0 && call(r, "fputs", {m.string("42 word!"), stream}) >= 0 &&
              call(r, "fflush", {stream}) == 0 && call(r, "fseek", {stream, 0, 0}) == 0 &&
              call(r, "fscanf", {stream, m.string("%d %4s"), out, out + 8}) == 2 &&
              m.load<int>(out) == 42 && m.string(out + 8) == "word" && call(r, "fgetc", {stream}) == '!',
          "fscanf leaves unread punctuation available to subsequent stdio");
    check(call(r, "ftruncate", {fd, 3}) == 0 && call(r, "fsync", {fd}) == 0,
          "File truncation and durable fsync succeed");
    check(call(r, "stat", {path, out}) == 0 && m.load<std::uint64_t>(out + 8) != 0 &&
              m.load<std::int64_t>(out + 48) == 3, "stat reports real file identity and size");
    auto inode = m.load<std::uint64_t>(out + 8);
    check(call(r, "fstat", {fd, out}) == 0 && m.load<std::uint64_t>(out + 8) == inode,
          "stat and fstat agree on file identity");
    auto offset = m.allocate(8); m.store(offset, std::int64_t{1});
    auto copy = call(r, "open", {m.string(cwd + "/copy.txt"), 0x40 | 2, 0600});
    auto position = call(r, "lseek", {fd, 0, 1});
    check(call(r, "sendfile", {copy, fd, offset, 2}) == 2 && m.load<std::int64_t>(offset) == 3 &&
              call(r, "lseek", {fd, 0, 1}) == position, "sendfile updates an explicit offset without moving the source descriptor");
    call(r, "close", {copy});
    auto directory = call(r, "open", {m.string(cwd), 0x10000});
    check(call(r, "fstat", {directory, out}) == 0 && (m.load<std::uint32_t>(out + 16) & 0170000) == 0040000 &&
              (m.load<std::uint32_t>(out + 16) & 0111) == 0111, "Directory stat retains traversal permission bits");
    check(call(r, "openat", {directory, m.string("offline.txt"), 0}) != UINT64_MAX &&
              call(r, "openat", {fd, m.string("x"), 0}) == UINT64_MAX && m.load<int>(r.errno_address(r.cpu)) == 20,
          "openat resolves directory descriptors and rejects regular-file bases");
    check(call(r, "mkdir", {m.string(cwd + "/sub"), 0700}) == 0 &&
              call(r, "chdir", {m.string(cwd + "/sub")}) == 0 &&
              call(r, "getcwd", {out, 256}) == out && m.string(out) == cwd + "/sub",
          "chdir changes only the guest's working directory");
    check(call(r, "chdir", {m.string(cwd)}) == 0, "Guest cwd can return to the save directory");
    auto canonical = call(r, "realpath", {path, 0});
    check(canonical && m.string(canonical) == cwd + "/offline.txt", "realpath returns a guest path, never a Windows path");
    auto times = m.allocate(32);
    m.store(times, std::int64_t{1451606400}); m.store(times + 8, std::int64_t{123456700});
    m.store(times + 16, std::int64_t{1451606500}); m.store(times + 24, std::int64_t{765432100});
    check(call(r, "utimensat", {directory, m.string("offline.txt"), times, 0}) == 0 &&
              call(r, "stat", {path, out}) == 0 && m.load<std::int64_t>(out + 88) == 1451606500 &&
              m.load<std::uint64_t>(out + 96) == 765432100, "utimensat preserves timestamps at Windows' 100ns precision");
    check(call(r, "fchmodat", {directory, m.string("offline.txt"), 0444, 0}) == 0 &&
              call(r, "fchmodat", {directory, m.string("offline.txt"), 0644, 0}) == 0,
          "fchmodat maps the Windows read-only attribute");
    check(call(r, "fclose", {stream}) == 0 && call(r, "fileno", {stream}) == UINT64_MAX,
          "Closing a stream invalidates its opaque guest handle");
    check(call(r, "truncate", {path, 8}) == 0 && call(r, "stat", {path, out}) == 0 && m.load<std::int64_t>(out + 48) == 8,
          "Path truncation extends an existing file");
    check(call(r, "unlinkat", {directory, m.string("copy.txt"), 0}) == 0 &&
              call(r, "unlinkat", {directory, m.string("sub"), 0x200}) == 0,
          "unlinkat distinguishes files and empty directories");
    check(call(r, "openat", {directory, m.string("../escape"), 0x40 | 2, 0600}) == UINT64_MAX,
          "Descriptor-relative paths cannot escape mounted storage");
    call(r, "close", {directory});
}
void locks(android::Runtime &r) {
    android::Runtime peer(r.options);
    auto &m = r.memory;
    auto path = r.working_directory() + "/locking.db";
    auto a = call(r, "open", {m.string(path), 0x40 | 2, 0600});
    auto a2 = call(r, "open", {m.string(path), 2});
    auto b = call(peer, "open", {peer.memory.string(path), 2});
    auto record = m.allocate(32), other = peer.memory.allocate(32);
    auto set = [](auto &memory, Address p, short type, std::int64_t start, std::int64_t length) {
        memory.store(p, type); memory.store(p + 2, short{0}); memory.store(p + 8, start); memory.store(p + 16, length);
    };
    set(m, record, 0, 100, 10); set(peer.memory, other, 0, 100, 10);
    check(call(r, "fcntl", {a, 6, record}) == 0 && call(peer, "fcntl", {b, 6, other}) == 0,
          "Separate guests can hold shared advisory locks");
    set(m, record, 1, 100, 10);
    check(call(r, "fcntl", {a, 6, record}) == UINT64_MAX && m.load<int>(r.errno_address(r.cpu)) == 11,
          "A conflicting exclusive upgrade fails with EAGAIN");
    set(peer.memory, other, 2, 100, 10); call(peer, "fcntl", {b, 6, other});
    check(call(r, "fcntl", {a2, 6, record}) == 0, "Lock ownership belongs to the guest, not an individual descriptor");
    set(peer.memory, other, 1, 100, 10);
    check(call(peer, "fcntl", {b, 5, other}) == 0 && peer.memory.load<short>(other) != 2,
          "F_GETLK detects another guest's lock");
    call(r, "close", {a2});
    set(peer.memory, other, 1, 100, 10);
    check(call(peer, "fcntl", {b, 6, other}) == 0, "Closing any descriptor releases the guest's locks for that file");
    set(m, record, 1, 100, 10);
    auto waiting = std::async(std::launch::async, [&] { return call(r, "fcntl", {a, 7, record}); });
    check(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
          "F_SETLKW waits for conflicting guest locks");
    set(peer.memory, other, 2, 100, 10); call(peer, "fcntl", {b, 6, other});
    check(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready && waiting.get() == 0,
          "F_SETLKW resumes after the lock is released");
    call(peer, "close", {b}); call(r, "close", {a});
}
void mappings_and_services(android::Runtime &r) {
    auto &m = r.memory;
    auto mapping = call(r, "mmap", {0, 8192, 3, 0x22, UINT64_MAX, 0});
    m.store(mapping, std::uint64_t{42});
    check(call(r, "mremap", {mapping, 8192, 4096, 0}) == mapping && m.load<std::uint64_t>(mapping) == 42,
          "mremap shrinks in place without requiring MAYMOVE");
    auto moved = call(r, "mremap", {mapping, 4096, 12288, 1});
    check(moved != UINT64_MAX && m.load<std::uint64_t>(moved) == 42 && m.load<int>(moved + 8192) == 0 &&
              !m.valid(mapping, 1), "mremap grows with data preserved and retires the old mapping");
    check(call(r, "munmap", {moved, 12288}) == 0 && !m.valid(moved, 1), "munmap revokes access");
    auto inaccessible = call(r, "mmap", {0, 4096, 0, 0x22, UINT64_MAX, 0});
    check(!m.valid(inaccessible, 1) && call(r, "mprotect", {inaccessible, 4096, 3}) == 0,
          "PROT_NONE retains its mapping so mprotect can restore access");
    m.store(inaccessible, std::uint64_t{99});
    check(call(r, "mprotect", {inaccessible, 4096, 0}) == 0, "Mapped pages can return to PROT_NONE");
    auto moved_none = call(r, "mremap", {inaccessible, 4096, 8192, 1});
    check(moved_none != UINT64_MAX && !m.valid(moved_none, 1) && call(r, "mprotect", {moved_none, 8192, 3}) == 0 &&
              m.load<std::uint64_t>(moved_none) == 99,
          "mremap preserves inaccessible data and its protection without a guest fault");
    call(r, "munmap", {moved_none, 8192});
    auto message = m.allocate(56), control = m.allocate(48);
    m.store(message + 32, control); m.store(message + 40, std::uint64_t{48});
    m.store(control, std::uint64_t{24}); m.store(control + 24, std::uint64_t{24});
    check(call(r, "__cmsg_nxthdr", {message, control}) == control + 24 &&
              call(r, "__cmsg_nxthdr", {message, control + 24}) == 0,
          "Ancillary iteration respects Android's 64-bit structure layout");
    m.store(control, UINT64_MAX);
    check(call(r, "__cmsg_nxthdr", {message, control}) == 0, "Malformed ancillary lengths cannot overflow");
    auto result = m.allocate(8); m.store(result, Address{123});
    check(call(r, "getaddrinfo", {m.string("example.invalid"), 0, 0, result}) == 8 && m.load<Address>(result) == 0,
          "Disabled DNS returns bionic EAI_NONAME and initializes the result");
    check(call(r, "socket", {2, 1, 0}) == UINT64_MAX && m.load<int>(r.errno_address(r.cpu)) == 101,
          "Disabled socket requests fail without creating a network handle");
    auto address = m.allocate(16), text = m.allocate(64);
    check(call(r, "inet_pton", {10, m.string("2001:db8::1"), address}) == 1 &&
              call(r, "inet_ntop", {10, address, text, 64}) == text && m.string(text) == "2001:db8::1",
          "Numeric IP conversion remains available without networking");
    check(call(r, "inet_ntop", {10, address, text, 2}) == 0 && m.load<int>(r.errno_address(r.cpu)) == 28,
          "Failed pointer-returning APIs return null, not an invalid all-ones pointer");
    bool reported = false;
    for (const auto &policy : r.report().at("suppressed_functions"))
        if (policy.at("name") == "socket" && policy.at("calls") == 1) reported = true;
    check(reported, "Suppressed functions are reported with call counts");
}
} // namespace
int main() {
    try {
        auto root = fs::current_path() / "test-artifacts" / path_from_utf8("offline-" + timestamp());
        android::Runtime r({"org.example.offline", "1", "en_US", 1, root, {}, {}});
        for (const auto *name : {"__cmsg_nxthdr", "chdir", "fchmodat", "fdopen", "fputwc", "fscanf", "getwc",
                                "mremap", "openat", "realpath", "sendfile", "sscanf", "strtold", "strtold_l",
                                "swscanf", "truncate", "ungetwc", "unlinkat", "utimensat", "vsscanf", "wcstod",
                                "wcstof", "wcstol", "wcstold", "wcstoll", "wcstoul", "wcstoull"})
            check(r.linker.symbol(name) != 0, "Every previously unresolved import has an explicit implementation");
        parsing(r); files(r); locks(r); mappings_and_services(r);
        write_json(root / "report.json", r.report());
        std::cout << checks << " offline compatibility checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Offline check " << checks << ": " << error.what() << '\n';
        return 1;
    }
}

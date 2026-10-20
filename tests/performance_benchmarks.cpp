#include <acp/android.hpp>

#include <Windows.h>

#include <iostream>

namespace {
using namespace acp;
using Clock = std::chrono::steady_clock;

// One host entry executes many real ARM64 -> SVC -> host -> ARM64 transitions.
// x19..x25 retain the target, count, arguments and caller LR across each import.
Address import_loop(guest::Memory &memory) {
    std::vector<std::uint32_t> words;
    auto mov = [&](unsigned destination, unsigned source) {
        words.push_back(0xaa0003e0u | (source << 16) | destination);
    };
    for (unsigned i = 0; i < 6; ++i)
        mov(19 + i, i);
    mov(25, 30);
    auto loop = words.size();
    for (unsigned i = 0; i < 4; ++i)
        mov(i, 21 + i);
    words.push_back(0xd63f0260u); // blr x19
    words.push_back(0xf1000694u); // subs x20, x20, #1
    auto displacement = static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(words.size());
    words.push_back(0x54000001u | ((static_cast<std::uint32_t>(displacement) & 0x7ffffu) << 5));
    mov(30, 25);
    words.push_back(0xd65f03c0u);
    auto code = memory.allocate(4096, guest::Read | guest::Write, 4096);
    memory.write(code, std::as_bytes(std::span(words)));
    memory.protect(code, 4096, guest::Read | guest::Execute);
    return code;
}

// A deterministic symbol-heavy ELF keeps linker measurements independent of game files.
Bytes symbol_fixture() {
    constexpr size_t count = 4096, symbols = 0x100;
    std::string names(1, '\0');
    std::vector<std::uint32_t> offsets;
    for (size_t i = 0; i < count; ++i) {
        offsets.push_back(static_cast<std::uint32_t>(names.size()));
        names += std::format("symbol_{:04}", i);
        names += '\0';
    }
    auto strings = symbols + (count + 1) * 24;
    auto sections = align_up(strings + names.size(), 8);
    Bytes bytes(sections + 3 * 64);
    auto put = [&]<class T>(size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    };
    put(0, std::uint32_t{0x464c457f});
    put(4, std::uint8_t{2});
    put(5, std::uint8_t{1});
    put(6, std::uint8_t{1});
    put(16, std::uint16_t{3});
    put(18, std::uint16_t{183});
    put(20, std::uint32_t{1});
    put(32, std::uint64_t{64});
    put(40, sections);
    put(52, std::uint16_t{64});
    put(54, std::uint16_t{56});
    put(56, std::uint16_t{2});
    put(58, std::uint16_t{64});
    put(60, std::uint16_t{3});
    put(64, std::uint32_t{1});
    put(68, std::uint32_t{6});
    put(96, std::uint64_t{bytes.size()});
    put(104, std::uint64_t{align_up(bytes.size(), 4096)});
    put(112, std::uint64_t{4096});
    put(120, std::uint32_t{2});
    put(124, std::uint32_t{6});
    put(128, std::uint64_t{0xc0});
    put(136, std::uint64_t{0xc0});
    put(152, std::uint64_t{48});
    put(160, std::uint64_t{48});
    put(168, std::uint64_t{8});
    put(0xc0, std::uint64_t{5});
    put(0xc8, std::uint64_t{strings});
    put(0xd0, std::uint64_t{10});
    put(0xd8, std::uint64_t{names.size()});
    for (size_t i = 0; i < count; ++i) {
        auto offset = symbols + (i + 1) * 24;
        put(offset, offsets[i]);
        put(offset + 4, std::uint8_t{0x12});
        put(offset + 6, std::uint16_t{1});
        put(offset + 8, std::uint64_t{0x80});
    }
    std::memcpy(bytes.data() + strings, names.data(), names.size());
    auto section = [&](size_t index, std::uint32_t type, size_t offset, size_t size, std::uint32_t link,
                       size_t stride) {
        auto at = sections + index * 64;
        put(at + 4, type);
        put(at + 24, std::uint64_t{offset});
        put(at + 32, std::uint64_t{size});
        put(at + 40, link);
        put(at + 48, std::uint64_t{8});
        put(at + 56, std::uint64_t{stride});
    };
    section(1, 11, symbols, (count + 1) * 24, 2, 24);
    section(2, 3, strings, names.size(), 0, 0);
    return bytes;
}

template <class F> Json measure(size_t operations, F work) {
    work(); // Warm translations, DLL dispatch, CRT and reusable allocations.
    std::vector<double> samples;
    for (size_t i = 0; i < 9; ++i) {
        auto start = Clock::now();
        work();
        samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
                          operations);
    }
    auto ordered = samples;
    std::ranges::sort(ordered);
    return {{"operations_per_sample", operations},
            {"samples_ns_per_operation", samples},
            {"median_ns_per_operation", ordered[ordered.size() / 2]},
            {"min_ns_per_operation", ordered.front()},
            {"max_ns_per_operation", ordered.back()}};
}
} // namespace

int main(int argc, char **argv) {
    using namespace acp;
    try {
        require(argc >= 2 && argc <= 4, "Usage: performance_benchmarks OUTPUT.json [LABEL] [--graphics]");
        const bool graphics = argc == 4 && std::string_view(argv[3]) == "--graphics";
        require(argc < 4 || graphics, "Expected --graphics as the fourth argument");
        auto root = fs::current_path() / "test-artifacts/performance";
        android::Runtime r({"org.example.performance", "1", "en_US", 1, root / "data", {}, {}});
        auto &m = r.memory;
        auto loop = import_loop(m);
        auto leaf = r.traps.add("benchmark/leaf", [](auto &c) { c.x(0, 42); });
        Json cases = Json::object();
        auto imports = [&](std::string_view label, Address target, size_t count,
                           std::array<Address, 4> arguments = {}) {
            cases[label] = measure(count, [&] {
                r.cpu.call(loop,
                           std::array{target, Address{count}, arguments[0], arguments[1], arguments[2],
                                      arguments[3]},
                           1'000'000'000);
            });
        };
        imports("import_dispatch", leaf, 200'000);
        auto source = m.string(std::string(65535, 'a'));
        auto destination = m.allocate(65536);
        imports("memcpy_64", r.linker.symbol("memcpy"), 100'000, {destination, source, 64, 0});
        imports("strlen_32", r.linker.symbol("strlen"), 100'000, {source + 65535 - 32});
        imports("strlen_4095", r.linker.symbol("strlen"), 10'000, {source + 65535 - 4095});
        imports("strncmp_prefix_32", r.linker.symbol("strncmp"), 10'000, {source, source, 32});
        auto jump = m.allocate(192);
        imports("setjmp", r.linker.symbol("setjmp"), 100'000, {jump});
        auto allocate = r.linker.symbol("malloc"), reallocate = r.linker.symbol("realloc"),
             release = r.linker.symbol("free");
        constexpr size_t allocations = 10'000;
        cases["malloc_free_4096"] = measure(allocations, [&] {
            for (size_t i = 0; i < allocations; ++i) {
                auto address = r.cpu.call(allocate, std::array<Address, 1>{4096});
                r.cpu.call(release, std::array{address});
            }
        });
        cases["realloc_shrink_4096"] = measure(allocations, [&] {
            for (size_t i = 0; i < allocations; ++i) {
                auto address = r.cpu.call(allocate, std::array<Address, 1>{4096});
                auto smaller = r.cpu.call(reallocate, std::array<Address, 2>{address, 2048});
                r.cpu.call(release, std::array{smaller});
            }
        });
        auto fixture = symbol_fixture();
        write_bytes(root / "symbols.so", fixture);
        r.linker.load(root / "symbols.so");
        std::uint64_t checksum = 0;
        cases["linker_last_symbol_4096"] = measure(20'000, [&] {
            for (size_t i = 0; i < 20'000; ++i)
                checksum += r.linker.symbol("symbol_4095");
        });
        cases["linker_host_symbol_4096"] = measure(20'000, [&] {
            for (size_t i = 0; i < 20'000; ++i)
                checksum += r.linker.symbol("memcpy");
        });
        if (graphics) {
            r.host->open_window("Compatibility bridge performance measurement", 128, 128);
            r.graphics->open(r.host->render_window(), {});
            r.graphics->vsync(false);
            auto ids = m.allocate(4);
            r.cpu.call(r.linker.symbol("glGenTextures"), std::array<Address, 2>{1, ids});
            r.cpu.call(r.linker.symbol("glBindTexture"),
                       std::array<Address, 2>{0x0de1, m.load<unsigned>(ids)});
            imports("glTexParameteri", r.linker.symbol("glTexParameteri"), 100'000,
                    {0x0de1, 0x2801, 0x2601});
            require(r.cpu.call(r.linker.symbol("glGetError")) == 0, "Graphics benchmark failed");
        }
        std::array<char, 512> processor{};
        GetEnvironmentVariableA("PROCESSOR_IDENTIFIER", processor.data(),
                                static_cast<DWORD>(processor.size()));
        Json report{{"schema", 1},
                    {"label", argc >= 3 ? argv[2] : "unlabeled"},
                    {"timestamp_utc", timestamp()},
                    {"compiler", _MSC_FULL_VER},
                    {"architecture", "Windows x64"},
                    {"cpu", processor.data()},
                    {"logical_processors", std::thread::hardware_concurrency()},
                    {"scope", "synthetic host/ARM64 import microbenchmarks; not gameplay or GPU time"},
                    {"warmup_samples", 1},
                    {"measured_samples", 9},
                    {"fixture_sha256", sha256(fixture)},
                    {"checksum", checksum},
                    {"cases", cases},
                    {"guest_memory", m.statistics()}};
        if (graphics)
            report["graphics"] = r.graphics->describe();
        write_json(path_from_utf8(argv[1]), report);
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

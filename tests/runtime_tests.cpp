#include <acp/android.hpp>

#include <future>
#include <iostream>
namespace {
using namespace acp;
unsigned checks = 0;
void check(bool value, std::string_view message) {
    ++checks;
    require(value, message);
}
template <class F> void rejects(F fn, std::string_view message) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, message);
}
void input_replay() {
    Json document{{"schema", 1},
                  {"events",
                   {{{"frame", 2}, {"type", "down"}, {"x", 0.25}, {"y", 0.5}},
                    {{"frame", 3}, {"type", "move"}, {"x", 1.0}, {"y", 1.0}},
                    {{"frame", 3}, {"type", "up"}, {"x", 0.75}, {"y", 0.25}},
                    {{"frame", 4}, {"type", "text"}, {"text", "test"}},
                    {{"frame", 5}, {"type", "quit"}}}}};
    host::InputReplay replay(document);
    check(replay.take(1, 1280, 720).empty(), "Replay waits for the requested frame");
    auto events = replay.take(2, 1280, 720);
    check(events.size() == 1 && events[0].type == host::EventType::PointerDown && events[0].x == 320 &&
              events[0].y == 360,
          "Replay maps normalized coordinates to drawable pixels");
    events = replay.take(3, 1920, 1080);
    check(events.size() == 2 && events[0].x == 1919 && events[0].y == 1079 &&
              events[1].type == host::EventType::PointerUp && events[1].x == 1440,
          "Replay preserves event order and follows resized drawables");
    events = replay.take(6, 1920, 1080);
    check(events.size() == 2 && events[0].text == "test" && events[1].type == host::EventType::Quit &&
              replay.delivered() == 5 && replay.remaining() == 0 && replay.take(7, 1920, 1080).empty(),
          "Replay delivers each event once, including after skipped frames");
    auto invalid = document;
    invalid["events"][1]["frame"] = 1;
    rejects([&] { host::InputReplay value(invalid); }, "Replay rejects unordered frames");
    invalid = document;
    invalid["events"][0]["x"] = 1.1;
    rejects([&] { host::InputReplay value(invalid); }, "Replay rejects coordinates outside the drawable");
    invalid = document;
    invalid["events"][0]["frame"] = -1;
    rejects([&] { host::InputReplay value(invalid); }, "Replay rejects negative frames");
    invalid["events"][0]["frame"] = 1.5;
    rejects([&] { host::InputReplay value(invalid); }, "Replay rejects fractional frames");
    invalid = document;
    invalid["events"][0]["type"] = "unknown";
    rejects([&] { host::InputReplay value(invalid); }, "Replay rejects unknown event kinds");
}
void package_contracts() {
    Bytes dex(196);
    auto put = [&]<class T>(size_t offset, T value) {
        std::memcpy(dex.data() + offset, &value, sizeof(T));
    };
    put(0, std::uint32_t{0x0a786564});
    put(40, std::uint32_t{0x12345678});
    put(56, std::uint32_t{6});
    put(60, std::uint32_t{112});
    put(64, std::uint32_t{3});
    put(68, std::uint32_t{136});
    put(80, std::uint32_t{2});
    put(84, std::uint32_t{148});
    put(96, std::uint32_t{1});
    put(100, std::uint32_t{164});
    for (unsigned i = 0; i < 3; ++i)
        put(136 + i * 4, i);
    put(150, std::uint16_t{1});
    put(152, std::uint32_t{3});
    put(158, std::uint16_t{2});
    put(160, std::uint32_t{4});
    unsigned index = 0;
    for (const auto *text :
         {"Lorg/example/Constants;", "J", "Ljava/lang/String;", "NEGATIVE", "TEXT", "encoded"}) {
        put(112 + index++ * 4, static_cast<std::uint32_t>(dex.size()));
        auto length = std::strlen(text);
        dex.push_back(static_cast<std::byte>(length));
        for (size_t i = 0; i <= length; ++i)
            dex.push_back(static_cast<std::byte>(text[i]));
    }
    put(164 + 24, static_cast<std::uint32_t>(dex.size()));
    for (unsigned byte : {2, 0, 0, 0, 0, 0x19, 1, 0x19})
        dex.push_back(static_cast<std::byte>(byte));
    auto values = dex.size();
    put(164 + 28, static_cast<std::uint32_t>(values));
    for (unsigned byte : {2, 6, 0xfb, 0x17, 5})
        dex.push_back(static_cast<std::byte>(byte));
    put(32, static_cast<std::uint32_t>(dex.size()));
    auto constants = android::dex_constants(dex);
    check(constants.size() == 2 && constants[0].name == "NEGATIVE" &&
              constants[0].value.get<std::uint64_t>() == static_cast<std::uint64_t>(-5ll),
          "DEX signed static constants retain their 64-bit bit pattern");
    check(constants[1].signature == "Ljava/lang/String;" && constants[1].value == "encoded",
          "DEX static string constants resolve the string table");
    dex[values + 1] = std::byte{0xe4};
    rejects([&] { android::dex_constants(dex); }, "Malformed DEX encoded scalar widths must fail");
    Json old_library{{"sha256", "old"},
                     {"needed", {"libc.so"}},
                     {"jni_exports", {"Java_example_old"}},
                     {"undefined", {{{"name", "malloc"}}}}};
    Json previous{
        {"manifest", {{"package", "org.example"}, {"version_name", "1"}}},
        {"libraries", {{"libtest.so", old_library}}},
        {"native_methods", {{{"class", "Lorg/example/Test;"}, {"name", "old"}, {"signature", "()V"}}}}};
    auto current = previous;
    current["manifest"]["version_name"] = "2";
    current["libraries"]["libtest.so"]["sha256"] = "new";
    current["libraries"]["libtest.so"]["needed"].push_back("libandroid.so");
    current["libraries"]["libtest.so"]["undefined"].push_back({{"name", "AAssetManager_open"}});
    current["libraries"]["libtest.so"]["jni_exports"] = {"Java_example_new"};
    current["native_methods"][0]["signature"] = "(I)V";
    auto report = android::compare_package_contracts(previous, current);
    check(report["libraries"]["libtest.so"]["status"] == "changed" &&
              report["native_imports"]["added"] == Json::array({"AAssetManager_open"}),
          "Update reports identify changed native binaries and new imports");
    check(report["libraries"]["libtest.so"]["dependencies"]["added"] == Json::array({"libandroid.so"}) &&
              report["native_methods"]["removed"].size() == 1 &&
              report["native_methods"]["added"].size() == 1,
          "Update reports detect dependency and Java/native signature changes");
    check(report["support_status"] == "unverified" && report["regression_testing_required"] == true,
          "Static update comparison never qualifies an untested version");
}
void memory_and_cpu() {
    guest::Memory memory;
    guest::TrapTable traps(memory);
    guest::Cpu cpu(memory, traps);
    auto address = memory.allocate(64);
    memory.store(address, std::uint64_t{0x123456789abcdef0});
    check(memory.load<std::uint64_t>(address) == 0x123456789abcdef0, "Guest integer round trip");
    rejects([&] { memory.load<int>(0); }, "Null guest reads must fault");
    memory.release(address);
    auto reused = memory.allocate(32);
    check(reused == address, "Freed guest memory should be reused");
    auto small = memory.allocate(32), large = memory.allocate(256), separator = memory.allocate(16);
    memory.release(small);
    memory.release(large);
    check(memory.allocate(288) == small, "Adjacent free ranges coalesce in both allocation indexes");
    memory.release(separator);
    rejects([&] { memory.release(address + 1); }, "Interior frees must fail");
    auto code = memory.allocate(4096, guest::Read | guest::Write, 4096);
    memory.store(code, 0x8b010000u);
    memory.store(code + 4, 0xd65f03c0u);
    memory.protect(code, 4096, guest::Read | guest::Execute);
    check(cpu.call(code, std::array<std::uint64_t, 2>{17, 25}) == 42, "ARM64 integer arguments and return");
    rejects([&] { memory.store(code, 0u); }, "Read-only guest code must reject writes");
    auto trap = traps.add("test/reentrant", [&](auto &c) {
        auto result = c.call(code, std::array<std::uint64_t, 2>{c.x(0), 10});
        c.x(0, result + 1);
    });
    check(cpu.call(trap, std::array<std::uint64_t, 1>{31}) == 42,
          "Deferred traps must allow reentrant guest callbacks");
    auto fault = memory.allocate(4096, guest::Read | guest::Write, 4096);
    memory.store(fault, 0xf9400000u);
    memory.store(fault + 4, 0xd65f03c0u);
    memory.protect(fault, 4096, guest::Read | guest::Execute);
    rejects([&] { cpu.call(fault, std::array<std::uint64_t, 1>{0}); },
            "Guest faults must return diagnostics instead of crashing the host");
    rejects([&] { cpu.call(address); }, "Calling data memory must fail");
    auto stack_code = memory.allocate(4096, guest::Read | guest::Write, 4096);
    const std::array<std::uint32_t, 4> stack_words{0xf94003e0, 0xf94007e1, 0x8b010000, 0xd65f03c0};
    memory.write(stack_code, std::as_bytes(std::span(stack_words)));
    memory.protect(stack_code, 4096, guest::Read | guest::Execute);
    check(cpu.call(stack_code, std::array<std::uint64_t, 10>{1, 2, 3, 4, 5, 6, 7, 8, 20, 22}) == 42,
          "AAPCS64 stack arguments beyond x7");
}
Bytes elf_fixture() {
    Bytes bytes(0x600);
    auto put = [&]<class T>(size_t offset, T value) {
        require(offset + sizeof(T) <= bytes.size(), "Fixture write overflow");
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put(0, std::uint32_t{0x464c457f});
    put(4, std::uint8_t{2});
    put(5, std::uint8_t{1});
    put(6, std::uint8_t{1});
    put(16, std::uint16_t{3});
    put(18, std::uint16_t{183});
    put(20, std::uint32_t{1});
    put(32, std::uint64_t{64});
    put(40, std::uint64_t{0x400});
    put(52, std::uint16_t{64});
    put(54, std::uint16_t{56});
    put(56, std::uint16_t{2});
    put(58, std::uint16_t{64});
    put(60, std::uint16_t{5});
    auto segment = [&](size_t p, std::uint32_t type, std::uint32_t flags, std::uint64_t offset,
                       std::uint64_t size, std::uint64_t memory) {
        put(p, type);
        put(p + 4, flags);
        put(p + 8, offset);
        put(p + 16, offset);
        put(p + 32, size);
        put(p + 40, memory);
        put(p + 48, std::uint64_t{4096});
    };
    segment(64, 1, 7, 0, bytes.size(), 4096);
    segment(120, 2, 6, 0x100, 96, 96);
    size_t d = 0x100;
    for (auto [tag, value] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             {5, 0x200}, {10, 22}, {6, 0x240}, {11, 24}, {12, 0x190}, {0, 0}}) {
        put(d, tag);
        put(d + 8, value);
        d += 16;
    }
    put(0x180, 0x8b010000u);
    put(0x184, 0xd65f03c0u);
    const std::array<std::uint32_t, 5> ctor{0xa9bf7bfd, 0x580001f0, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x190, ctor.data(), sizeof(ctor));
    const char names[] = "\0sample_add\0host_ping\0";
    static_assert(sizeof(names) == 23);
    std::memcpy(bytes.data() + 0x200, names, 22);
    put(0x258, std::uint32_t{1});
    put(0x25c, std::uint8_t{0x12});
    put(0x25e, std::uint16_t{1});
    put(0x260, std::uint64_t{0x180});
    put(0x268, std::uint64_t{8});
    put(0x270, std::uint32_t{12});
    put(0x274, std::uint8_t{0x12});
    put(0x2b0, std::uint64_t{0x1d0});
    put(0x2b8, (std::uint64_t{2} << 32) | 1026);
    auto section = [&](size_t index, std::uint32_t type, std::uint64_t offset, std::uint64_t size,
                       std::uint32_t link, std::uint64_t stride) {
        auto p = 0x400 + index * 64;
        put(p + 4, type);
        put(p + 24, offset);
        put(p + 32, size);
        put(p + 40, link);
        put(p + 48, std::uint64_t{8});
        put(p + 56, stride);
    };
    section(1, 11, 0x240, 72, 2, 24);
    section(2, 3, 0x200, 22, 0, 0);
    section(3, 4, 0x2b0, 24, 1, 24);
    section(4, 6, 0x100, 96, 2, 16);
    return bytes;
}
void elf_loading() {
    auto bytes = elf_fixture();
    guest::ElfImage parsed(bytes);
    check(parsed.symbols().size() == 3 && parsed.relocations().size() == 1,
          "ELF symbol and relocation decoding");
    auto file = fs::current_path() / "test-artifacts/native-test.so";
    write_bytes(file, bytes);
    guest::Memory memory;
    guest::TrapTable traps(memory);
    guest::Cpu cpu(memory, traps);
    guest::Linker linker(memory, traps);
    auto &image = linker.load(file);
    check(linker.unresolved() == std::vector<std::string>{"host_ping"},
          "Required unresolved imports are diagnosed");
    rejects([&] { linker.relocate(); }, "Required missing symbols must prevent relocation");
    int initialized = 0;
    linker.provide("host_ping", [&](auto &c) {
        ++initialized;
        c.x(0, 0);
    });
    linker.relocate();
    check(memory.load<Address>(image.base + 0x1d0) == linker.symbol("host_ping"),
          "ELF JUMP_SLOT relocation points to a guest trap");
    linker.initialize(cpu);
    linker.initialize(cpu);
    check(initialized == 1, "ELF constructors run exactly once");
    check(cpu.call(linker.symbol("sample_add"), std::array<std::uint64_t, 2>{19, 23}) == 42,
          "Loaded ARM64 ELF export execution");
    bytes.resize(64);
    rejects([&] { guest::ElfImage invalid(bytes); }, "Truncated ELF tables must be rejected");
}
void jni_and_storage() {
    auto root = fs::current_path() / "test-artifacts" / path_from_utf8("runtime-" + timestamp());
    android::Runtime r({"org.example.native",
                        "1.0",
                        "en_US",
                        1,
                        root / "data",
                        root / "obb/main.1.org.example.native.obb",
                        {}});
    auto &j = *r.jni;
    auto &m = r.memory;
    auto text = j.string("Hello \xc3\xa9");
    check(j.text(text) == "Hello \xc3\xa9", "JNI UTF16 conversion");
    auto table = m.load<Address>(j.environment());
    auto baseline_references = j.describe().at("live_references").get<size_t>();
    Address retained{};
    j.with_local_frame(r.cpu, [&] {
        auto nested = j.object("[Ljava/lang/Object;");
        auto child = j.object("java/util/HashMap");
        j.get(child).elements = {j.global_ref(j.string("key")), j.global_ref(j.string("value"))};
        j.get(nested).elements = {j.global_ref(child)};
        retained = j.global_ref(nested);
        return std::uint64_t{};
    });
    auto child = j.get(retained).elements[0];
    check(j.text(j.get(child).elements[1]) == "value",
          "Retained JNI containers keep their elements alive after a local frame ends");
    j.delete_ref(retained);
    check(j.describe().at("live_references").get<size_t>() == baseline_references,
          "Releasing a JNI container releases transitive array and map element references");
    auto call = [&](size_t slot, std::initializer_list<std::uint64_t> rest) {
        std::vector<std::uint64_t> args{j.environment()};
        args.insert(args.end(), rest);
        return r.cpu.call(m.load<Address>(table + slot * 8), args);
    };
    check(call(164, {text}) == 7, "JNI string length counts UTF16 units");
    auto chars = call(169, {text, 0});
    check(call(167, {0}) == 0, "Android NewStringUTF accepts an absent optional UTF8 string");
    check(m.string(chars) == "Hello \xc3\xa9", "JNI modified UTF8 output");
    call(170, {text, chars});
    auto array = call(179, {4});
    auto buffer = call(187, {array, 0});
    m.store(buffer + 8, std::int32_t{42});
    call(195, {array, buffer, 0});
    check(BinaryView(j.get(array).bytes).get<std::int32_t>(8) == 42, "JNI array copy-back");
    rejects([&] { call(203, {array, 3, 2, m.allocate(8)}); }, "JNI array bounds must be checked");
    auto clazz = j.class_ref("org/example/Test");
    j.hook("org/example/Test", "answer", "()I", [](auto &) { return 42; });
    auto mid = call(113, {clazz, m.string("answer"), m.string("()I")});
    check(call(129, {clazz, mid}) == 42, "JNI method resolution and invocation");
    auto child_type = j.class_ref("org/example/Child");
    j.set_superclass("org/example/Child", "org/example/Test");
    j.hook("org/example/Test", "value", "()I", [](auto &) { return 17; });
    j.hook("org/example/Child", "value", "()I", [](auto &) { return 42; });
    auto instance = j.object("org/example/Child");
    auto parent_method = call(33, {clazz, m.string("value"), m.string("()I")});
    check(call(49, {instance, parent_method}) == 42, "JNI virtual calls use the receiver override");
    check(call(79, {instance, clazz, parent_method}) == 17,
          "JNI nonvirtual calls use the requested superclass");
    auto constructor = call(33, {child_type, m.string("<init>"), m.string("()V")});
    rejects([&] { call(28, {child_type, constructor}); },
            "Unknown constructors must not inherit Object initialization");
    auto activity_type = j.class_ref("android/app/Activity");
    auto audio_field =
        call(144, {activity_type, m.string("AUDIO_SERVICE"), m.string("Ljava/lang/String;")});
    check(j.text(call(145, {activity_type, audio_field})) == "audio",
          "JNI static fields resolve through Android inheritance");
    r.options.target_sdk = 36;
    auto context = j.object("android/app/Activity");
    auto app_info = call(34, {context, call(33, {activity_type, m.string("getApplicationInfo"),
                                                 m.string("()Landroid/content/pm/ApplicationInfo;")})});
    auto app_info_type = j.class_ref("android/content/pm/ApplicationInfo");
    auto target_sdk = call(94, {app_info_type, m.string("targetSdkVersion"), m.string("I")});
    auto flags = call(94, {app_info_type, m.string("flags"), m.string("I")});
    check(call(100, {app_info, target_sdk}) == 36 && (call(100, {app_info, flags}) & 2) == 0,
          "ApplicationInfo exposes the package target SDK and release flags");
    auto package_name = call(94, {app_info_type, m.string("packageName"), m.string("Ljava/lang/String;")});
    check(j.text(call(95, {app_info, package_name})) == r.options.package,
          "Inherited instance fields resolve through the Android class hierarchy");
    check(r.path("/data/data/org.example.native/files/save.dat") ==
              fs::weakly_canonical(root / "data/files/save.dat"),
          "Android internal storage mapping");
    check(r.path("/sdcard/Android/data/org.example.native/files/save.dat") ==
              fs::weakly_canonical(root / "data/external/Android/data/org.example.native/files/save.dat"),
          "Android external application storage is separate from private files");
    check(r.cpu.call(r.linker.symbol("mkdir"), std::array<Address, 2>{m.string(""), 0700}) == UINT64_MAX &&
              m.load<int>(r.errno_address(r.cpu)) == 2,
          "mkdir on an empty path returns ENOENT without aborting the guest");
    rejects([&] { r.path("/data/data/org.example.native/files/../../outside", true); },
            "Guest traversal must be rejected");
    rejects([&] { r.path("C:/Windows/win.ini"); }, "Host paths must be rejected");
    rejects([&] { r.path("/sdcard/Android/obb/org.example.native/a.obb", true); },
            "OBB writes must be rejected");
    auto v = host::fit_viewport(1920, 1200, 1280, 720);
    check(v.width == 1920 && v.height == 1080 && v.y == 60, "Aspect ratio viewport");
    check(v.guest_x(960) == 0.5f && v.guest_y(600) == 0.5f, "Viewport input mapping");
    check(r.cpu.call(r.linker.symbol("malloc"), std::array<std::uint64_t, 1>{64}) != 0,
          "Android malloc bridge");
    auto seconds = m.allocate(8), calendar = m.allocate(56);
    m.store(seconds, std::int64_t{0});
    check(r.cpu.call(r.linker.symbol("gmtime_r"), std::array{seconds, calendar}) == calendar &&
              m.load<int>(calendar + 20) == 70 && m.load<int>(calendar + 12) == 1 &&
              m.load<std::int64_t>(calendar + 40) == 0 && m.string(m.load<Address>(calendar + 48)) == "UTC",
          "Android tm layout includes UTC offset and a valid timezone string");
    check(r.cpu.call(r.linker.symbol("localtime_r"), std::array{seconds, calendar}) == calendar &&
              !m.string(m.load<Address>(calendar + 48)).empty(),
          "Android localtime returns the host timezone without a null tm_zone");
    m.store(seconds, std::int64_t{-1});
    check(r.cpu.call(r.linker.symbol("gmtime_r"), std::array{seconds, calendar}) == calendar &&
              m.load<int>(calendar + 20) == 69 && m.load<int>(calendar) == 59,
          "Guest UTC conversion supports timestamps before 1970");
    auto stream =
        r.cpu.call(r.linker.symbol("fopen"), std::array{m.string("fortified.bin"), m.string("wb+")});
    check(stream != 0, "Guest stdio creates a persistent file");
    auto written = m.string("data"), destination = m.allocate(8);
    check(r.cpu.call(r.linker.symbol("__fwrite_chk"), std::array<Address, 5>{written, 1, 4, stream, 4}) ==
              4,
          "Bionic fortified fwrite uses the stream and buffer-capacity arguments in ABI order");
    rejects(
        [&] {
            r.cpu.call(r.linker.symbol("__fwrite_chk"), std::array<Address, 5>{written, 1, 4, stream, 3});
        },
        "Fortified stdio rejects buffers smaller than the requested transfer");
    r.cpu.call(r.linker.symbol("fseek"), std::array<Address, 3>{stream, 0, 0});
    check(r.cpu.call(r.linker.symbol("__fread_chk"),
                     std::array<Address, 5>{destination, 1, 4, stream, 8}) == 4 &&
              m.string(destination) == "data",
          "Fortified stdio reads the saved bytes back");
    r.cpu.call(r.linker.symbol("fclose"), std::array{stream});
    auto sink = r.cpu.call(r.linker.symbol("fopen"), std::array{m.string("/dev/null"), m.string("wb")});
    check(sink != 0 &&
              r.cpu.call(r.linker.symbol("fwrite"), std::array<Address, 4>{written, 1, 4, sink}) == 4,
          "Guest stdio can serialize to the Android null device");
    check(r.cpu.call(r.linker.symbol("fclose"), std::array{sink}) == 0,
          "Closing a virtual stdio device releases its host stream");
    auto output = m.allocate(8), varargs = m.allocate(32), arguments = m.allocate(16);
    m.store(arguments, m.string("guest"));
    m.store(arguments + 8, std::int64_t{4294967298});
    m.store(varargs + 8, arguments + 16);
    m.store(varargs + 24, std::int32_t{-16});
    auto formatted = std::string("guest:4294967298");
    check(r.cpu.call(r.linker.symbol("vasprintf"), std::array{output, m.string("%s:%lld"), varargs}) ==
                  formatted.size() &&
              m.string(m.load<Address>(output)) == formatted,
          "vasprintf allocates guest output and marshals ARM64 va_list with 64-bit integers");
    r.cpu.call(r.linker.symbol("free"), std::array{m.load<Address>(output)});
    auto guest_wide = [&](std::u32string_view text) {
        auto address = m.allocate((text.size() + 1) * 4);
        m.write(address, std::as_bytes(std::span(text)));
        m.store(address + text.size() * 4, std::uint32_t{});
        return address;
    };
    auto wide_output = m.allocate(128);
    check(r.cpu.call(r.linker.symbol("swprintf"),
                     std::array<Address, 4>{wide_output, 32, guest_wide(U"_DYNAMIC%d_"), 17}) == 11 &&
              m.load<std::uint32_t>(wide_output + 4) == U'D' &&
              m.load<std::uint32_t>(wide_output + 10 * 4) == U'_',
          "Wide formatting uses 32-bit Android wchar_t for the game's dynamic label");
    m.store(arguments, guest_wide(U"\U0001f331XYZ"));
    m.store(arguments + 8, std::int64_t{4294967298});
    m.store(varargs + 8, arguments + 16);
    m.store(varargs + 24, std::int32_t{-16});
    check(r.cpu.call(r.linker.symbol("vswprintf"),
                     std::array{wide_output, Address{32}, guest_wide(U"%4.2ls:%lld"), varargs}) == 15 &&
              m.load<std::uint32_t>(wide_output) == U' ' &&
              m.load<std::uint32_t>(wide_output + 8) == 0x1f331 &&
              m.load<std::uint32_t>(wide_output + 12) == U'X' &&
              m.load<std::uint32_t>(wide_output + 60) == 0,
          "ARM64 wide va_list preserves Unicode character width, precision and LP64 integers");
    check(r.cpu.call(r.linker.symbol("swprintf"),
                     std::array<Address, 4>{wide_output, 3, guest_wide(U"%d"), 1234}) == UINT64_MAX &&
              m.load<int>(r.errno_address(r.cpu)) == 75 && m.load<std::uint32_t>(wide_output + 8) == 0,
          "Truncated swprintf terminates the buffer and reports Android EOVERFLOW");
    check(r.cpu.call(r.linker.symbol("swprintf"),
                     std::array<Address, 3>{wide_output, 0, guest_wide(U"x")}) == UINT64_MAX &&
              m.load<int>(r.errno_address(r.cpu)) == 22,
          "Zero-capacity swprintf reports EINVAL");
    check(r.cpu.call(r.linker.symbol("syscall"), std::array<std::uint64_t, 1>{999}) == UINT64_MAX &&
              m.load<int>(r.errno_address(r.cpu)) == 38,
          "C syscall normalizes Linux errors to -1 and thread-local errno");
    auto raw_syscall = m.allocate(4096, guest::Read | guest::Write, 4096);
    const std::array<std::uint32_t, 3> raw_words{0xd2807ce8, 0xd4000001, 0xd65f03c0};
    m.write(raw_syscall, std::as_bytes(std::span(raw_words)));
    m.protect(raw_syscall, 4096, guest::Read | guest::Execute);
    check(r.cpu.call(raw_syscall) == static_cast<std::uint64_t>(-38ll),
          "Raw ARM64 SVC retains the Linux negative errno convention");
    r.post_callback([](auto &) { throw Error("A future timer ran before its deadline"); },
                    std::chrono::hours(1));
    r.wait_callbacks(std::chrono::seconds(1));
    std::promise<std::chrono::steady_clock::time_point> completed;
    auto future = completed.get_future();
    auto posted = std::chrono::steady_clock::now();
    r.post_callback(
        [&](auto &cpu) {
            require(cpu.tls() != r.cpu.tls() && r.jni->environment(cpu) != 0,
                    "Timers execute on an attached guest callback thread");
            completed.set_value(std::chrono::steady_clock::now());
        },
        std::chrono::milliseconds(20));
    check(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "Scheduled callbacks wake without a frame-loop poll");
    check(future.get() >= posted + std::chrono::milliseconds(20),
          "Scheduled callbacks respect their deadline");
    r.wait_callbacks();
}
void worker_failure_waits() {
    for (const auto *operation : {"sem_wait", "pthread_mutex_lock", "pthread_cond_timedwait"}) {
        auto root = fs::current_path() / "test-artifacts" / path_from_utf8("failure-" + timestamp());
        android::Runtime r({"org.example.failure", "1", "en_US", 1, root, {}, {}});
        auto object = r.memory.allocate(64), mutex = r.memory.allocate(40);
        auto timeout = r.memory.allocate(16);
        auto invoke = [&](std::string_view name, std::initializer_list<Address> args) {
            return r.cpu.call(r.linker.symbol(name), std::span(args.begin(), args.size()));
        };
        std::promise<void> locked, release;
        auto released = release.get_future();
        std::thread holder;
        if (std::string_view(operation) == "sem_wait")
            invoke("sem_init", {object, 0, 0});
        else if (std::string_view(operation) == "pthread_mutex_lock") {
            invoke("pthread_mutex_init", {object, 0});
            auto host_mutex = r.mutexes.at(object);
            holder = std::thread([host_mutex, &locked, &released] {
                std::lock_guard lock(*host_mutex);
                locked.set_value();
                released.wait();
            });
            locked.get_future().wait();
        } else {
            invoke("pthread_mutex_init", {mutex, 0});
            invoke("pthread_mutex_lock", {mutex});
            auto deadline = std::chrono::system_clock::now() + std::chrono::hours(1);
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(deadline.time_since_epoch());
            r.memory.store(timeout, seconds.count());
            r.memory.store(timeout + 8, std::int64_t{0});
        }
        r.post_callback([](auto &) { throw Error("deliberate worker failure"); },
                        std::chrono::milliseconds(20));
        auto start = std::chrono::steady_clock::now();
        bool reported = false;
        try {
            invoke(operation, {object, mutex, timeout});
        } catch (const Error &error) {
            reported =
                std::string_view(error.what()).find("deliberate worker failure") != std::string_view::npos;
        }
        release.set_value();
        if (holder.joinable())
            holder.join();
        check(reported && std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
              "Blocking guest synchronization reports worker failure promptly");
        check(!r.report().at("worker_errors").empty(), "Worker failures remain in the diagnostic report");
    }
}
} // namespace
int main() {
    try {
        check(sha256(std::span<const std::byte>{}) ==
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA256 empty input");
        Bytes content(2 << 20, std::byte{42});
        auto hash_file = fs::current_path() / "test-artifacts/hash.bin";
        write_bytes(hash_file, content);
        check(sha256(hash_file) == sha256(content), "Streaming file hashes match in-memory hashes");
        check(is_safe_relative("assets/file.txt"), "Safe relative path");
        check(!is_safe_relative("../outside"), "Archive traversal rejection");
        rejects([] { android::parse_manifest(Bytes(16)); }, "Malformed manifests must fail");
        rejects([] { android::dex_native_methods(Bytes(112)); }, "Malformed DEX must fail");
        input_replay();
        package_contracts();
        memory_and_cpu();
        elf_loading();
        jni_and_storage();
        worker_failure_waits();
        std::cout << checks << " contract checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "After " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}

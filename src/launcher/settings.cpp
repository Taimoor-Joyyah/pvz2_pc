#include <acp/launcher.hpp>

#include <Windows.h>

namespace acp::launcher {
namespace {
std::string mode_name(host::WindowMode mode) {
    switch (mode) {
    case host::WindowMode::Windowed:
        return "windowed";
    case host::WindowMode::Maximized:
        return "maximized";
    case host::WindowMode::Fullscreen:
        return "fullscreen";
    }
    throw Error("Invalid window mode");
}
fs::path resolve(const fs::path &directory, const Json &object, const char *name,
                 const fs::path &fallback) {
    auto value = object.value(name, utf8(fallback));
    if (value.empty())
        return {};
    auto path = path_from_utf8(value);
    return fs::absolute(path.is_absolute() ? path : directory / path).lexically_normal();
}
} // namespace
fs::path default_config_path() {
    return host::executable_directory() / "launcher.json";
}
Settings defaults() {
    Settings value;
    auto directory = host::executable_directory();
    value.executable = directory / "pvz2pc-game.exe";
    value.root = host::default_data_root();
    // A source checkout and a portable distribution both work without depending on CWD.
    for (auto candidate = directory; !candidate.empty();) {
        if (fs::is_regular_file(candidate / ".runtime/active.json")) {
            value.root = candidate / ".runtime";
            break;
        }
        auto parent = candidate.parent_path();
        if (parent == candidate)
            break;
        candidate = std::move(parent);
    }
    return value;
}
void validate(const Settings &value) {
    auto dimensions = [](int width, int height) {
        return width >= 64 && height >= 64 && width <= 8192 && height <= 8192 &&
               std::uint64_t(width) * height <= 16'777'216;
    };
    require(!value.root.empty(), "Select an installation directory");
    require(dimensions(value.window_width, value.window_height),
            "Invalid window size (64..8192, at most 16M pixels)");
    require(dimensions(value.render_width, value.render_height),
            "Invalid render resolution (64..8192, at most 16M pixels)");
    require(value.volume >= 0 && value.volume <= 100, "Volume must be between 0 and 100");
    require(value.fps_limit >= 0 && value.fps_limit <= 240,
            "Frame limit must be 0 (game controlled) or 1..240");
    require(std::ranges::all_of(value.profile,
                                [](unsigned char c) {
                                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                           (c >= '0' && c <= '9') || c == '-' || c == '_';
                                }),
            "Profile names may contain only letters, digits, hyphens and underscores");
    require(value.version.empty() || is_safe_relative(value.version),
            "Invalid installation version identifier");
    require(value.version.find_first_of("/\\") == value.version.npos,
            "Version must be an installation identifier");
    mode_name(value.window_mode);
}
Json describe(const Settings &value) {
    return {{"schema", 1},
            {"game_executable", utf8(value.executable)},
            {"runtime_root", utf8(value.root)},
            {"profile", value.profile},
            {"version", value.version},
            {"angle_directory", utf8(value.angle_directory)},
            {"game_config", utf8(value.game_config)},
            {"display",
             {{"mode", mode_name(value.window_mode)},
              {"width", value.window_width},
              {"height", value.window_height},
              {"render_mode", value.native_resolution ? "native" : "custom"},
              {"render_width", value.render_width},
              {"render_height", value.render_height},
              {"vsync", value.vsync}}},
            {"audio", {{"volume", value.volume}, {"muted", value.muted}}},
            {"performance", {{"fps_limit", value.fps_limit}}}};
}
void save(const fs::path &file, const Settings &value) {
    validate(value);
    write_json(fs::absolute(file), describe(value));
}
Settings load(const fs::path &file, bool create) {
    auto value = defaults();
    if (!fs::exists(file)) {
        if (create)
            save(file, value);
        return value;
    }
    auto data = read_json(file);
    require(data.at("schema") == 1, "Unsupported launcher configuration schema");
    auto directory = fs::absolute(file).parent_path();
    value.executable = resolve(directory, data, "game_executable", value.executable);
    value.root = resolve(directory, data, "runtime_root", value.root);
    value.angle_directory = resolve(directory, data, "angle_directory", {});
    value.game_config = resolve(directory, data, "game_config", {});
    value.profile = data.value("profile", "");
    value.version = data.value("version", "");
    auto display = data.value("display", Json::object());
    auto mode = display.value("mode", "windowed");
    require(mode == "windowed" || mode == "maximized" || mode == "fullscreen", "Unknown display mode");
    value.window_mode = mode == "fullscreen"  ? host::WindowMode::Fullscreen
                        : mode == "maximized" ? host::WindowMode::Maximized
                                              : host::WindowMode::Windowed;
    value.window_width = display.value("width", value.window_width);
    value.window_height = display.value("height", value.window_height);
    auto render_mode = display.value("render_mode", "native");
    require(render_mode == "native" || render_mode == "custom", "Unknown render mode");
    value.native_resolution = render_mode == "native";
    value.render_width = display.value("render_width", value.render_width);
    value.render_height = display.value("render_height", value.render_height);
    value.vsync = display.value("vsync", value.vsync);
    auto audio = data.value("audio", Json::object());
    value.volume = audio.value("volume", value.volume);
    value.muted = audio.value("muted", value.muted);
    value.fps_limit = data.value("performance", Json::object()).value("fps_limit", value.fps_limit);
    validate(value);
    return value;
}
std::wstring quote_argument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto ch : argument) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'"')
            result += L'\\';
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
void *start_game(const Settings &value, const fs::path &configuration) {
    validate(value);
    require(fs::is_regular_file(value.executable) &&
                _wcsicmp(value.executable.extension().c_str(), L".exe") == 0,
            "Select an existing game executable (.exe)");
    require(fs::is_regular_file(configuration), "Save launcher settings before starting the game");
    auto executable = fs::absolute(value.executable);
    std::wstring module(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    require(length > 0 && length < module.size(), "Cannot identify the launcher executable");
    module.resize(length);
    require(!fs::equivalent(executable, fs::path(module)), "Select the game executable, not the launcher");
    auto command = quote_argument(executable.native()) + L" run --launcher-config " +
                   quote_argument(fs::absolute(configuration).native());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    auto directory = executable.parent_path().native();
    auto started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                  directory.c_str(), &startup, &process);
    auto failure = GetLastError();
    require(started != FALSE, "Cannot start the game (Windows error " + std::to_string(failure) + ")");
    CloseHandle(process.hThread);
    return process.hProcess;
}
} // namespace acp::launcher

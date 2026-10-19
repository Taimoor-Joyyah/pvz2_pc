#include <acp/launcher.hpp>

#include <Windows.h>

#include <iostream>
#include <shellapi.h>

int main() {
    using namespace acp;
    try {
        auto root = fs::current_path() / "test-artifacts" / path_from_utf8("launcher-" + timestamp());
        auto file = root / "launcher.json";
        auto settings = launcher::defaults();
        settings.root = root / "runtime";
        settings.executable = root / "game with spaces.exe";
        settings.profile = "qa-launcher";
        settings.version = "com.example-1063";
        settings.window_mode = host::WindowMode::Fullscreen;
        settings.window_width = 1600;
        settings.window_height = 900;
        settings.native_resolution = false;
        settings.render_width = 2560;
        settings.render_height = 1440;
        settings.vsync = false;
        settings.volume = 37;
        settings.muted = true;
        settings.fps_limit = 120;
        launcher::save(file, settings);
        auto loaded = launcher::load(file);
        require(launcher::describe(loaded) == launcher::describe(settings),
                "Launcher settings must round-trip every configured option");
        auto contents = read_json(file);
        contents["runtime_root"] = "relative/runtime";
        contents["game_executable"] = "bin/game.exe";
        contents["angle_directory"] = "angle";
        write_json(file, contents);
        loaded = launcher::load(file);
        require(loaded.root == fs::absolute(root / "relative/runtime") &&
                    loaded.executable == fs::absolute(root / "bin/game.exe") &&
                    loaded.angle_directory == fs::absolute(root / "angle"),
                "All relative paths resolve beside the configuration, independently of working directory");
        for (auto dimension : {-1, 0, 8193}) {
            auto invalid = settings;
            invalid.window_width = dimension;
            bool rejected{};
            try {
                launcher::validate(invalid);
            } catch (const Error &) {
                rejected = true;
            }
            require(rejected, "Invalid window dimensions must be rejected before launch");
        }
        auto invalid = settings;
        invalid.profile = "../default";
        bool rejected{};
        try {
            launcher::validate(invalid);
        } catch (const Error &) {
            rejected = true;
        }
        require(rejected, "Profiles cannot escape their storage directory");
        std::vector<std::wstring> arguments{L"game.exe",  L"",     L"a b",    L"trailing space ",
                                            L"C:\\a b\\", L"a\"b", L"x\\\"y", L"Unicode \u03a9"};
        std::wstring command;
        for (const auto &argument : arguments) {
            if (!command.empty())
                command += L' ';
            command += launcher::quote_argument(argument);
        }
        int count{};
        auto decoded = CommandLineToArgvW(command.c_str(), &count);
        require(decoded && count == int(arguments.size()),
                "Quoted process command line must preserve argument count");
        for (int index = 0; index < count; ++index)
            require(arguments[index] == decoded[index],
                    "Quoting must preserve spaces, Unicode, quotes and trailing backslashes");
        LocalFree(decoded);
        contents["display"]["mode"] = "unknown";
        write_json(file, contents);
        rejected = false;
        try {
            launcher::load(file);
        } catch (const Error &) {
            rejected = true;
        }
        require(rejected, "Unknown display modes fail with a configuration error");
        std::cout << "Launcher persistence, relative paths, validation and process quoting passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

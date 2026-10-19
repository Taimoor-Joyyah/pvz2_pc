#include <acp/launcher.hpp>
#include <acp/pvz2.hpp>

#include <Windows.h>

#include "replay.hpp"
#include <charconv>
#include <iostream>
namespace {
using namespace acp;
struct Arguments {
    std::string command;
    std::map<std::string, std::vector<std::string>> values;
    std::set<std::string> flags;
    Arguments(int argc, wchar_t **argv) {
        int first = 1;
        command = "run";
        if (argc > 1 && !std::wstring_view(argv[1]).starts_with(L"--")) {
            command = narrow(argv[1]);
            first = 2;
        }
        for (int i = first; i < argc; ++i) {
            auto key = narrow(argv[i]);
            if (key == "--probe" || key == "--verify" || key == "--live-input" || key == "--fullscreen" ||
                key == "--windowed" || key == "--maximized" || key == "--vsync" || key == "--no-vsync" ||
                key == "--mute" || key == "--performance-profile") {
                flags.insert(key);
                continue;
            }
            require(key.starts_with("--"), "Expected an option: " + key);
            require(i + 1 < argc, "Missing value for " + key);
            values[key].push_back(narrow(argv[++i]));
        }
    }
    std::optional<std::string> get(const std::string &key) const {
        auto it = values.find(key);
        return it == values.end() ? std::nullopt : std::optional(it->second.back());
    }
    fs::path path(const std::string &key, fs::path fallback = {}) const {
        auto value = get(key);
        return value ? path_from_utf8(*value) : fallback;
    }
    std::uint64_t count(const std::string &key) const {
        auto value = get(key);
        if (!value)
            return 0;
        std::uint64_t result{};
        auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), result);
        require(error == std::errc{} && end == value->data() + value->size(),
                key + " requires a nonnegative integer");
        return result;
    }
};
class LogRedirect {
    std::ofstream file_;
    std::streambuf *out_{}, *error_{};

  public:
    void open(const fs::path &path) {
        fs::create_directories(path.parent_path());
        file_.open(path, std::ios::binary | std::ios::trunc);
        require(bool(file_), "Cannot create game log: " + utf8(path));
        out_ = std::cout.rdbuf(file_.rdbuf());
        error_ = std::cerr.rdbuf(file_.rdbuf());
    }
    ~LogRedirect() {
        if (out_)
            std::cout.rdbuf(out_);
        if (error_)
            std::cerr.rdbuf(error_);
    }
};
} // namespace
int acp::launcher::run_game(int argc, wchar_t **argv, bool graphical) {
    using namespace acp;
    LogRedirect log_redirect;
    std::unique_ptr<android::GameAdapter> adapter;
    std::unique_ptr<android::Runtime> runtime;
    fs::path report_path;
    std::uint64_t frames = 0;
    Json frame_captures = Json::array();
    std::optional<ReplaySession> replay;
    games::Pvz2Options game_options;
    launcher::Settings launch_settings;
    fs::path launcher_config, log_path;
    const auto launch_start = std::chrono::steady_clock::now();
    try {
        Arguments args(argc, argv);
        launcher_config = args.path("--launcher-config", launcher::default_config_path());
        launch_settings = launcher::load(launcher_config);
        launch_settings.root = fs::absolute(args.path("--root", launch_settings.root));
        if (auto value = args.get("--profile"))
            launch_settings.profile = *value;
        if (auto value = args.get("--version"))
            launch_settings.version = *value;
        launch_settings.angle_directory = args.path("--angle-dir", launch_settings.angle_directory);
        auto number = [&](const char *name, int &value) {
            if (args.get(name)) {
                auto count = args.count(name);
                require(count <= INT_MAX, std::string(name) + " is too large");
                value = static_cast<int>(count);
            }
        };
        number("--width", launch_settings.window_width);
        number("--height", launch_settings.window_height);
        number("--render-width", launch_settings.render_width);
        number("--render-height", launch_settings.render_height);
        number("--volume", launch_settings.volume);
        number("--fps", launch_settings.fps_limit);
        require(int(args.flags.contains("--fullscreen")) + int(args.flags.contains("--windowed")) +
                        int(args.flags.contains("--maximized")) <=
                    1,
                "Select one window mode");
        if (args.flags.contains("--fullscreen"))
            launch_settings.window_mode = host::WindowMode::Fullscreen;
        if (args.flags.contains("--maximized"))
            launch_settings.window_mode = host::WindowMode::Maximized;
        if (args.flags.contains("--windowed"))
            launch_settings.window_mode = host::WindowMode::Windowed;
        require(!args.flags.contains("--vsync") || !args.flags.contains("--no-vsync"),
                "Conflicting vsync options");
        if (args.flags.contains("--vsync"))
            launch_settings.vsync = true;
        if (args.flags.contains("--no-vsync"))
            launch_settings.vsync = false;
        if (args.flags.contains("--mute"))
            launch_settings.muted = true;
        if (auto mode = args.get("--render-mode")) {
            require(*mode == "native" || *mode == "custom", "Render mode must be native or custom");
            launch_settings.native_resolution = *mode == "native";
        } else if (args.get("--render-width") || args.get("--render-height"))
            launch_settings.native_resolution = false;
        launcher::validate(launch_settings);
        auto root = launch_settings.root;
        if (args.command == "settings") {
            auto value = launcher::describe(launch_settings);
            value["configuration"] = utf8(fs::absolute(launcher_config));
            if (args.get("--out"))
                write_json(args.path("--out"), value);
            else
                std::cout << value.dump(2) << '\n';
            return 0;
        }
        if (graphical) {
            log_path = root / "reports" /
                       path_from_utf8("game-" + timestamp() + "-" + std::to_string(GetCurrentProcessId()) +
                                      ".log");
            log_redirect.open(log_path);
        }
        if (args.command == "inspect") {
            std::vector<fs::path> apks;
            for (const auto &path : args.values["--apk"])
                apks.push_back(path_from_utf8(path));
            auto info = android::inspect_package_set(apks);
            if (auto compare = args.get("--compare")) {
                auto previous = android::open_installation(root, *compare);
                info["compatibility"] = android::compare_package_contracts(previous.metadata, info);
                info["comparison_installation"] = *compare;
            }
            if (auto out = args.get("--out")) {
                write_json(path_from_utf8(*out), info);
                std::cout << "Inspection written to " << *out << '\n';
            } else
                std::cout << info.dump(2) << '\n';
            return 0;
        }
        if (args.command == "list") {
            std::cout << android::installation_list(root).dump(2) << '\n';
            return 0;
        }
        if (args.command == "import") {
            std::vector<fs::path> apks;
            for (const auto &path : args.values["--apk"])
                apks.push_back(path_from_utf8(path));
            std::optional<fs::path> data;
            if (auto p = args.get("--data"))
                data = path_from_utf8(*p);
            auto installation = android::import_package(root, apks, args.path("--obb"), data);
            std::cout << "Imported " << installation.id() << "\nData: " << utf8(installation.data_directory)
                      << '\n';
            return 0;
        }
        require(args.command == "run", "Unknown command: " + args.command);
        auto limit = args.count("--frames");
        auto window_width = launch_settings.window_width, window_height = launch_settings.window_height;
        auto capture = args.get("--capture");
        auto capture_every = args.count("--capture-every");
        require(!args.get("--capture-every") || (capture && capture_every > 0),
                "--capture-every requires --capture and a positive interval");
        require(!capture || limit > 0, "--capture requires a positive --frames limit");
        require(!args.flags.contains("--live-input") || args.get("--input"),
                "--live-input requires --input FILE");
        if (auto input = args.get("--input"))
            replay.emplace(path_from_utf8(*input), args.flags.contains("--live-input"));
        auto installation = android::open_installation(
            root, launch_settings.version.empty() ? std::nullopt : std::optional(launch_settings.version));
        if (auto profile =
                launch_settings.profile.empty() ? std::nullopt : std::optional(launch_settings.profile)) {
            require(!profile->empty() && std::ranges::all_of(*profile,
                                                             [](unsigned char c) {
                                                                 return (c >= 'a' && c <= 'z') ||
                                                                        (c >= 'A' && c <= 'Z') ||
                                                                        (c >= '0' && c <= '9') ||
                                                                        c == '-' || c == '_';
                                                             }),
                    "Profile names may contain only letters, digits, hyphens and underscores");
            installation.data_directory = installation.root / "profiles" /
                                          path_from_utf8(installation.package()) / path_from_utf8(*profile);
        }
        game_options.render_width = launch_settings.native_resolution ? 0 : launch_settings.render_width;
        game_options.render_height = launch_settings.native_resolution ? 0 : launch_settings.render_height;
        game_options.fps_limit = launch_settings.fps_limit;
        adapter = games::pvz2(game_options);
        auto manifest = installation.metadata.at("manifest");
        android::Manifest identity;
        identity.package = manifest.at("package");
        identity.version_code = manifest.at("version_code");
        identity.version_name = manifest.at("version_name");
        require(adapter->accepts(identity), "No game adapter accepts this package");
        require(!installation.metadata.at("obb").is_null(),
                "This game requires its matching OBB; import the APK and OBB into a new --root");
        auto obb = installation.directory /
                   path_from_utf8(installation.metadata.at("obb").at("file").get<std::string>());
        require(fs::file_size(obb) == installation.metadata["obb"]["size"].get<std::uint64_t>(),
                "Installed OBB size mismatch");
        for (const auto &[name, lib] : installation.metadata["libraries"].items())
            require(sha256(installation.library(name)) == lib["sha256"].get<std::string>(),
                    "Installed library was modified: " + name);
        if (args.flags.contains("--verify"))
            require(sha256(obb) == installation.metadata["obb"]["sha256"].get<std::string>(),
                    "Installed OBB hash mismatch");
        if (!args.flags.contains("--probe"))
            android::backup_before_launch(installation);
        auto data = args.flags.contains("--probe")
                        ? root / "probes" / path_from_utf8(installation.id()) / path_from_utf8(timestamp())
                        : installation.data_directory;
        android::RuntimeOptions options{identity.package,
                                        identity.version_name,
                                        "en_US",
                                        identity.version_code,
                                        data,
                                        obb,
                                        installation.directory / "apks"};
        options.application_metadata = manifest.value("metadata", Json::object());
        options.target_sdk = manifest.value("target_sdk", 1u);
        options.performance_profile = args.flags.contains("--performance-profile");
        runtime = std::make_unique<android::Runtime>(std::move(options));
        auto &r = *runtime;
        report_path =
            args.path("--out", root / "reports" / path_from_utf8(installation.id() + "-last-run.json"));
        adapter->configure(r, installation);
        r.phase = "loading";
        r.linker.load(installation.library(adapter->main_library()));
        auto missing = r.linker.unresolved();
        r.message(std::format("Loaded {} ARM64 images; {} imports have no implementation",
                              r.linker.images().size(), missing.size()));
        r.unavailable_imports();
        r.linker.relocate();
        if (args.flags.contains("--probe")) {
            r.initialize_libraries();
            auto report = r.report();
            report["result"] = "native-libraries-initialized";
            write_json(report_path, report);
            std::cout << "ARM64 constructors and JNI initialization completed; report: "
                      << utf8(report_path) << '\n';
            return 0;
        }
        std::optional<fs::path> angle;
        if (!launch_settings.angle_directory.empty())
            angle = launch_settings.angle_directory;
        r.host->open_window("PVZ2 PC - experimental compatibility runtime", static_cast<int>(window_width),
                            static_cast<int>(window_height));
        r.host->window_mode(launch_settings.window_mode);
        r.graphics->open(r.host->render_window(), angle);
        r.graphics->vsync(launch_settings.vsync);
        r.host->audio_gain(launch_settings.muted ? 0.f : launch_settings.volume / 100.f);
        r.initialize_libraries();
        adapter->start(r);
        r.timings.record(FrameStage::Startup, std::chrono::steady_clock::now() - launch_start);
        bool captured = false;
        auto [drawable_width, drawable_height] = r.host->drawable_size();
        TimingSummary frame_times;
        bool quit = false, replay_quit = false;
        while (!quit && !adapter->quit_requested()) {
            r.check_threads();
            auto frame_start = std::chrono::steady_clock::now();
            auto events = r.host->poll();
            for (const auto &event : events) {
                if (event.type == host::EventType::Quit) {
                    quit = true;
                    break;
                }
                if (event.type == host::EventType::Resize && event.width > 0 && event.height > 0) {
                    drawable_width = event.width;
                    drawable_height = event.height;
                }
                adapter->event(r, event);
            }
            if (!quit && replay)
                for (const auto &event : replay->take(frames, drawable_width, drawable_height)) {
                    if (event.type == host::EventType::Quit) {
                        quit = true;
                        replay_quit = true;
                        break;
                    }
                    adapter->event(r, event);
                }
            if (quit)
                break;
            r.timings.record(FrameStage::Events, std::chrono::steady_clock::now() - frame_start);
            adapter->frame(r);
            r.timings.measure(FrameStage::Presentation, [&] { r.graphics->finish_frame(); });
            r.check_threads();
            if (limit && frames + 1 == limit)
                r.timings.measure(FrameStage::CallbackDrain, [&] { r.wait_callbacks(); });
            if (capture && (frames + 1 == limit || (capture_every && (frames + 1) % capture_every == 0))) {
                auto path = path_from_utf8(*capture);
                const bool final_frame = frames + 1 == limit;
                if (!final_frame)
                    path = path.parent_path() /
                           path_from_utf8(utf8(path.stem()) + std::format("-{:06}", frames + 1) +
                                          utf8(path.extension()));
                r.timings.measure(FrameStage::Capture, [&] {
                    write_png_rgba(path, drawable_width, drawable_height,
                                   r.graphics->read_rgba(drawable_width, drawable_height), true);
                });
                frame_captures.push_back({{"frame", frames + 1},
                                          {"path", utf8(path)},
                                          {"width", drawable_width},
                                          {"height", drawable_height}});
                captured |= final_frame;
            }
            r.timings.measure(FrameStage::Swap, [&] { r.graphics->present(); });
            ++frames;
            frame_times.add(std::chrono::steady_clock::now() - frame_start);
            if (limit && frames >= limit)
                break;
        }
        adapter->stop(r);
        r.phase = "stopped";
        auto report = r.report();
        report["result"] = "session-ended";
        report["frames"] = frames;
        report["stop_reason"] = replay_quit                 ? "input-replay"
                                : quit                      ? "window-closed"
                                : adapter->quit_requested() ? "application-exit"
                                                            : "frame-limit";
        report["data_directory"] = utf8(data);
        report["launcher_configuration"] = utf8(fs::absolute(launcher_config));
        report["launch_settings"] = launcher::describe(launch_settings);
        report["window"] = r.host->describe_window();
        if (!log_path.empty())
            report["log"] = utf8(log_path);
        report["game_options"] = {{"google_play_games", "disabled"}, {"purchases", "disabled"}};
        if (replay)
            report["input_replay"] = replay->report();
        report["frame_captures"] = frame_captures;
        if (frames)
            report["frame_time_ms"] = frame_times.report();
        if (captured)
            report["capture"] = *capture;
        write_json(report_path, report);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
        if (runtime && !report_path.empty()) {
            try {
                auto report = runtime->report();
                report["result"] = "failed";
                report["frames"] = frames;
                if (replay)
                    report["input_replay"] = replay->report();
                report["frame_captures"] = frame_captures;
                report["error"] = e.what();
                report["launch_settings"] = launcher::describe(launch_settings);
                report["game_options"] = {{"google_play_games", "disabled"}, {"purchases", "disabled"}};
                write_json(report_path, report);
                std::cerr << "Diagnostic report: " << utf8(report_path) << '\n';
            } catch (const std::exception &diagnostic) {
                std::cerr << "Report write failed: " << diagnostic.what() << '\n';
            }
        }
        if (graphical) {
            auto message = std::string(e.what());
            if (!log_path.empty())
                message += "\n\nLog: " + utf8(log_path);
            MessageBoxW(nullptr, widen(message).c_str(), L"PVZ2 PC", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}

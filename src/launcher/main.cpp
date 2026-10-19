#include <acp/pvz2.hpp>

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
        require(argc >= 2, "Usage: pvz2pc inspect|import|list|run [--apk FILE] [--obb FILE] [--root DIR] "
                           "[--out FILE] [--version ID] [--probe] [--frames N] [--capture PNG] "
                           "[--capture-every N] [--input FILE] [--live-input] [--profile NAME] [--config "
                           "INI] [--angle-dir DIR] [--width N] [--height N]");
        command = narrow(argv[1]);
        for (int i = 2; i < argc; ++i) {
            auto key = narrow(argv[i]);
            if (key == "--probe" || key == "--verify" || key == "--live-input") {
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
} // namespace
int wmain(int argc, wchar_t **argv) {
    using namespace acp;
    std::unique_ptr<android::GameAdapter> adapter;
    std::unique_ptr<android::Runtime> runtime;
    fs::path report_path;
    std::uint64_t frames = 0;
    Json frame_captures = Json::array();
    std::optional<ReplaySession> replay;
    fs::path game_config;
    games::Pvz2Options game_options;
    try {
        Arguments args(argc, argv);
        auto root = args.path("--root", host::default_data_root());
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
        auto window_width = args.get("--width") ? args.count("--width") : 1280;
        auto window_height = args.get("--height") ? args.count("--height") : 720;
        require(window_width >= 64 && window_width <= 8192 && window_height >= 64 && window_height <= 8192,
                "Window width and height must be between 64 and 8192");
        auto capture = args.get("--capture");
        auto capture_every = args.count("--capture-every");
        require(!args.get("--capture-every") || (capture && capture_every > 0),
                "--capture-every requires --capture and a positive interval");
        require(!capture || limit > 0, "--capture requires a positive --frames limit");
        require(!args.flags.contains("--live-input") || args.get("--input"),
                "--live-input requires --input FILE");
        if (auto input = args.get("--input"))
            replay.emplace(path_from_utf8(*input), args.flags.contains("--live-input"));
        auto installation = android::open_installation(root, args.get("--version"));
        if (auto profile = args.get("--profile")) {
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
        game_config = args.path("--config", root / "game.ini");
        game_options = games::read_pvz2_options(game_config, !args.flags.contains("--probe"));
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
        if (auto path = args.get("--angle-dir"))
            angle = path_from_utf8(*path);
        r.host->open_window("PVZ2 PC - experimental compatibility runtime", static_cast<int>(window_width),
                            static_cast<int>(window_height));
        r.graphics->open(r.host->native_window(), angle);
        r.initialize_libraries();
        adapter->start(r);
        bool captured = false;
        auto [drawable_width, drawable_height] = r.host->drawable_size();
        std::vector<double> frame_times;
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
            adapter->frame(r);
            r.graphics->finish_frame();
            r.check_threads();
            if (limit && frames + 1 == limit)
                r.wait_callbacks();
            if (capture && (frames + 1 == limit || (capture_every && (frames + 1) % capture_every == 0))) {
                auto path = path_from_utf8(*capture);
                const bool final_frame = frames + 1 == limit;
                if (!final_frame)
                    path = path.parent_path() /
                           path_from_utf8(utf8(path.stem()) + std::format("-{:06}", frames + 1) +
                                          utf8(path.extension()));
                write_png_rgba(path, drawable_width, drawable_height,
                               r.graphics->read_rgba(drawable_width, drawable_height), true);
                frame_captures.push_back({{"frame", frames + 1},
                                          {"path", utf8(path)},
                                          {"width", drawable_width},
                                          {"height", drawable_height}});
                captured |= final_frame;
            }
            r.graphics->present();
            ++frames;
            frame_times.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start)
                    .count());
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
        report["game_options"] = {{"config", utf8(fs::absolute(game_config))},
                                  {"google_play_games", "disabled"},
                                  {"purchases", game_options.purchases == games::PurchaseMode::AutoComplete
                                                    ? "auto_complete"
                                                    : "disabled"}};
        if (replay)
            report["input_replay"] = replay->report();
        report["frame_captures"] = frame_captures;
        if (!frame_times.empty()) {
            std::ranges::sort(frame_times);
            double total = 0;
            for (auto time : frame_times)
                total += time;
            auto percentile = [&](double fraction) {
                return frame_times[static_cast<size_t>((frame_times.size() - 1) * fraction)];
            };
            report["frame_time_ms"] = {{"mean", total / frames},
                                       {"p50", percentile(0.5)},
                                       {"p95", percentile(0.95)},
                                       {"p99", percentile(0.99)},
                                       {"max", frame_times.back()}};
        }
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
                report["game_options"] = {
                    {"config", utf8(fs::absolute(game_config))},
                    {"google_play_games", "disabled"},
                    {"purchases", game_options.purchases == games::PurchaseMode::AutoComplete
                                      ? "auto_complete"
                                      : "disabled"}};
                write_json(report_path, report);
                std::cerr << "Diagnostic report: " << utf8(report_path) << '\n';
            } catch (const std::exception &diagnostic) {
                std::cerr << "Report write failed: " << diagnostic.what() << '\n';
            }
        }
        return 1;
    }
}

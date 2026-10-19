#pragma once
#include <acp/host.hpp>

namespace acp::launcher {
struct Settings {
    fs::path executable, root, angle_directory, game_config;
    std::string profile, version;
    host::WindowMode window_mode = host::WindowMode::Windowed;
    int window_width = 1280, window_height = 720;
    bool native_resolution = true;
    int render_width = 1920, render_height = 1080;
    bool vsync = true, muted = false;
    int volume = 100, fps_limit = 0;
};
fs::path default_config_path();
Settings defaults();
void validate(const Settings &settings);
Settings load(const fs::path &file, bool create = false);
void save(const fs::path &file, const Settings &settings);
Json describe(const Settings &settings);
std::wstring quote_argument(std::wstring_view argument);
// Returned process handle belongs to the caller (CloseHandle).
void *start_game(const Settings &settings, const fs::path &configuration);
int run_game(int argc, wchar_t **argv, bool graphical = false);
} // namespace acp::launcher

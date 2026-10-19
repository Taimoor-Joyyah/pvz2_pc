#include <acp/launcher.hpp>

#include <Windows.h>

#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    auto result = acp::launcher::run_game(argc, argv, true);
    LocalFree(argv);
    return result;
}

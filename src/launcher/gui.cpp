#include <acp/launcher.hpp>

#include <Windows.h>

#include <charconv>
#include <cmath>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#pragma comment(                                                                                           \
    linker,                                                                                                \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using namespace acp;
using Microsoft::WRL::ComPtr;
enum Id {
    Title = 100,
    Subtitle,
    Executable,
    Root,
    Profile,
    Version,
    Angle,
    Mode,
    Width,
    Height,
    RenderMode,
    RenderWidth,
    RenderHeight,
    Vsync,
    Fps,
    Mute,
    Volume,
    ConfigPath,
    Status,
    BrowseExe,
    BrowseRoot,
    BrowseAngle,
    Folder,
    Save,
    Play,
    Close
};
std::wstring text(HWND control) {
    auto length = GetWindowTextLengthW(control);
    std::wstring value(size_t(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}
std::optional<fs::path> choose_path(HWND owner, bool folder, const fs::path &initial) {
    ComPtr<IFileOpenDialog> dialog;
    require(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog))),
            "Cannot open the file picker");
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC filter{L"Windows executable", L"*.exe"};
        dialog->SetFileTypes(1, &filter);
    }
    auto directory = folder ? initial : initial.parent_path();
    if (fs::is_directory(directory)) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&item))))
            dialog->SetFolder(item.Get());
    }
    auto result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    require(SUCCEEDED(result), "File picker failed");
    ComPtr<IShellItem> item;
    require(SUCCEEDED(dialog->GetResult(&item)), "No file was selected");
    PWSTR path{};
    require(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)), "Cannot read the selected path");
    fs::path value(path);
    CoTaskMemFree(path);
    return value;
}
class Launcher {
    struct Control {
        HWND window{};
        int x{}, y{}, width{}, height{}, font{};
        float move_x{}, grow_x{}, move_y{};
    };
    HWND window_{};
    HINSTANCE instance_{};
    fs::path configuration_;
    launcher::Settings settings_;
    std::map<int, HWND> controls_;
    std::vector<Control> layout_;
    std::vector<std::string> versions_;
    HFONT fonts_[3]{};
    HBRUSH background_ = CreateSolidBrush(RGB(246, 248, 250));
    HBRUSH banner_ = CreateSolidBrush(RGB(23, 52, 67));
    UINT dpi_ = 96;
    HANDLE process_{};
    bool loading_ = true;
    int scaled(int value) const {
        return MulDiv(value, dpi_, 96);
    }
    void status(std::wstring_view value) {
        SetWindowTextW(controls_.at(Status), std::wstring(value).c_str());
    }
    HWND add(int id, const wchar_t *kind, const wchar_t *caption, DWORD style, int x, int y, int width,
             int height, int font = 1, bool stretch = false, bool right = false, bool bottom = false) {
        auto control = CreateWindowExW(
            std::wstring_view(kind) == L"EDIT" ? WS_EX_CLIENTEDGE : 0, kind, caption,
            WS_CHILD | WS_VISIBLE | style, scaled(x), scaled(y), scaled(width), scaled(height), window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        require(control != nullptr, "Cannot create a launcher control");
        controls_[id] = control;
        layout_.push_back(
            {control, x, y, width, height, font, float(right), float(stretch), float(bottom)});
        return control;
    }
    void label(const wchar_t *caption, int x, int y, int width = 150, float move_x = 0) {
        add(1000 + int(layout_.size()), L"STATIC", caption, SS_LEFT, x, y + 4, width, 22);
        layout_.back().move_x = move_x;
    }
    void edit(int id, int x, int y, int width, bool numeric = false, bool stretch = false,
              bool right = false) {
        add(id, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0), x, y, width, 26, 1,
            stretch, right);
    }
    void combo(int id, int x, int y, int width, std::initializer_list<const wchar_t *> values,
               bool editable = false, float move_x = 0, float grow_x = 0) {
        auto control =
            add(id, L"COMBOBOX", L"",
                WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST), x, y, width, 230);
        layout_.back().move_x = move_x;
        layout_.back().grow_x = grow_x;
        for (auto value : values)
            SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    }
    void set_fonts() {
        HFONT next[3]{};
        int sizes[]{23, 10, 9};
        for (int i = 0; i < 3; ++i)
            next[i] = CreateFontW(-MulDiv(sizes[i], dpi_, 72), 0, 0, 0, i == 0 ? FW_SEMIBOLD : FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (const auto &control : layout_)
            SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(next[control.font]), TRUE);
        for (auto font : fonts_)
            if (font)
                DeleteObject(font);
        std::copy(std::begin(next), std::end(next), std::begin(fonts_));
    }
    void arrange() {
        RECT client{};
        GetClientRect(window_, &client);
        auto dx = client.right - scaled(860), dy = client.bottom - scaled(642);
        auto place = [&](const Control &control, HDWP batch) {
            auto x = scaled(control.x) + int(std::lround(dx * control.move_x));
            auto y = scaled(control.y) + int(std::lround(dy * control.move_y));
            auto width = std::max(1, scaled(control.width) + int(std::lround(dx * control.grow_x)));
            auto height = scaled(control.height);
            if (batch)
                return DeferWindowPos(batch, control.window, nullptr, x, y, width, height,
                                      SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(control.window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            return HDWP{};
        };
        auto batch = BeginDeferWindowPos(static_cast<int>(layout_.size()));
        if (batch)
            for (const auto &control : layout_)
                if (!(batch = place(control, batch)))
                    break;
        if (!batch || !EndDeferWindowPos(batch))
            for (const auto &control : layout_)
                place(control, nullptr);
        // Moved child controls need repainting too, including group boxes and labels.
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    fs::path field_path(int id) const {
        auto value = text(controls_.at(id));
        if (value.empty())
            return {};
        fs::path path(value);
        return fs::absolute(path.is_absolute() ? path : configuration_.parent_path() / path)
            .lexically_normal();
    }
    int integer(int id, const char *name) const {
        auto value = narrow(text(controls_.at(id)));
        int result{};
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        require(error == std::errc{} && end == value.data() + value.size(),
                std::string("Enter a number for ") + name);
        return result;
    }
    void set_text(int id, const std::wstring &value) {
        SetWindowTextW(controls_.at(id), value.c_str());
    }
    void resolution_controls() {
        auto custom = SendMessageW(controls_.at(RenderMode), CB_GETCURSEL, 0, 0) == 1;
        EnableWindow(controls_.at(RenderWidth), custom);
        EnableWindow(controls_.at(RenderHeight), custom);
    }
    void installed_choices() {
        auto previous_version = settings_.version;
        auto selected = SendMessageW(controls_.at(Version), CB_GETCURSEL, 0, 0);
        if (selected >= 0 && size_t(selected) < versions_.size())
            previous_version = versions_[selected];
        auto profile = text(controls_.at(Profile));
        SendMessageW(controls_.at(Version), CB_RESETCONTENT, 0, 0);
        SendMessageW(controls_.at(Profile), CB_RESETCONTENT, 0, 0);
        SendMessageW(controls_.at(Version), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Active installation"));
        versions_ = {""};
        int selection = 0;
        auto root = field_path(Root);
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(root / "installations", error)) {
            if (!entry.is_directory())
                continue;
            auto id = utf8(entry.path().filename());
            auto caption = widen(id);
            try {
                auto metadata = read_json(entry.path() / "installation.json");
                caption = widen(metadata.at("manifest").value("version_name", id) + "  (" + id + ")");
            } catch (const std::exception &) {
            }
            SendMessageW(controls_.at(Version), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(caption.c_str()));
            if (id == previous_version)
                selection = int(versions_.size());
            versions_.push_back(id);
        }
        if (!previous_version.empty() && selection == 0) {
            auto caption = widen(previous_version + " (not found)");
            SendMessageW(controls_.at(Version), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(caption.c_str()));
            selection = int(versions_.size());
            versions_.push_back(previous_version);
        }
        SendMessageW(controls_.at(Version), CB_SETCURSEL, selection, 0);
        std::string package = "com.ea.game.pvz2_row";
        try {
            auto active = read_json(root / "active.json").at("id").get<std::string>();
            package = read_json(root / "installations" / path_from_utf8(active) / "installation.json")
                          .at("manifest")
                          .at("package")
                          .get<std::string>();
        } catch (const std::exception &) {
        }
        error.clear();
        for (const auto &entry : fs::directory_iterator(root / "profiles" / path_from_utf8(package), error))
            if (entry.is_directory()) {
                auto name = entry.path().filename().native();
                SendMessageW(controls_.at(Profile), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(name.c_str()));
            }
        set_text(Profile, profile);
    }
    void build() {
        dpi_ = GetDpiForWindow(window_);
        add(Title, L"STATIC", L"PVZ2 PC", SS_LEFT, 24, 14, 600, 36, 0, true);
        add(Subtitle, L"STATIC", L"Choose your installation and display settings, then play.", SS_LEFT, 25,
            52, 790, 22, 2, true);
        label(L"Game executable", 24, 96);
        edit(Executable, 180, 92, 560, false, true);
        add(BrowseExe, L"BUTTON", L"Browse...", WS_TABSTOP, 750, 92, 86, 27, 1, false, true);
        label(L"Installation directory", 24, 134);
        edit(Root, 180, 130, 560, false, true);
        add(BrowseRoot, L"BUTTON", L"Browse...", WS_TABSTOP, 750, 130, 86, 27, 1, false, true);
        label(L"Profile (blank = default)", 24, 172);
        combo(Profile, 180, 168, 210, {}, true, 0, 0.5f);
        label(L"Game version", 420, 172, 100, 0.5f);
        combo(Version, 530, 168, 306, {}, false, 0.5f, 0.5f);
        label(L"ANGLE directory (optional)", 24, 210);
        edit(Angle, 180, 206, 560, false, true);
        add(BrowseAngle, L"BUTTON", L"Browse...", WS_TABSTOP, 750, 206, 86, 27, 1, false, true);
        add(1100, L"BUTTON", L"Display", BS_GROUPBOX, 24, 248, 812, 166, 1, true);
        label(L"Window mode", 40, 277, 130);
        combo(Mode, 180, 273, 210, {L"Windowed", L"Maximized", L"Fullscreen"}, false, 0, 1);
        label(L"Client size", 420, 277, 100, 1);
        edit(Width, 530, 273, 85, true, false, true);
        label(L"x", 626, 277, 15, 1);
        edit(Height, 650, 273, 85, true, false, true);
        label(L"Rendering", 40, 316, 130);
        combo(RenderMode, 180, 312, 210, {L"Client resolution at launch", L"Custom resolution"}, false, 0,
              1);
        label(L"Render size", 420, 316, 100, 1);
        edit(RenderWidth, 530, 312, 85, true, false, true);
        label(L"x", 626, 316, 15, 1);
        edit(RenderHeight, 650, 312, 85, true, false, true);
        add(1101, L"STATIC",
            L"Resolution is prepared before launch. Relaunch after changing render quality.", SS_LEFT, 180,
            347, 630, 22, 2, true);
        add(Vsync, L"BUTTON", L"Vertical sync", WS_TABSTOP | BS_AUTOCHECKBOX, 180, 376, 210, 24, 1, true);
        label(L"Frame limit", 420, 376, 100, 1);
        edit(Fps, 530, 372, 85, true, false, true);
        add(1102, L"STATIC", L"0 = game controlled", SS_LEFT, 628, 379, 180, 20, 2, false, true);
        add(1103, L"BUTTON", L"Audio", BS_GROUPBOX, 24, 426, 812, 65, 1, true);
        add(Mute, L"BUTTON", L"Mute", WS_TABSTOP | BS_AUTOCHECKBOX, 42, 450, 130, 24);
        label(L"Volume", 225, 450, 65);
        edit(Volume, 300, 446, 85, true);
        label(L"0 - 100", 400, 450, 80);
        add(ConfigPath, L"STATIC", L"", SS_PATHELLIPSIS, 24, 509, 812, 22, 2, true, false, true);
        add(Status, L"STATIC", L"Ready. Saved settings also apply when opening the game directly.", SS_LEFT,
            24, 541, 812, 24, 1, true, false, true);
        add(Folder, L"BUTTON", L"Open game folder", WS_TABSTOP, 24, 586, 160, 34, 1, false, false, true);
        add(Close, L"BUTTON", L"Close", WS_TABSTOP, 520, 586, 90, 34, 1, false, true, true);
        add(Save, L"BUTTON", L"Save settings", WS_TABSTOP, 620, 586, 100, 34, 1, false, true, true);
        add(Play, L"BUTTON", L"Save && Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 730, 586, 106, 34, 1, false,
            true, true);
        set_text(Executable, settings_.executable.native());
        set_text(Root, settings_.root.native());
        set_text(Profile, widen(settings_.profile));
        set_text(Angle, settings_.angle_directory.native());
        set_text(Width, std::to_wstring(settings_.window_width));
        set_text(Height, std::to_wstring(settings_.window_height));
        set_text(RenderWidth, std::to_wstring(settings_.render_width));
        set_text(RenderHeight, std::to_wstring(settings_.render_height));
        set_text(Fps, std::to_wstring(settings_.fps_limit));
        set_text(Volume, std::to_wstring(settings_.volume));
        set_text(ConfigPath, L"Settings: " + configuration_.native());
        SendMessageW(controls_.at(Mode), CB_SETCURSEL, static_cast<int>(settings_.window_mode), 0);
        SendMessageW(controls_.at(RenderMode), CB_SETCURSEL, settings_.native_resolution ? 0 : 1, 0);
        SendMessageW(controls_.at(Vsync), BM_SETCHECK, settings_.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(controls_.at(Mute), BM_SETCHECK, settings_.muted ? BST_CHECKED : BST_UNCHECKED, 0);
        installed_choices();
        resolution_controls();
        set_fonts();
        loading_ = false;
        SetTimer(window_, 1, 500, nullptr);
        arrange();
    }
    launcher::Settings collect() const {
        auto value = settings_;
        value.executable = field_path(Executable);
        value.root = field_path(Root);
        value.angle_directory = field_path(Angle);
        value.profile = narrow(text(controls_.at(Profile)));
        auto version = SendMessageW(controls_.at(Version), CB_GETCURSEL, 0, 0);
        value.version = version >= 0 && size_t(version) < versions_.size() ? versions_[version] : "";
        value.window_mode =
            static_cast<host::WindowMode>(SendMessageW(controls_.at(Mode), CB_GETCURSEL, 0, 0));
        value.native_resolution = SendMessageW(controls_.at(RenderMode), CB_GETCURSEL, 0, 0) == 0;
        value.window_width = integer(Width, "window width");
        value.window_height = integer(Height, "window height");
        value.render_width = integer(RenderWidth, "render width");
        value.render_height = integer(RenderHeight, "render height");
        value.fps_limit = integer(Fps, "frame limit");
        value.volume = integer(Volume, "volume");
        value.vsync = SendMessageW(controls_.at(Vsync), BM_GETCHECK, 0, 0) == BST_CHECKED;
        value.muted = SendMessageW(controls_.at(Mute), BM_GETCHECK, 0, 0) == BST_CHECKED;
        launcher::validate(value);
        return value;
    }
    void command(int id, int notification) {
        if (loading_)
            return;
        if (id == RenderMode && notification == CBN_SELCHANGE) {
            resolution_controls();
            return;
        }
        if (id == Root && notification == EN_KILLFOCUS) {
            installed_choices();
            return;
        }
        if (notification != BN_CLICKED)
            return;
        if (id >= BrowseExe && id <= BrowseAngle) {
            auto target = id == BrowseExe ? Executable : id == BrowseRoot ? Root : Angle;
            auto selected = choose_path(window_, id == BrowseRoot || id == BrowseAngle, field_path(target));
            if (selected) {
                set_text(target, selected->native());
                if (id == BrowseRoot)
                    installed_choices();
            }
        } else if (id == Save || id == Play) {
            settings_ = collect();
            launcher::save(configuration_, settings_);
            status(L"Settings saved. Changes apply the next time the game starts.");
            if (id == Play) {
                require(!process_, "The game is already running");
                require(fs::is_regular_file(settings_.root / "active.json") || !settings_.version.empty(),
                        "No installation was found in this directory. Select the imported runtime root.");
                process_ = static_cast<HANDLE>(launcher::start_game(settings_, configuration_));
                EnableWindow(controls_.at(Play), FALSE);
                status(L"Game is running. New settings will apply to the next launch.");
                ShowWindow(window_, SW_MINIMIZE);
            }
        } else if (id == Folder) {
            auto directory = field_path(Executable).parent_path();
            require(fs::is_directory(directory), "The game executable directory does not exist");
            ShellExecuteW(window_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == Close)
            DestroyWindow(window_);
    }
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            build();
            return 0;
        case WM_SIZE:
            if (!loading_ && wparam != SIZE_MINIMIZED)
                arrange();
            return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO *>(lparam);
            RECT minimum{0, 0, scaled(860), scaled(642)};
            AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
            info->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            auto rectangle = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window_, nullptr, rectangle->left, rectangle->top,
                         rectangle->right - rectangle->left, rectangle->bottom - rectangle->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            set_fonts();
            arrange();
            return 0;
        }
        case WM_COMMAND:
            command(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_TIMER:
            if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                DWORD exit{};
                GetExitCodeProcess(process_, &exit);
                CloseHandle(process_);
                process_ = nullptr;
                EnableWindow(controls_.at(Play), TRUE);
                status(exit ? L"Game stopped with an error. See the runtime reports directory."
                            : L"Game closed. Ready to play again.");
            }
            return 0;
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wparam);
            auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
            auto header = id == Title || id == Subtitle;
            SetTextColor(dc, header ? RGB(255, 255, 255) : RGB(32, 45, 53));
            SetBkColor(dc, header ? RGB(23, 52, 67) : RGB(246, 248, 250));
            return reinterpret_cast<LRESULT>(header ? banner_ : background_);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window_, &paint);
            RECT area{};
            GetClientRect(window_, &area);
            FillRect(dc, &area, background_);
            area.bottom = scaled(82);
            FillRect(dc, &area, banner_);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto self = reinterpret_cast<Launcher *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Launcher *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        try {
            return self->handle(message, wparam, lparam);
        } catch (const std::exception &error) {
            MessageBoxW(window, widen(error.what()).c_str(), L"PVZ2 PC Launcher", MB_OK | MB_ICONERROR);
            return message == WM_CREATE ? -1 : 0;
        }
    }

  public:
    Launcher(HINSTANCE instance, fs::path configuration)
        : instance_(instance), configuration_(fs::absolute(configuration)),
          settings_(launcher::load(configuration_)) {}
    ~Launcher() {
        if (process_)
            CloseHandle(process_);
        for (auto font : fonts_)
            if (font)
                DeleteObject(font);
        DeleteObject(background_);
        DeleteObject(banner_);
    }
    int run(int show) {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = instance_;
        type.lpszClassName = L"ACP_Launcher";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        require(RegisterClassW(&type) != 0, "Cannot register launcher window");
        dpi_ = GetDpiForSystem();
        RECT size{0, 0, scaled(860), scaled(642)};
        AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        auto window = CreateWindowExW(0, type.lpszClassName, L"PVZ2 PC Launcher",
                                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                      size.right - size.left, size.bottom - size.top, nullptr, nullptr,
                                      instance_, this);
        require(window != nullptr, "Cannot create launcher window");
        ShowWindow(window, show);
        UpdateWindow(window);
        MSG message{};
        while (auto result = GetMessageW(&message, nullptr, 0, 0)) {
            require(result != -1, "Launcher message loop failed");
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return int(message.wParam);
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DisableProcessWindowsGhosting();
    auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int result = 1;
    try {
        auto configuration = acp::launcher::default_config_path();
        int argc{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                acp::require(std::wstring_view(argv[i]) == L"--launcher-config" && i + 1 < argc,
                             "Usage: pvz2pc-launcher [--launcher-config FILE]");
                configuration = argv[++i];
            }
            LocalFree(argv);
        }
        Launcher launcher(instance, configuration);
        result = launcher.run(show);
    } catch (const std::exception &error) {
        MessageBoxW(nullptr, acp::widen(error.what()).c_str(), L"PVZ2 PC Launcher", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    return result;
}

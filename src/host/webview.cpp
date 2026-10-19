#include <acp/host.hpp>

#include <Windows.h>

#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>

namespace acp::host {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
void checked(HRESULT result, std::string_view operation) {
    require(SUCCEEDED(result), std::format("{} failed ({:#x})", operation, static_cast<unsigned>(result)));
}
std::string take_string(LPWSTR value) {
    auto result = value ? narrow(value) : std::string{};
    CoTaskMemFree(value);
    return result;
}
std::string decode(std::string_view input) {
    std::string result;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size() && hex(input[i + 1]) >= 0 && hex(input[i + 2]) >= 0) {
            result += static_cast<char>(hex(input[i + 1]) * 16 + hex(input[i + 2]));
            i += 2;
        } else
            result += input[i] == '+' ? ' ' : input[i];
    }
    return result;
}
class WindowsWebView final : public WebView, public std::enable_shared_from_this<WindowsWebView> {
  public:
    explicit WindowsWebView(WebViewRequest request) : request_(std::move(request)) {}
    ~WindowsWebView() override {
        close();
        if (com_initialized_)
            CoUninitialize();
    }
    bool closed() const override {
        return closed_;
    }
    void close() override {
        closed_ = true;
        if (controller_)
            controller_->Close();
        webview_.Reset();
        controller_.Reset();
        environment_.Reset();
        if (window_)
            DestroyWindow(window_);
        window_ = nullptr;
    }
    void open(HWND owner) {
        checked(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "WebView COM initialization");
        com_initialized_ = true;
        WNDCLASSW type{};
        type.lpfnWndProc = window_proc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"AndroidPcRuntimeWebView";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        require(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                "Cannot register web view window");
        auto title = widen(request_.title);
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, type.lpszClassName, title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760,
                                  owner, nullptr, type.hInstance, this);
        require(window_ != nullptr, "Cannot create web view window");
        fs::create_directories(request_.data_directory);
        auto self = shared_from_this();
        checked(
            CreateCoreWebView2EnvironmentWithOptions(
                nullptr, request_.data_directory.c_str(), nullptr,
                Callback<
                    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([self](
                                                                                    HRESULT result,
                                                                                    ICoreWebView2Environment
                                                                                        *environment)
                                                                                    -> HRESULT {
                    if (self->closed_)
                        return S_OK;
                    try {
                        checked(
                            result,
                            "WebView2 runtime initialization (install Microsoft Edge WebView2 Runtime)");
                        require(environment != nullptr, "WebView2 returned no environment");
                        self->environment_ = environment;
                        checked(
                            environment->CreateCoreWebView2Controller(
                                self->window_,
                                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                    [self](HRESULT status, ICoreWebView2Controller *controller) -> HRESULT {
                                        if (self->closed_)
                                            return S_OK;
                                        try {
                                            checked(status, "WebView2 controller creation");
                                            require(controller != nullptr,
                                                    "WebView2 returned no controller");
                                            self->controller_ = controller;
                                            checked(controller->get_CoreWebView2(&self->webview_),
                                                    "WebView2 content interface");
                                            self->initialize();
                                        } catch (const std::exception &error) {
                                            self->fail(error.what());
                                        }
                                        return S_OK;
                                    })
                                    .Get()),
                            "WebView2 controller request");
                    } catch (const std::exception &error) {
                        self->fail(error.what());
                    }
                    return S_OK;
                }).Get()),
            "WebView2 environment request");
    }

  private:
    WebViewRequest request_;
    HWND window_{};
    bool closed_{}, closing_{}, com_initialized_{};
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    void fail(std::string error) {
        if (closed_ || closing_)
            return;
        closing_ = true;
        try {
            if (request_.failure)
                request_.failure(std::move(error));
        } catch (...) {
            // Client failures must not cross the Win32 or COM callback boundary.
        }
        if (window_)
            PostMessageW(window_, WM_APP + 1, 0, 0);
        else
            close();
    }
    void message(const Json &value) {
        if (!closed_ && !closing_ && request_.message && request_.message(value)) {
            closing_ = true;
            PostMessageW(window_, WM_APP + 1, 0, 0);
        }
    }
    void navigate(ICoreWebView2NavigationStartingEventArgs *args) {
        LPWSTR raw{};
        checked(args->get_Uri(&raw), "WebView navigation URI");
        auto uri = take_string(raw);
        auto prefix = request_.callback_scheme + "://";
        if (!request_.callback_scheme.empty() && uri.starts_with(prefix)) {
            args->put_Cancel(TRUE);
            Json values = Json::object();
            auto start = uri.find('?');
            if (start != uri.npos) {
                for (auto query = std::string_view(uri).substr(start + 1); !query.empty();) {
                    auto end = query.find('&');
                    auto item = query.substr(0, end);
                    auto equal = item.find('=');
                    if (equal != item.npos)
                        values[decode(item.substr(0, equal))] = decode(item.substr(equal + 1));
                    if (end == query.npos)
                        break;
                    query.remove_prefix(end + 1);
                }
            }
            message(values);
        } else if (uri != "about:blank" && !uri.starts_with("data:"))
            args->put_Cancel(TRUE);
    }
    void initialize() {
        ComPtr<ICoreWebView2Settings> settings;
        checked(webview_->get_Settings(&settings), "WebView settings");
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        RECT bounds{};
        GetClientRect(window_, &bounds);
        checked(controller_->put_Bounds(bounds), "WebView bounds");
        auto weak = weak_from_this();
        EventRegistrationToken token{};
        auto navigation = Callback<ICoreWebView2NavigationStartingEventHandler>(
            [weak](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                if (auto self = weak.lock()) {
                    try {
                        self->navigate(args);
                    } catch (const std::exception &error) {
                        self->fail(error.what());
                    }
                }
                return S_OK;
            });
        checked(webview_->add_NavigationStarting(navigation.Get(), &token), "WebView navigation handler");
        checked(webview_->add_FrameNavigationStarting(navigation.Get(), &token), "WebView frame handler");
        checked(webview_->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [weak](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                            if (auto self = weak.lock()) {
                                try {
                                    LPWSTR raw{};
                                    checked(args->get_WebMessageAsJson(&raw), "WebView message");
                                    self->message(Json::parse(take_string(raw)));
                                } catch (const std::exception &error) {
                                    self->fail(error.what());
                                }
                            }
                            return S_OK;
                        })
                        .Get(),
                    &token),
                "WebView message handler");
        checked(
            webview_->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [weak](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                        if (auto self = weak.lock()) {
                            try {
                                BOOL success{};
                                checked(args->get_IsSuccess(&success), "WebView navigation status");
                                require(success != FALSE, "WebView HTML load failed");
                                if (!self->request_.page_script.empty()) {
                                    auto script = widen(self->request_.page_script);
                                    checked(self->webview_->ExecuteScript(script.c_str(), nullptr),
                                            "WebView page initialization");
                                }
                                if (!self->request_.preview.empty())
                                    SetTimer(self->window_, 1, 500, nullptr);
                            } catch (const std::exception &error) {
                                self->fail(error.what());
                            }
                        }
                        return S_OK;
                    })
                    .Get(),
                &token),
            "WebView load handler");
        // Embedded SDK documents stay offline; a page's explicit browser action is handled by its adapter.
        checked(webview_->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL),
                "WebView request filter");
        checked(
            webview_->add_WebResourceRequested(
                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                    [weak](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
                        if (auto self = weak.lock()) {
                            ComPtr<ICoreWebView2WebResourceRequest> request;
                            LPWSTR raw{};
                            if (SUCCEEDED(args->get_Request(&request)) &&
                                SUCCEEDED(request->get_Uri(&raw))) {
                                auto uri = take_string(raw);
                                if (uri.starts_with("http://") || uri.starts_with("https://")) {
                                    ComPtr<ICoreWebView2WebResourceResponse> response;
                                    if (SUCCEEDED(self->environment_->CreateWebResourceResponse(
                                            nullptr, 503, L"Offline", L"Content-Type: text/plain",
                                            &response)))
                                        args->put_Response(response.Get());
                                }
                            }
                        }
                        return S_OK;
                    })
                    .Get(),
                &token),
            "WebView offline handler");
        auto html = widen(request_.html);
        checked(webview_->NavigateToString(html.c_str()), "WebView HTML navigation");
    }
    void capture() {
        if (!webview_ || request_.preview.empty())
            return;
        fs::create_directories(request_.preview.parent_path());
        ComPtr<IStream> stream;
        checked(SHCreateStreamOnFileEx(request_.preview.c_str(),
                                       STGM_CREATE | STGM_READWRITE | STGM_SHARE_DENY_WRITE,
                                       FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream),
                "WebView preview output");
        auto weak = weak_from_this();
        checked(webview_->CapturePreview(
                    COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                    Callback<ICoreWebView2CapturePreviewCompletedHandler>([weak, stream](
                                                                              HRESULT status) -> HRESULT {
                        if (auto self = weak.lock()) {
                            try {
                                checked(status, "WebView preview capture");
                                stream->Commit(STGC_DEFAULT);
                                self->message(
                                    {{"type", "host-preview"}, {"path", utf8(self->request_.preview)}});
                            } catch (const std::exception &error) {
                                self->fail(error.what());
                            }
                        }
                        return S_OK;
                    }).Get()),
                "WebView preview request");
    }
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<WindowsWebView *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<WindowsWebView *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        if (self) {
            if (message == WM_APP + 1) {
                self->close();
                return 0;
            }
            if (message == WM_CLOSE) {
                self->fail("Web view closed by user");
                return 0;
            }
            if (message == WM_SIZE && self->controller_) {
                RECT bounds{};
                GetClientRect(window, &bounds);
                self->controller_->put_Bounds(bounds);
            }
            if (message == WM_TIMER && wparam == 1) {
                KillTimer(window, 1);
                try {
                    self->capture();
                } catch (const std::exception &error) {
                    self->fail(error.what());
                }
                return 0;
            }
            if (message == WM_NCDESTROY)
                self->window_ = nullptr;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};
} // namespace
std::shared_ptr<WebView> open_webview(void *owner, WebViewRequest request) {
    auto view = std::make_shared<WindowsWebView>(std::move(request));
    try {
        view->open(static_cast<HWND>(owner));
    } catch (...) {
        view->close();
        throw;
    }
    return view;
}
} // namespace acp::host

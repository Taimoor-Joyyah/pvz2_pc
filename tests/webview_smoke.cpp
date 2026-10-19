#include <acp/host.hpp>

#include <iostream>
#include <thread>
int main() {
    using namespace acp;
    try {
        auto host = host::windows();
        host->open_window("Compatibility web view validation", 640, 480);
        auto root = fs::current_path() / "out/webview-smoke";
        bool loaded{}, captured{}, scheme_received{};
        std::string failure;
        host::WebViewRequest request;
        request.title = "Web view validation";
        request.data_directory = root / "profile";
        request.preview = root / "webview.png";
        request.html =
            R"(<html><body style="background:#183b39;color:white;font:32px sans-serif;padding:40px">Android compatibility web view<canvas width="240" height="160" id="paint"></canvas><script>let c=document.querySelector('canvas').getContext('2d');c.fillStyle='#efad3b';c.fillRect(20,20,200,120);</script></body></html>)";
        request.callback_scheme = "fixture";
        request.page_script = "chrome.webview.postMessage({type:'ready',answer:42});let "
                              "f=document.createElement('iframe');f.src='fixture://bridge/"
                              "?action=loaded&value=hello%20world%2B';document.body.appendChild(f);";
        request.message = [&](const Json &message) {
            if (message.value("type", "") == "ready")
                loaded = message.at("answer") == 42;
            if (message.value("type", "") == "host-preview")
                captured = true;
            if (message.value("action", "") == "loaded")
                scheme_received = message.at("value") == "hello world+";
            return loaded && captured && scheme_received;
        };
        request.failure = [&](std::string error) { failure = std::move(error); };
        host->show_webview(std::move(request));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!captured && failure.empty() && std::chrono::steady_clock::now() < deadline) {
            host->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(failure.empty(), failure);
        require(loaded && captured && scheme_received && fs::file_size(root / "webview.png") > 1000,
                "Web view failed HTML, script, message or preview validation");
        write_json(root / "report.json", {{"result", "passed"},
                                          {"html", loaded},
                                          {"preview", captured},
                                          {"callback_scheme", scheme_received}});
        host->close_webviews();
        std::cout << "WebView2 HTML, JavaScript, message and PNG preview checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

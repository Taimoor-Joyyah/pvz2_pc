#include <acp/android.hpp>

#include <iostream>
int wmain(int argc, wchar_t **argv) {
    using namespace acp;
    std::unique_ptr<android::Runtime> runtime;
    fs::path report_path;
    try {
        require(argc == 2, "Usage: native_sample native-test.apk");
        fs::path apk(argv[1]);
        if (!fs::is_regular_file(apk)) {
            std::cout << "Build the C++23 Android fixture with tools/build-android-fixture.ps1 first.\n";
            return 77;
        }
        const auto info = android::inspect_package(apk);
        require(info.manifest.package == "org.acp.nativetest", "Unexpected test package");
        auto root = fs::current_path() / "test-artifacts" / path_from_utf8("native-app-" + timestamp());
        report_path = root / "report.json";
        auto installation = android::import_package(root, {apk}, {});
        auto source = root / "downloaded-content";
        auto downloaded_file = source / "files/content.bin";
        write_bytes(downloaded_file, std::as_bytes(std::span("content", 7)));
        auto reimported = android::import_package(root, {apk}, {}, source);
        auto target = installation.data_directory / "external/Android/data" /
                      path_from_utf8(info.manifest.package) / "files/content.bin";
        require(reimported.id() == installation.id() && read_bytes(target) == read_bytes(downloaded_file),
                "Existing installations must accept matching downloaded data in external app storage");
        android::import_package(root, {apk}, {}, source);
        write_bytes(downloaded_file, std::as_bytes(std::span("changed", 7)));
        bool conflict = false;
        try {
            android::import_package(root, {apk}, {}, source);
        } catch (const Error &) {
            conflict = true;
        }
        require(conflict && read_bytes(target) != read_bytes(downloaded_file),
                "Import must reject conflicting downloaded content without replacing existing data");
        runtime =
            std::make_unique<android::Runtime>(android::RuntimeOptions{info.manifest.package,
                                                                       info.manifest.version_name,
                                                                       "en_US",
                                                                       info.manifest.version_code,
                                                                       installation.data_directory,
                                                                       {},
                                                                       installation.directory / "apks"});
        auto &r = *runtime;
        r.linker.load(installation.library("libacp_native_test.so"));
        r.unavailable_imports();
        r.linker.relocate();
        r.java_libraries = {"libacp_native_test.so"};
        r.initialize_libraries();
        const auto entry = r.linker.symbol("acp_native_test");
        const auto save = r.memory.string("/data/data/org.acp.nativetest/files/native-test-save.txt");
        for (int run = 0; run < 2; ++run) {
            const auto result = r.jni->call_native(r.cpu, entry, std::array{r.jni->environment(), save});
            require(result == 0, std::format("Android fixture failed check {} on run {}", result, run));
        }
        r.post_callback([&r, entry, save](guest::Cpu &cpu) {
            const auto result = r.jni->call_native(cpu, entry, std::array{r.jni->environment(cpu), save});
            require(result == 0,
                    std::format("Android fixture failed check {} on the Java callback worker", result));
        });
        r.wait_callbacks();
        require(fs::is_regular_file(installation.data_directory / "files/native-test-save.txt"),
                "Guest file writes did not reach isolated persistent storage");
        auto report = r.report();
        report["result"] = "passed";
        report["apk_sha256"] = info.digest;
        report["checks"] = {"constructors",
                            "C++23 expected",
                            "guest C++ exceptions",
                            "JNI strings and arrays",
                            "JNI references and frames",
                            "guest qsort callbacks",
                            "TLS and pthread keys",
                            "mutexes and conditions",
                            "JNI thread attachment",
                            "persistent file round trip",
                            "APK import and downloaded-data collision handling",
                            "Java worker callbacks"};
        report["checks"].push_back("NDK OpenSL interfaces and PCM structure layouts");
        write_json(report_path, report);
        std::cout << "Independent Android C++23 native test passed on the main and callback threads; "
                  << utf8(report_path) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (runtime && !report_path.empty()) {
            auto report = runtime->report();
            report["result"] = "failed";
            report["error"] = e.what();
            write_json(report_path, report);
            std::cerr << "Report: " << utf8(report_path) << '\n';
        }
        return 1;
    }
}

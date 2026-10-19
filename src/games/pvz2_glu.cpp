#include <acp/pvz2.hpp>

#include <random>
namespace acp::games {
namespace {
constexpr auto platform = "csdk/glucentralservices/util/AndroidPlatform";
struct Properties {
    android::Runtime &runtime;
    std::map<std::string, Json> records;
    std::vector<Address> lifecycle_receivers;
    bool foreground = true;
    fs::path path(const std::string &name) {
        return runtime.options.data / "preferences/pc-runtime-glu" /
               (sha256(std::as_bytes(std::span(name))) + ".json");
    }
    Json &get(const std::string &name) {
        auto [it, fresh] = records.try_emplace(name, Json::object());
        if (fresh && fs::is_regular_file(path(name)))
            it->second = read_json(path(name));
        require(it->second.is_object(), "Invalid Glu properties record");
        return it->second;
    }
    void save(const std::string &name) {
        write_json(path(name), get(name));
    }
};
} // namespace
std::function<void(bool)> install_pvz2_glu(android::Runtime &r) {
    auto &j = *r.jni;
    auto properties = std::make_shared<Properties>(r);
    auto strings = std::make_shared<std::map<std::string, std::optional<std::string>>>();
    auto hook = [&j, &r](const char *name, const char *signature, android::JavaHandler handler) {
        j.hook(platform, name, signature, [&r, handler = std::move(handler)](android::JavaCall &call) {
            std::lock_guard lock(r.state_mutex);
            return handler(call);
        });
    };
    hook("<init>", "(Landroid/content/Context;)V", [&j](auto &c) {
        j.get(c.receiver)
            .fields["csdk/glucentralservices/util/AndroidPlatform#mContextLandroid/content/Context;"] =
            j.global_ref(c.arguments[0]);
        j.get(c.receiver).fields["csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ"] = 1;
        return 0;
    });
    hook("destroy", "()V", [&j](auto &c) {
        j.get(c.receiver).fields["csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ"] = 0;
        return 0;
    });
    hook("registerApplicationEvent", "()V", [&j, properties](auto &c) {
        for (auto receiver : properties->lifecycle_receivers)
            if (&j.get(receiver) == &j.get(c.receiver))
                return 0;
        properties->lifecycle_receivers.push_back(j.global_ref(c.receiver));
        return 0;
    });
    hook("scheduleEvent", "(JJ)V", [&j, &r](auto &c) {
        auto receiver = j.global_ref(c.receiver), token = c.arguments[1];
        auto delay = std::bit_cast<std::int64_t>(c.arguments[0]);
        require(delay >= 0, "Glu timer delay is negative");
        r.post_callback(
            [&r, receiver, token](guest::Cpu &cpu) {
                auto &jni = *r.jni;
                if (jni.get(receiver).fields.value(
                        "csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ", 0)) {
                    auto target = jni.native(platform, "onTimerEvent", "(J)V");
                    require(target != 0, "Glu timer callback binding is missing");
                    jni.call_native(cpu, target, std::array{jni.environment(cpu), receiver, token});
                }
                jni.delete_ref(receiver);
            },
            std::chrono::milliseconds(delay));
        return 0;
    });
    hook("showConsent", "(JJLandroid/app/Activity;Ljava/lang/String;)V", [&r, &j](auto &c) {
        require(c.arguments[2] != 0, "Glu consent requires an activity");
        auto asset = r.asset("consentform.html");
        require(asset.has_value(), "The APK is missing its consentform.html asset");
        auto bytes = read_bytes(r.path(asset->android_path));
        auto config = Json::parse(j.text(c.arguments[3]));
        config["plat"] = "android";
        if (config.value("app_name", "").empty())
            config["app_name"] = "Plants vs. Zombies 2";
        auto receiver = j.global_ref(c.receiver), token = c.arguments[0], event_token = c.arguments[1];
        auto finished = std::make_shared<std::atomic_bool>(false);
        auto finish = [&r, receiver, token, event_token, finished](std::string status) {
            if (r.stopping || finished->exchange(true))
                return;
            r.post_callback(
                [&r, receiver, token, event_token, status = std::move(status)](guest::Cpu &cpu) {
                    auto &jni = *r.jni;
                    auto callback = jni.native(platform, "onShowConsentComplete", "(JLjava/lang/String;)V");
                    auto cleanup = jni.native(platform, "deleteConsentCallbacks", "(JJ)V");
                    require(callback && cleanup, "Glu consent result bindings are missing");
                    jni.call_native(cpu, callback,
                                    std::array{jni.environment(cpu), receiver, token, jni.string(status)});
                    jni.call_native(cpu, cleanup,
                                    std::array{jni.environment(cpu), receiver, token, event_token});
                    jni.delete_ref(receiver);
                });
        };
        host::WebViewRequest request;
        request.title = "Plants vs. Zombies 2";
        request.html.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        request.callback_scheme = "consent";
        request.data_directory = r.options.data / "cache/webview/profile";
        request.preview = r.options.data / "cache/webview/consent.png";
        auto command = Json{{"args", {{"info", config.dump()}}}}.dump();
        request.page_script =
            "try { setUpConsentDialog(" + command +
            "); } catch (error) { chrome.webview.postMessage({action:'dismiss',status:'Error: "
            "'+error.message}); }";
        request.failure = [finish](std::string error) { finish("Error: " + error); };
        request.message = [&r, receiver, event_token, finished, finish](const Json &message) {
            auto action = message.value("action", "");
            if (action == "dismiss") {
                finish(message.value("status", "Error: Missing consent result"));
                return true;
            }
            if (action == "browser")
                host::open_url(message.value("url", ""));
            if (action == "load_complete" && message.value("status", "").starts_with("Error")) {
                finish(message.at("status").get<std::string>());
                return true;
            }
            if (action == "event" && !finished->load() && !r.stopping) {
                auto event = message.value("status", "");
                r.post_callback([&r, receiver, event_token, event = std::move(event)](guest::Cpu &cpu) {
                    auto &jni = *r.jni;
                    auto callback = jni.native(platform, "onConsentEvent", "(JLjava/lang/String;)V");
                    require(callback != 0, "Glu consent event binding is missing");
                    jni.call_native(
                        cpu, callback,
                        std::array{jni.environment(cpu), receiver, event_token, jni.string(event)});
                });
            }
            return false;
        };
        r.host->show_webview(std::move(request));
        return 0;
    });
    for (bool form : {false, true})
        hook(form ? "showCMPConsentForm" : "requestCMPConsentInfo",
             form ? "(JLandroid/app/Activity;ZZ)V" : "(JLandroid/app/Activity;)V", [&j, &r, form](auto &c) {
                 require(c.arguments[1] != 0, "Glu consent requires an activity");
                 auto receiver = j.global_ref(c.receiver), token = c.arguments[0];
                 r.post_callback([&r, receiver, token, form](guest::Cpu &cpu) {
                     auto &jni = *r.jni;
                     if (jni.get(receiver).fields.value(
                             "csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ", 0)) {
                         auto signature = form ? "(JZLjava/lang/String;Ljava/lang/String;Ljava/lang/"
                                                 "String;Ljava/lang/String;Ljava/lang/String;)V"
                                               : "(JZ)V";
                         auto target = jni.native(platform,
                                                  form ? "onShowCMPConsentFormComplete"
                                                       : "onRequestCMPConsentInfoComplete",
                                                  signature);
                         require(target != 0, "Glu unavailable-consent callback binding is missing");
                         if (form)
                             jni.call_native(
                                 cpu, target,
                                 std::array{jni.environment(cpu), receiver, token, Address{0}, Address{0},
                                            Address{0}, Address{0}, Address{0},
                                            jni.string("Consent service unavailable in offline mode")});
                         else
                             jni.call_native(cpu, target,
                                             std::array{jni.environment(cpu), receiver, token, Address{0}});
                     }
                     jni.delete_ref(receiver);
                 });
                 return 0;
             });
    hook("sendHTTPRequest",
         "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;JJ)V",
         [&j, &r](auto &c) {
             auto receiver = j.global_ref(c.receiver), token = c.arguments[5];
             r.post_callback([&r, receiver, token](guest::Cpu &cpu) {
                 auto &jni = *r.jni;
                 if (jni.get(receiver).fields.value(
                         "csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ", 0)) {
                     auto target = jni.native(platform, "onHTTPResponse", "(JI[BLjava/util/Map;)V");
                     require(target != 0, "Glu HTTP failure callback binding is missing");
                     jni.call_native(cpu, target,
                                     std::array{jni.environment(cpu), receiver, token, UINT64_MAX,
                                                Address{0}, Address{0}});
                 }
                 jni.delete_ref(receiver);
             });
             return 0;
         });
    hook("downloadFile", "(Ljava/lang/String;Ljava/lang/String;JJZ)V", [&j, &r](auto &c) {
        auto receiver = j.global_ref(c.receiver), token = c.arguments[3];
        r.post_callback([&r, receiver, token](guest::Cpu &cpu) {
            auto &jni = *r.jni;
            if (jni.get(receiver).fields.value("csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ",
                                               0)) {
                auto target = jni.native(platform, "onDownloadResponse", "(JILjava/lang/String;)V");
                require(target != 0, "Glu download failure callback binding is missing");
                jni.call_native(
                    cpu, target,
                    std::array{jni.environment(cpu), receiver, token, UINT64_MAX, jni.string("")});
            }
            jni.delete_ref(receiver);
        });
        return 0;
    });
    hook("setToStringStore", "(Ljava/lang/String;Ljava/lang/String;)V", [&j, strings](auto &c) {
        (*strings)[j.text(c.arguments[0])] =
            c.arguments[1] ? std::optional(j.text(c.arguments[1])) : std::nullopt;
        return 0;
    });
    hook("getFromStringStore", "(Ljava/lang/String;)Ljava/lang/String;", [&j, strings](auto &c) -> Address {
        auto value = strings->find(j.text(c.arguments[0]));
        return value == strings->end() || !value->second ? 0 : j.string(*value->second);
    });
    hook("privateFilePath", "()Ljava/lang/String;",
         [&j, &r](auto &) { return j.string("/data/data/" + r.options.package + "/files"); });
    hook("isNewInstall", "()Z", [&r](auto &) { return !fs::exists(r.options.data / "files/tags"); });
    hook("checkPrivateDirectoryExists", "(Ljava/lang/String;Z)Z", [&j, &r](auto &c) {
        auto path = r.resolve_path(j.text(c.arguments[0]), true);
        if (!path || fs::exists(*path) || !c.arguments[1])
            return false;
        std::error_code error;
        return fs::create_directories(*path, error);
    });
    hook("removePrivateData", "(Ljava/lang/String;)Z",
         [](auto &) { return false; }); // Original Java method performs no deletion.
    hook("getApplicationID", "()Ljava/lang/String;",
         [&j, &r](auto &) { return j.string(r.options.package); });
    hook("getApplicationVersion", "()Ljava/lang/String;",
         [&j, &r](auto &) { return j.string(r.options.version_name); });
    hook("getLocale", "()Ljava/lang/String;",
         [&j](auto &) { return j.string(""); }); // Original SDK returns an empty locale.
    hook("getLanguage", "()Ljava/lang/String;", [&j, &r](auto &) {
        auto separator = r.options.locale.find('_');
        auto language = r.options.locale.substr(0, separator);
        auto country = separator == std::string::npos ? "" : r.options.locale.substr(separator + 1);
        if (country == "TW" || country == "HK")
            return j.string(language + "-hant");
        return j.string((language == "es" || language == "pt") ? language + "-" + country : language);
    });
    hook("getCpuCoreCount", "()I",
         [](auto &) { return std::max(1u, std::thread::hardware_concurrency()); });
    hook("getCpuName", "()Ljava/lang/String;", [&j](auto &) { return j.string("arm64-v8a"); });
    // D3D11 desktop GPUs do not match the Java SDK's Android GPU model families.
    // Its unknown-device path is tier 1 with the 0x100 fallback flag.
    hook("getDeviceTier", "()I", [](auto &) { return 0x101; });
    hook("getETC2IfSupported", "()Ljava/lang/String;", [&j, &r](auto &) {
        auto version = r.graphics->describe().at("version").get<std::string>();
        return j.string(version.starts_with("OpenGL ES 3.") ? "ETC2" : "");
    });
    hook("getRamAmount", "()D", [](auto &) {
        return std::bit_cast<std::uint64_t>(double(guest::Memory::capacity) / (1024 * 1024));
    });
    for (const auto *name : {"getGpuDeviceName", "getGpuDeviceVendor"})
        hook(name, "()Ljava/lang/String;", [&j, &r, name](auto &) {
            auto description = r.graphics->describe();
            auto value =
                description.at(std::string_view(name) == "getGpuDeviceName" ? "renderer" : "vendor")
                    .get<std::string>();
            return j.string(value.empty() ? "Unknown" : value);
        });
    hook("getScreenSize", "()Ljava/lang/String;", [&j, &r](auto &) {
        auto description = r.graphics->describe();
        auto width = description.at("width").get<int>(), height = description.at("height").get<int>();
        return j.string(std::format("{}x{}", std::min(width, height), std::max(width, height)));
    });
    hook("generateUUID", "()Ljava/lang/String;", [&j](auto &) {
        std::random_device random;
        std::array<unsigned, 4> entropy{random(), random(), random(), random()};
        auto bytes = std::as_writable_bytes(std::span(entropy));
        bytes[6] = (bytes[6] & std::byte{15}) | std::byte{64};
        bytes[8] = (bytes[8] & std::byte{63}) | std::byte{128};
        std::string value;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                value += '-';
            value += std::format("{:02x}", std::to_integer<unsigned>(bytes[i]));
        }
        return j.string(value);
    });
    hook("readAssetAsString", "(Ljava/lang/String;)Ljava/lang/String;", [&j, &r](auto &c) -> Address {
        auto name = j.text(c.arguments[0]);
        if (!is_safe_relative(name))
            return 0;
        auto asset = r.asset(name);
        if (!asset)
            return 0;
        auto bytes = read_bytes(r.path(asset->android_path));
        return j.string(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    });
    hook("readProperties", "(Ljava/lang/String;[Ljava/lang/String;)[Ljava/lang/String;",
         [&j, properties](auto &c) {
             auto &values = properties->get(j.text(c.arguments[0]));
             auto output = j.object("[Ljava/lang/String;");
             for (auto key : j.get(c.arguments[1]).elements) {
                 auto name = j.text(key);
                 auto value = values.find(name);
                 if (value != values.end() && value->is_string()) {
                     j.get(output).elements.push_back(j.global_ref(key));
                     j.get(output).elements.push_back(j.global_ref(j.string(value->get<std::string>())));
                 }
             }
             return output;
         });
    hook("writeProperties", "(Ljava/lang/String;[Ljava/lang/String;)V", [&j, properties](auto &c) {
        auto name = j.text(c.arguments[0]);
        auto &values = properties->get(name);
        const auto &items = j.get(c.arguments[1]).elements;
        require(items.size() % 2 == 0, "Glu property updates require key/value pairs");
        for (size_t i = 0; i < items.size(); i += 2) {
            auto key = j.text(items[i]);
            if (items[i + 1])
                values[key] = j.text(items[i + 1]);
            else
                values.erase(key);
        }
        properties->save(name);
        return 0;
    });
    hook("readSharedProperty", "(Ljava/lang/String;)Ljava/lang/String;", [&j, properties](auto &c) {
        auto file = properties->path("glu-analytics");
        auto values = fs::exists(file) ? read_json(file) : properties->get("glu-analytics");
        auto found = values.find(j.text(c.arguments[0]));
        return j.string(
            found != values.end() && found->is_boolean() ? (found->get<bool>() ? "true" : "false") : "");
    });
    return [&r, properties](bool foreground) {
        std::vector<Address> receivers;
        {
            std::lock_guard lock(r.state_mutex);
            if (properties->foreground == foreground)
                return;
            properties->foreground = foreground;
            for (auto receiver : properties->lifecycle_receivers)
                receivers.push_back(r.jni->global_ref(receiver));
        }
        for (auto receiver : receivers)
            r.post_callback([&r, receiver, foreground](guest::Cpu &cpu) {
                auto &jni = *r.jni;
                if (jni.get(receiver).fields.value(
                        "csdk/glucentralservices/util/AndroidPlatform#mIsAvailableZ", 0)) {
                    auto target = jni.native(platform, "onApplicationEvent", "(Ljava/lang/String;)V");
                    require(target != 0, "Glu lifecycle callback binding is missing");
                    jni.call_native(cpu, target,
                                    std::array{jni.environment(cpu), receiver,
                                               jni.string(foreground ? "resume" : "pause")});
                }
                jni.delete_ref(receiver);
            });
    };
}
} // namespace acp::games

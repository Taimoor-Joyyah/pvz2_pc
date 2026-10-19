#include <acp/pvz2.hpp>

#include <random>
namespace acp::games {
std::map<std::string, Address> install_pvz2_environment(android::Runtime &r, Address activity) {
    auto &j = *r.jni;
    auto hook = [&r, &j](std::string type, std::string name, std::string signature,
                         android::JavaHandler handler) {
        j.hook(std::move(type), std::move(name), std::move(signature),
               [&r, handler = std::move(handler)](android::JavaCall &call) {
                   std::lock_guard lock(r.state_mutex);
                   return handler(call);
               });
    };
    constexpr auto type = "com/ea/nimble/ApplicationEnvironmentImpl";
    j.set_superclass(type, "com/ea/nimble/Component");
    j.set_interfaces(type, {"com/ea/nimble/IApplicationEnvironment"});
    auto instance = j.global_ref(j.object(type));
    j.get(instance).fields["component_id"] = "com.ea.nimble.applicationEnvironment";
    auto context = j.global_ref(j.object("android/app/Application"));
    auto language = std::make_shared<std::optional<std::string>>();
    auto player_id = std::make_shared<std::optional<std::string>>();
    auto players = j.global_ref(j.object("java/util/HashMap"));
    constexpr auto network_type = "com/ea/nimble/NetworkImpl";
    constexpr auto status_type = "com/ea/nimble/Network$Status";
    j.set_superclass(network_type, "com/ea/nimble/Component");
    j.set_interfaces(network_type, {"com/ea/nimble/INetwork"});
    j.set_superclass(status_type, "java/lang/Enum");
    std::vector<Address> statuses;
    for (const auto *name : {"UNKNOWN", "NONE", "DEAD", "OK"}) {
        auto value = j.object(status_type);
        j.get(value).fields = {{"name", name}, {"ordinal", statuses.size()}};
        statuses.push_back(j.global_ref(value));
        j.field(status_type, name, "Lcom/ea/nimble/Network$Status;", value);
    }
    hook(status_type, "toString", "()Ljava/lang/String;", [&j](auto &c) {
        return j.string("NET " + j.get(c.receiver).fields.at("name").template get<std::string>());
    });
    hook(status_type, "values", "()[Lcom/ea/nimble/Network$Status;", [&j, statuses](auto &) {
        auto array = j.object("[Lcom/ea/nimble/Network$Status;");
        for (auto status : statuses)
            j.get(array).elements.push_back(j.global_ref(status));
        return array;
    });
    auto network = j.global_ref(j.object(network_type));
    j.get(network).fields["component_id"] = "com.ea.nimble.network";
    hook("com/ea/nimble/Network", "getComponent", "()Lcom/ea/nimble/INetwork;",
         [&j, network](auto &) { return j.local_ref(network); });
    j.hook_suppressed("com/ea/nimble/Network", "getHttpProxy", "()Ljava/lang/String;",
                      "online-network", "null; no Android HTTP proxy", [](auto &) { return 0; });
    hook("com/ea/nimble/Utility", "readFile", "(Ljava/lang/String;)Ljava/lang/String;",
         [&j, &r](auto &c) -> Address {
             auto name = j.text(c.arguments[0]);
             if (!is_safe_relative(name))
                 return 0;
             auto asset = r.asset(name);
             if (!asset) {
                 r.message("[Nimble] APK asset unavailable: " + name);
                 return 0;
             }
             auto bytes = read_bytes(r.path(asset->android_path));
             return j.string(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
         });
    for (const auto *target : {"com/ea/nimble/INetwork", network_type}) {
        j.hook_suppressed(target, "getStatus", "()Lcom/ea/nimble/Network$Status;",
             "online-network", "Network.Status.NONE; offline gameplay remains available",
             [&j, none = statuses[1]](auto &) { return j.local_ref(none); });
        j.hook_suppressed(target, "isNetworkWifi", "()Z", "online-network", "false; Android Wi-Fi unavailable",
                          [](auto &) { return false; });
        j.hook_suppressed(target, "forceRedetectNetworkStatus", "()V", "online-network", "No-op; keep offline status",
                          [](auto &) { return 0; });
    }
    auto device_file = r.options.data / "preferences/pc-runtime-device.json";
    std::string device_id;
    if (fs::is_regular_file(device_file))
        device_id = read_json(device_file).at("android_id").get<std::string>();
    else {
        std::random_device random;
        std::array<unsigned, 4> entropy{random(), random(), random(), random()};
        device_id = sha256(std::as_bytes(std::span(entropy))).substr(0, 16);
        write_json(device_file, {{"android_id", device_id}});
    }
    auto configuration = r.options.application_metadata.value("com.ea.nimble.configuration", Json("live"));
    require(configuration.is_string() && is_safe_relative(configuration.get<std::string>()) &&
                configuration.get<std::string>().find('/') == std::string::npos,
            "Invalid Nimble environment directory");
    auto environment = configuration.get<std::string>();
    auto document_path = "/data/data/" + r.options.package + "/files/Nimble/" + environment;
    auto cache_path = "/data/data/" + r.options.package + "/cache/Nimble/" + environment;
    for (const auto &path : {document_path, cache_path, cache_path + "/temp"})
        fs::create_directories(r.path(path, true));
    hook("com/ea/nimble/ApplicationEnvironment", "getComponent",
         "()Lcom/ea/nimble/IApplicationEnvironment;",
         [&j, instance](auto &) { return j.local_ref(instance); });
    hook("com/ea/nimble/ApplicationEnvironment", "getCurrentActivity", "()Landroid/app/Activity;",
         [&j, activity](auto &) { return j.local_ref(activity); });
    for (const auto *name : {"isMainApplicationActive", "isMainApplicationRunning"})
        hook("com/ea/nimble/ApplicationEnvironment", name, "()Z", [&j, activity, name](auto &) {
            return j.get(activity).fields.value(
                std::string_view(name) == "isMainApplicationActive" ? "host_active" : "host_running", true);
        });
    for (const auto *target : {"com/ea/nimble/IApplicationEnvironment", type}) {
        auto string = [&](const char *name, std::string value) {
            hook(target, name, "()Ljava/lang/String;",
                 [&j, value = std::move(value)](auto &) { return j.string(value); });
        };
        string("getAndroidId", device_id);
        string("getApplicationBundleId", r.options.package);
        string("getApplicationName", "Plants vs. Zombies 2");
        string("getApplicationVersion", r.options.version_name);
        string("getDeviceBrand", "AndroidPcRuntime");
        string("getDeviceManufacturer", "AndroidPcRuntime");
        string("getDeviceCodename", "desktop");
        string("getDeviceModel", "Desktop");
        string("getDeviceFingerprint", "AndroidPcRuntime/desktop/arm64-v8a");
        string("getDeviceString", "AndroidPcRuntimeDesktop");
        string("getDocumentPath", document_path);
        string("getCachePath", cache_path);
        string("getTempPath", cache_path + "/temp");
        string("getOsVersion", "16");
        string("getCurrencyCode", "USD"); // RuntimeOptions currently selects en_US.
        for (const auto *name : {"getAdvertisingId", "getGoogleAdvertisingId", "getCarrier"})
            hook(target, name, "()Ljava/lang/String;", [](auto &) { return 0; });
        for (bool short_code : {false, true})
            hook(target, short_code ? "getShortApplicationLanguageCode" : "getApplicationLanguageCode",
                 "()Ljava/lang/String;", [&j, language, short_code](auto &) -> Address {
                     if (!*language)
                         return 0;
                     auto value = **language;
                     if (short_code)
                         value = value.substr(0, value.find('-'));
                     return j.string(value);
                 });
        hook(target, "setApplicationLanguageCode", "(Ljava/lang/String;)V", [&j, language](auto &c) {
            *language = c.arguments[0] ? std::optional(j.text(c.arguments[0])) : std::nullopt;
            return 0;
        });
        hook(target, "getApplicationContext", "()Landroid/content/Context;",
             [&j, context](auto &) { return j.local_ref(context); });
        hook(target, "getPlayerIdMap", "()Ljava/util/Map;",
             [&j, players](auto &) { return j.local_ref(players); });
        hook(target, "getGameSpecifiedPlayerId", "()Ljava/lang/String;",
             [&j, player_id](auto &) -> Address { return *player_id ? j.string(**player_id) : 0; });
        hook(target, "setGameSpecifiedPlayerId", "(Ljava/lang/String;)V", [&j, player_id](auto &c) {
            *player_id = c.arguments[0] ? std::optional(j.text(c.arguments[0])) : std::nullopt;
            return 0;
        });
        hook(target, "setPlayerId", "(Ljava/lang/String;Ljava/lang/String;)V", [&j, players](auto &c) {
            auto &items = j.get(players).elements;
            for (size_t i = 0; i < items.size(); i += 2)
                if (j.text(items[i]) == j.text(c.arguments[0])) {
                    j.delete_ref(items[i + 1]);
                    items[i + 1] = j.global_ref(c.arguments[1]);
                    return 0;
                }
            items.push_back(j.global_ref(c.arguments[0]));
            items.push_back(j.global_ref(c.arguments[1]));
            return 0;
        });
        hook(target, "getAgeCompliance", "()I", [](auto &) { return UINT64_MAX; });
        hook(target, "refreshAgeCompliance", "()V", [&r](auto &) {
            r.message("[Nimble] Age compliance refresh unavailable while offline");
            return 0;
        });
        hook(target, "isLimitAdTrackingEnabled", "()Z", [](auto &) { return true; });
        hook(target, "getIadAttribution", "()Z", [](auto &) { return false; });
        for (const auto *name : {"isDeviceRooted", "isAppCracked"})
            hook(target, name, "()Z", [](auto &) { return false; });
        hook(target, "getParameter", "(Ljava/lang/String;)Ljava/lang/String;",
             [&j, &r, device_id](auto &c) -> Address {
                 auto name = j.text(c.arguments[0]);
                 const std::map<std::string, std::string> values{
                     {"androidId", device_id},
                     {"deviceModel", "Desktop"},
                     {"deviceManufacturer", "AndroidPcRuntime"},
                     {"deviceBrand", "AndroidPcRuntime"},
                     {"deviceCodename", "desktop"},
                     {"deviceAbis", "arm64-v8a"},
                     {"deviceLocale", r.options.locale},
                     {"deviceLanguage", r.options.locale.substr(0, r.options.locale.find('_'))},
                     {"countryCode", r.options.locale.find('_') == std::string::npos
                                         ? ""
                                         : r.options.locale.substr(r.options.locale.find('_') + 1)},
                     {"systemName", "Android"},
                     {"systemVersion", "16"},
                     {"platform", "android"},
                     {"limitAdTracking", "true"}};
                 auto value = values.find(name);
                 return value == values.end() ? 0 : j.string(value->second);
             });
    }
    return {{"com.ea.nimble.applicationEnvironment", instance}, {"com.ea.nimble.network", network}};
}
} // namespace acp::games

#include <acp/pvz2.hpp>

#include <chrono>
#include <cmath>
#include <thread>
namespace acp::games {
namespace {
constexpr std::string_view activity = "com/popcap/SexyAppFramework/SexyAppFrameworkActivity",
                           surface = "com/popcap/SexyAppFramework/AndroidSurfaceView";
class Pvz2 final : public android::GameAdapter {
    Address activity_{}, surface_{}, http_proxy_{};
    int width_ = 1280, height_ = 720;
    host::Viewport viewport_{0, 0, 1280, 720};
    bool down_{}, keyboard_{};
    std::atomic_bool quitting_{};
    Pvz2Options options_;
    std::atomic_int frame_rate_{60};
    std::atomic<float> view_scale_{0.5f};
    std::optional<bool> audio_focused_;
    std::chrono::steady_clock::time_point previous_frame_{};
    Json preferences_ = Json::object();
    fs::path preferences_file_;
    Pvz2Input input_;
    std::map<std::string, Address> nimble_components_;
    std::map<std::string, Address> java_components_;
    std::map<std::string, std::chrono::steady_clock::time_point> traces_;
    bool nimble_setup_{};
    std::function<void(bool)> glu_lifecycle_;
    Address target(android::Runtime &r, std::string_view type, std::string_view name,
                   std::string_view signature) {
        if (auto address = r.jni->native(type, name, signature))
            return address;
        require(false, "PVZ2 native binding missing: " + std::string(type) + "#" + std::string(name) +
                           std::string(signature));
        return 0;
    }
    void nimble_broadcast(android::Runtime &r, std::string action, Address values = 0) {
        std::vector<Address> ids;
        {
            std::lock_guard lock(r.state_mutex);
            auto [first, last] = r.broadcast_receivers.equal_range(action);
            for (; first != last; ++first)
                ids.push_back(r.jni->get(first->second)
                                  .fields.at("com/ea/nimble/bridge/BaseNativeCallback#m_idI")
                                  .get<Address>());
        }
        Address snapshot{};
        if (values && !ids.empty()) {
            std::lock_guard lock(r.state_mutex);
            snapshot = r.jni->object("java/util/HashMap");
            for (auto value : r.jni->get(values).elements)
                r.jni->get(snapshot).elements.push_back(r.jni->global_ref(value));
        }
        for (auto id : ids) {
            auto payload_reference = r.jni->global_ref(snapshot);
            r.post_callback([&r, id, action, payload_reference](guest::Cpu &cpu) {
                auto &jni = *r.jni;
                auto type = jni.class_ref("com/ea/nimble/bridge/BaseNativeCallback");
                auto data = jni.object("[Ljava/lang/Object;");
                auto text = jni.string(action);
                auto payload =
                    payload_reference ? jni.local_ref(payload_reference) : jni.object("java/util/HashMap");
                jni.delete_ref(payload_reference);
                jni.get(data).elements = {jni.global_ref(text), jni.global_ref(payload)};
                auto function = jni.native("com/ea/nimble/bridge/BaseNativeCallback", "nativeCallback",
                                           "(I[Ljava/lang/Object;)V");
                require(function != 0, "Nimble native notification callback is missing");
                jni.call_native(cpu, function, std::array{jni.environment(cpu), type, id, data});
            });
        }
    }
    void nimble_stage(android::Runtime &r, guest::Cpu &cpu, std::string_view stage) {
        constexpr auto type = "com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent";
        auto function = target(r, type, stage, "()V");
        std::vector<std::pair<std::string, Address>> components;
        {
            std::lock_guard lock(r.state_mutex);
            components.assign(nimble_components_.begin(), nimble_components_.end());
        }
        for (const auto &[id, component] : components) {
            r.message(std::format("[Nimble] {} {}", stage, id));
            r.jni->call_native(cpu, function, std::array{r.jni->environment(cpu), component});
        }
    }
    std::uint64_t call(android::Runtime &r, std::string_view type, std::string_view name,
                       std::string_view signature, std::initializer_list<std::uint64_t> arguments = {}) {
        std::vector<std::uint64_t> args{r.jni->environment(), type == surface ? surface_ : activity_};
        args.insert(args.end(), arguments);
        const bool startup =
            name == "Native_GameAppInitialize" || name == "Native_applicationWillFinishLaunching" ||
            name == "Native_applicationDidFinishLaunching" || name == "Native_onSurfaceCreated";
        // Early draw callbacks also perform synchronous RTON decoding and resource setup.
        const bool drawing = name == "Native_onDrawFrame";
        return r.jni->call_native(r.cpu, target(r, type, name, signature), args,
                                  startup   ? 10'000'000'000ull
                                  : drawing ? 2'000'000'000ull
                                            : 200'000'000ull,
                                  std::chrono::seconds(startup   ? 120
                                                       : drawing ? 60
                                                                 : 30));
    }
    void audio_focus(android::Runtime &r, bool focused) {
        if (audio_focused_ == focused)
            return;
        audio_focused_ = focused;
        // The Android shell keeps application activity separate from audio focus.
        // Repeating DidBecomeActive on desktop focus gains restarts native music.
        call(r, activity, focused ? "Native_NotifyAudioGainedFocus" : "Native_NotifyAudioLostFocus", "()V");
        r.message(
            std::format("[AudioFocus] {}: {}", focused ? "gained" : "lost", r.audio->describe().dump()));
    }

  public:
    explicit Pvz2(Pvz2Options options) : options_(std::move(options)) {
        require((options_.render_width == 0 && options_.render_height == 0) ||
                    (options_.render_width >= 64 && options_.render_height >= 64 &&
                     std::uint64_t(options_.render_width) * options_.render_height <= 16'777'216),
                "Invalid Android render resolution");
        require(options_.fps_limit >= 0 && options_.fps_limit <= 240, "Invalid frame limit");
        if (options_.render_width) {
            width_ = options_.render_width;
            height_ = options_.render_height;
        }
    }
    bool quit_requested() const override {
        return quitting_;
    }
    bool accepts(const android::Manifest &m) const override {
        return m.package == "com.ea.game.pvz2_row";
    }
    std::string_view main_library() const override {
        return "libPVZ2.so";
    }
    void configure(android::Runtime &r, const android::Installation &) override {
        r.java_libraries = {"libNimble.so", "libPVZ2.so"};
        auto &j = *r.jni;
        install_pvz2_persistence(r);
        glu_lifecycle_ = install_pvz2_glu(r);
        install_pvz2_analytics(r);
        install_pvz2_ads(r);
        install_pvz2_http(r);
        j.set_superclass("com/ea/nimble/Error", "java/lang/Exception");
        for (bool cause : {false, true})
            j.hook("com/ea/nimble/Error", "<init>",
                   cause ? "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/Throwable;)V"
                         : "(Ljava/lang/String;ILjava/lang/String;)V",
                   [&j, cause](auto &c) {
                       auto &fields = j.get(c.receiver).fields;
                       fields["com/ea/nimble/Error#m_domainLjava/lang/String;"] =
                           j.global_ref(c.arguments[0]);
                       fields["com/ea/nimble/Error#m_codeI"] = c.arguments[1];
                       fields["message"] = c.arguments[2] ? Json(j.text(c.arguments[2])) : Json(nullptr);
                       fields["cause"] = cause ? j.global_ref(c.arguments[3]) : 0;
                       return 0;
                   });
        j.hook("com/ea/nimble/Error", "getDomain", "()Ljava/lang/String;", [&j](auto &c) {
            return j.local_ref(j.get(c.receiver)
                                   .fields.at("com/ea/nimble/Error#m_domainLjava/lang/String;")
                                   .template get<Address>());
        });
        j.hook("com/ea/nimble/Error", "getCode", "()I", [&j](auto &c) {
            return j.get(c.receiver).fields.at("com/ea/nimble/Error#m_codeI").template get<std::uint64_t>();
        });
        j.set_superclass("com/ea/nimble/SynergyEnvironmentImpl", "com/ea/nimble/Component");
        j.set_interfaces("com/ea/nimble/SynergyEnvironmentImpl", {"com/ea/nimble/ISynergyEnvironment"});
        auto synergy = j.global_ref(j.object("com/ea/nimble/SynergyEnvironmentImpl"));
        j.hook("com/ea/nimble/SynergyEnvironment", "getComponent", "()Lcom/ea/nimble/ISynergyEnvironment;",
               [&j, synergy](auto &) { return j.local_ref(synergy); });
        // Match the Java SDK's defaults when no server environment is cached.
        for (const auto *type :
             {"com/ea/nimble/ISynergyEnvironment", "com/ea/nimble/SynergyEnvironmentImpl"}) {
            for (const auto *name :
                 {"getEADeviceId", "getEAHardwareId", "getGosMdmAppKey", "getNexusClientId",
                  "getNexusClientSecret", "getProductId", "getSellId", "getSynergyId"})
                j.hook(type, name, "()Ljava/lang/String;", [](auto &) { return 0; });
            j.hook(type, "getServerUrlWithKey", "(Ljava/lang/String;)Ljava/lang/String;",
                   [](auto &) { return 0; });
            for (const auto *name : {"isDataAvailable", "isUpdateInProgress"})
                j.hook(type, name, "()Z", [](auto &) { return false; });
            j.hook(type, "isFeatureDisabled", "(Ljava/lang/String;)Z", [](auto &) { return false; });
            j.hook(type, "getTrackingPostInterval", "()I", [](auto &) { return UINT64_MAX; });
            j.hook(type, "getLatestAppVersionCheckResult", "()I", [](auto &) { return 0; });
            j.hook(
                type, "checkAndInitiateSynergyEnvironmentUpdate", "()Lcom/ea/nimble/Error;", [&j](auto &) {
                    auto error = j.object("com/ea/nimble/Error");
                    j.get(error).fields = {{"com/ea/nimble/Error#m_domainLjava/lang/String;",
                                            j.global_ref(j.string("NimbleError"))},
                                           {"com/ea/nimble/Error#m_codeI", 1002},
                                           {"message", "Network services are unavailable in offline mode"},
                                           {"cause", 0}};
                    return error;
                });
        }
        j.set_superclass(std::string(activity), "android/app/Activity");
        j.set_superclass(std::string(surface), "android/view/SurfaceView");
        j.set_superclass("com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent",
                         "com/ea/nimble/Component");
        activity_ = j.object(std::string(activity));
        surface_ = j.object(std::string(surface));
        http_proxy_ = j.object("com/popcap/SexyAppFramework/AndroidHttpProxy");
        java_components_ = install_pvz2_environment(r, activity_);
        java_components_["com.ea.nimble.synergyEnvironment"] = synergy;
        preferences_file_ = r.options.data / "preferences" / "popcap.json";
        if (fs::exists(preferences_file_))
            preferences_ = read_json(preferences_file_);
        auto hook = [&](std::string_view type, std::string_view name, std::string_view sig,
                        android::JavaHandler fn) {
            j.hook(std::string(type), std::string(name), std::string(sig), std::move(fn));
        };
        for (const auto *name : {"Cloud_Connect", "Cloud_attemptSilentSync"})
            hook("com/popcap/SexyAppFramework/cloud/Cloud", name, "()V", [](auto &) { return 0; });
        hook("com/popcap/SexyAppFramework/cloud/Cloud", "Cloud_GetPcpId", "()Ljava/lang/String;",
             [&j](auto &) { return j.string(""); });
        hook("com/popcap/SexyAppFramework/cloud/Cloud", "Cloud_SetPcpId", "(Ljava/lang/String;)V",
             [](auto &) { return 0; });
        install_pvz2_play(r);
        java_components_["com.ea.nimble.mtx.googleplay"] = install_pvz2_billing(
            r, [this, &r](auto action, auto payload) { nimble_broadcast(r, std::move(action), payload); });
        auto notification_channels = std::make_shared<Json>(Json::object());
        hook("com/popcap/SexyAppFramework/AndroidNotification", "getScheduledLocalNotificationsData",
             "()Ljava/lang/String;", [&j](auto &) { return j.string("[]"); });
        hook("com/popcap/SexyAppFramework/AndroidNotification", "getAllNotificationIds",
             "()Ljava/util/List;", [&j](auto &) { return j.object("java/util/ArrayList"); });
        hook("com/popcap/SexyAppFramework/AndroidNotification", "getTimezone", "()Ljava/lang/String;",
             [&j](auto &) { return j.string(std::string(std::chrono::current_zone()->name())); });
        for (auto [name, signature] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"removeAllScheduledNotifications", "()V"},
                 {"removeNotification", "(I)V"},
                 {"removeNotifications", "(Ljava/util/List;)V"},
                 {"removeScheduledNotification", "(I)V"},
                 {"removeScheduledNotifications", "(Ljava/util/List;)V"},
                 {"removeScheduledNotificationsBySource", "(Ljava/lang/String;)V"}})
            hook("com/popcap/SexyAppFramework/AndroidNotification", name, signature,
                 [](auto &) { return 0; });
        auto notification_warning = std::make_shared<bool>(false);
        hook("com/popcap/SexyAppFramework/AndroidNotification", "scheduleBasicNotification",
             "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
             "String;Ljava/lang/String;IILjava/lang/String;IILjava/lang/String;Ljava/lang/String;)V",
             [&r, notification_warning](auto &) {
                 std::lock_guard lock(r.state_mutex);
                 if (!std::exchange(*notification_warning, true))
                     r.message("[PopCap] Desktop notifications unavailable; no notification was scheduled");
                 return 0;
             });
        hook("com/popcap/SexyAppFramework/AndroidNotification", "deleteNotificationChannel",
             "(Ljava/lang/String;)V", [&r, &j, notification_channels](auto &c) {
                 std::lock_guard lock(r.state_mutex);
                 notification_channels->erase(j.text(c.arguments[0]));
                 return 0;
             });
        hook("com/popcap/SexyAppFramework/AndroidNotification", "createNotificationChannel",
             "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
             [&r, &j, notification_channels](auto &c) {
                 std::lock_guard lock(r.state_mutex);
                 (*notification_channels)[j.text(c.arguments[0])] = {
                     {"name", j.text(c.arguments[1])},
                     {"description", j.text(c.arguments[2])},
                     {"sound", j.text(c.arguments[3])}};
                 return 0;
             });
        auto asset_file = [&r](std::string filename) -> std::optional<android::Runtime::AssetFile> {
            if (!filename.starts_with("ASSET:")) {
                auto path = r.resolve_path(filename);
                if (path && fs::is_regular_file(*path))
                    return android::Runtime::AssetFile{filename, static_cast<size_t>(fs::file_size(*path))};
            }
            if (filename.starts_with("ASSET:"))
                filename.erase(0, 6);
            std::replace(filename.begin(), filename.end(), '\\', '/');
            if (!filename.ends_with(".smf") && !filename.ends_with(".rton"))
                filename += ".smf";
            if (!is_safe_relative(filename))
                return {};
            return r.asset(filename);
        };
        hook(activity, "Resources_GetAssetFileSize", "(Ljava/lang/String;)J", [&j, asset_file](auto &c) {
            auto file = asset_file(j.text(c.arguments[0]));
            return file ? static_cast<std::uint64_t>(file->size) : UINT64_MAX;
        });
        hook(activity, "Resources_GetAssetFileInfo", "(Ljava/lang/String;[J)Ljava/lang/String;",
             [&j, asset_file](auto &c) -> Address {
                 auto file = asset_file(j.text(c.arguments[0]));
                 if (!file)
                     return 0;
                 auto &bytes = j.get(c.arguments[1]).bytes;
                 require(bytes.size() >= 16, "PVZ2 asset information array is too small");
                 std::uint64_t values[] = {0, file->size};
                 std::memcpy(bytes.data(), values, 16);
                 return j.string(file->android_path);
             });
        hook(activity, "instance", "()Lcom/popcap/SexyAppFramework/SexyAppFrameworkActivity;",
             [this, &j](auto &) { return j.local_ref(activity_); });
        hook(surface, "GetActivity", "()Landroid/app/Activity;",
             [this, &j](auto &) { return j.local_ref(activity_); });
        hook("com/ea/nimble/bridge/BaseNativeCallback", "<init>", "(I)V", [&j](auto &c) {
            j.get(c.receiver).fields["com/ea/nimble/bridge/BaseNativeCallback#m_idI"] =
                static_cast<std::uint32_t>(c.arguments[0]);
            return 0;
        });
        hook("com/ea/nimble/Utility", "registerReceiver",
             "(Ljava/lang/String;Landroid/content/BroadcastReceiver;)V", [&r, &j](auto &c) {
                 std::lock_guard lock(r.state_mutex);
                 r.broadcast_receivers.emplace(j.text(c.arguments[0]), j.global_ref(c.arguments[1]));
                 return 0;
             });
        hook("com/ea/nimble/Utility", "sendBroadcastSerializable", "(Ljava/lang/String;Ljava/util/Map;)V",
             [this, &r, &j](auto &c) {
                 nimble_broadcast(r, j.text(c.arguments[0]), c.arguments[1]);
                 return 0;
             });
        hook("com/ea/nimble/Utility", "sendBroadcast", "(Ljava/lang/String;)V", [this, &r, &j](auto &c) {
            nimble_broadcast(r, j.text(c.arguments[0]));
            return 0;
        });
        hook("com/ea/nimble/Utility", "unregisterReceiver", "(Landroid/content/BroadcastReceiver;)V",
             [&r, &j](auto &c) {
                 std::lock_guard lock(r.state_mutex);
                 for (auto it = r.broadcast_receivers.begin(); it != r.broadcast_receivers.end();) {
                     if (&j.get(it->second) == &j.get(c.arguments[0])) {
                         j.delete_ref(it->second);
                         it = r.broadcast_receivers.erase(it);
                     } else
                         ++it;
                 }
                 return 0;
             });
        j.field("com/popcap/SexyAppFramework/AndroidNotification", "mContext", "Landroid/content/Context;",
                activity_);
        hook("com/popcap/SexyAppFramework/AndroidNotification", "<init>", "()V", [this, &j](auto &c) {
            j.get(c.receiver).fields["context"] = activity_;
            return 0;
        });
        hook("com/ea/nimble/bridge/NimbleCppComponentRegistrar", "register", "(Ljava/lang/String;)V",
             [this, &j](auto &c) {
                 auto id = j.text(c.arguments[0]);
                 auto component =
                     j.object("com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent");
                 j.get(component).fields["component_id"] = id;
                 j.get(component).fields
                     ["com/ea/nimble/bridge/"
                      "NimbleCppComponentRegistrar$NimbleCppComponent#m_componentIdLjava/lang/String;"] =
                     j.global_ref(j.string(id));
                 if (auto existing = nimble_components_.find(id); existing != nimble_components_.end())
                     j.delete_ref(existing->second);
                 nimble_components_[id] = j.global_ref(component);
                 return 0;
             });
        hook("com/ea/nimble/Base", "getComponent", "(Ljava/lang/String;)Lcom/ea/nimble/Component;",
             [this, &j, &r](auto &c) -> Address {
                 std::lock_guard lock(r.state_mutex);
                 auto id = j.text(c.arguments[0]);
                 if (auto it = nimble_components_.find(id); it != nimble_components_.end())
                     return j.local_ref(it->second);
                 if (auto it = java_components_.find(id); it != java_components_.end())
                     return j.local_ref(it->second);
                 return 0;
             });
        hook("com/ea/nimble/Base", "isComponentAvailable", "(Ljava/lang/String;)Z",
             [this, &j, &r](auto &c) {
                 std::lock_guard lock(r.state_mutex);
                 auto id = j.text(c.arguments[0]);
                 return nimble_components_.contains(id) || java_components_.contains(id);
             });
        hook("com/ea/nimble/Base", "setupNimble", "()V", [this, &r](auto &c) {
            if (!nimble_setup_) {
                nimble_stage(r, c.cpu, "setup");
                nimble_setup_ = true;
                nimble_broadcast(r, "nimble.notification.componentIndependentSetupFinished");
                nimble_stage(r, c.cpu, "restore");
            }
            return 0;
        });
        hook("com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent", "getComponentId",
             "()Ljava/lang/String;", [&j](auto &c) {
                 return j.string(j.get(c.receiver).fields.at("component_id").template get<std::string>());
             });
        hook("com/ea/nimble/bridge/NimbleCppComponentRegistrar", "getComponentId",
             "(Lcom/ea/nimble/Component;)Ljava/lang/String;", [&j](auto &c) {
                 return j.string(
                     j.get(c.arguments[0]).fields.at("component_id").template get<std::string>());
             });
        auto logger = j.object("com/ea/nimble/ILog");
        j.get(logger).fields["threshold"] = 3;
        hook("com/ea/nimble/Log", "getComponent", "()Lcom/ea/nimble/ILog;",
             [&j, logger](auto &) { return j.local_ref(logger); });
        hook("com/ea/nimble/NimbleApplicationConfiguration", "configValueExists", "(Ljava/lang/String;)Z",
             [&r, &j](auto &c) { return r.options.application_metadata.contains(j.text(c.arguments[0])); });
        for (const auto *kind : {"Boolean", "Int", "Double", "String"}) {
            auto descriptor = std::string(std::string_view(kind) == "Boolean"  ? "Z"
                                          : std::string_view(kind) == "Int"    ? "I"
                                          : std::string_view(kind) == "Double" ? "D"
                                                                               : "Ljava/lang/String;");
            for (bool with_default : {false, true})
                hook("com/ea/nimble/NimbleApplicationConfiguration", "getConfigValueAs" + std::string(kind),
                     "(Ljava/lang/String;" + (with_default ? descriptor : "") + ")" + descriptor,
                     [&r, &j, kind, with_default](auto &c) -> std::uint64_t {
                         auto key = j.text(c.arguments[0]);
                         auto &metadata = r.options.application_metadata;
                         auto it = metadata.find(key);
                         if (it == metadata.end())
                             return with_default
                                        ? (std::string_view(kind) == "String" ? j.local_ref(c.arguments[1])
                                                                              : c.arguments[1])
                                        : 0;
                         const auto &value = *it;
                         if (std::string_view(kind) == "String") {
                             require(!value.is_object(), "Resource-valued Nimble configuration requires "
                                                         "resources.arsc resolution: " +
                                                             key);
                             return value.is_string() ? j.string(value.get<std::string>()) : 0;
                         }
                         if (std::string_view(kind) == "Boolean")
                             return value.is_boolean() ? value.get<bool>() : false;
                         if (std::string_view(kind) == "Double")
                             return value.is_number_float()
                                        ? std::bit_cast<std::uint64_t>(value.get<double>())
                                        : 0;
                         return value.is_number_integer()
                                    ? static_cast<std::uint64_t>(value.get<std::int32_t>())
                                    : 0;
                     });
        }
        hook("com/ea/nimble/ILog", "getThresholdLevel", "()I", [&j](auto &c) {
            return j.get(c.receiver).fields.at("threshold").template get<std::uint64_t>();
        });
        hook("com/ea/nimble/ILog", "setThresholdLevel", "(I)V", [&j](auto &c) {
            j.get(c.receiver).fields["threshold"] = c.arguments[0];
            return 0;
        });
        hook("com/ea/nimble/ILog", "getLogFilePath", "()Ljava/lang/String;", [&r, &j](auto &) {
            return j.string("/data/data/" + r.options.package + "/files/nimble.log");
        });
        hook("com/ea/nimble/ILog", "setLogCallback", "(Lcom/ea/nimble/ILog$LogCallback;)V", [&j](auto &c) {
            auto &fields = j.get(c.receiver).fields;
            j.delete_ref(fields.value("callback", Address{}));
            fields["callback"] = j.global_ref(c.arguments[0]);
            return 0;
        });
        for (const auto *kind : {"Title", "Source"})
            hook("com/ea/nimble/ILog", "writeWith" + std::string(kind),
                 std::string_view(kind) == "Title"
                     ? "(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/Object;)V"
                     : "(ILjava/lang/Object;Ljava/lang/String;[Ljava/lang/Object;)V",
                 [&r, &j](auto &c) {
                     auto text = j.text(c.arguments[2]);
                     r.message("[Nimble] " + text);
                     std::ofstream stream(r.options.data / "files/nimble.log", std::ios::app);
                     stream << text << '\n';
                     return 0;
                 });
        auto string = [&](std::string_view name, std::function<std::string()> value) {
            hook(activity, name, "()Ljava/lang/String;",
                 [&j, value = std::move(value)](auto &) { return j.string(value()); });
        };
        const auto package = r.options.package;
        string("Info_SysGetPackageName", [package] { return package; });
        string("getAndroidPackageName", [package] { return package; });
        string("Info_SysGetActivityName", [] { return "com.popcap.PvZ2.PvZ2GameActivity"; });
        string("Info_SysGetProductVersionString", [&r] { return r.options.version_name; });
        string("Info_SysGetUserLocale", [&r] { return r.options.locale; });
        string("Info_SysGetCountryCodeString", [] { return "US"; });
        string("Info_SysGetUserCurrencyCode", [] { return "USD"; });
        string("Info_SysGetUserCurrencySymbol", [] { return "$"; });
        string("Device_GetDeviceName", [] { return "Desktop"; });
        string("Diag_GetHardwareModel", [] { return "Windows x64"; });
        string("Diag_GetOSVersion", [] { return "16"; });
        string("Resources_GetAppSupportDataFolder",
               [package] { return "/sdcard/Android/data/" + package + "/files"; });
        string("Resources_GetUserDataFolder",
               [package] { return "/sdcard/Android/data/" + package + "/files"; });
        string("Resources_GetCacheDataFolder",
               [package] { return "/sdcard/Android/data/" + package + "/files"; });
        string("Device_GetCachesDir", [package] { return "/data/data/" + package + "/cache"; });
        string("Resources_GetExternalStorageDirectory", [] { return "/sdcard"; });
        string("Resources_GetResourceFolder", [package] { return "/data/data/" + package + "/files"; });
        string("FrameworkInfo_SysGetMainExpansionFilePath", [&r, package] {
            return "/sdcard/Android/obb/" + package + "/" + utf8(r.options.obb.filename());
        });
        string("Util_GetUUIDString", [&r] {
            auto path = r.options.data / "preferences" / "device.json";
            if (!fs::exists(path)) {
                auto token = sha256(std::as_bytes(std::span(r.options.package)));
                write_json(path, {{"uuid", token.substr(0, 32)}});
            }
            return read_json(path).at("uuid").get<std::string>();
        });
        string("Diag_GetDeviceID", [&r] {
            auto token = sha256(std::as_bytes(std::span(r.options.package)));
            return token.substr(0, 32);
        });
        hook(activity, "Info_SysGetProductVersion", "()I", [&r](auto &) { return r.options.version_code; });
        hook(activity, "Info_SysGetIntentExtraDataString", "(Ljava/lang/String;)Ljava/lang/String;",
             [](auto &) -> std::uint64_t { return 0; });
        hook(activity, "getHasCrashReport", "()Z", [](auto &) { return false; });
        hook(activity, "getLoadedLibraries", "()Z",
             [&r](auto &) { return r.java_initialized.size() == r.java_libraries.size(); });
        hook(activity, "StartTracing", "(Ljava/lang/String;)V", [this, &j, &r](auto &c) {
            std::lock_guard lock(r.state_mutex);
            traces_.try_emplace(j.text(c.arguments[0]), std::chrono::steady_clock::now());
            return 0;
        });
        hook(activity, "StopTracing", "(Ljava/lang/String;)V", [this, &j, &r](auto &c) {
            std::lock_guard lock(r.state_mutex);
            auto name = j.text(c.arguments[0]);
            auto it = traces_.find(name);
            if (it != traces_.end()) {
                auto ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - it->second)
                        .count();
                r.message(std::format("[trace] {}: {:.2f} ms", name, ms));
                traces_.erase(it);
            }
            return 0;
        });
        // The desktop host does not create the optional Java splash overlay.
        hook(activity, "SplashScreenShowing", "()Z", [](auto &) { return false; });
        hook(activity, "RemoveSplashScreen", "()V", [](auto &) { return 0; });
        hook(activity, "Web_SysOpenURL", "(Ljava/lang/String;)Z", [](auto &) { return false; });
        for (const auto *name : {"Diag_GetMemoryAvailable", "Diag_GetMemoryTotal"})
            hook(activity, name, "()J", [](auto &) { return 2ull << 30; });
        hook(activity, "Diag_GetMemoryUsed", "()J", [](auto &) { return 128ull << 20; });
        hook(activity, "Device_IsTablet", "()Z", [](auto &) { return 1; });
        hook(activity, "Device_GetCurrentUIOrientation", "()I", [](auto &) { return 3; });
        hook(activity, "Device_IsSupportedUIOrientation", "(I)Z",
             [](auto &c) { return c.arguments[0] == 3 || c.arguments[0] == 4; });
        hook(activity, "Device_ForceRotateToOrientation", "(I)V", [](auto &) { return 0; });
        hook(activity, "Device_IsKeyboardShowing", "()Z", [this](auto &) { return keyboard_; });
        for (const auto *name : {"Device_ShowKeyboard", "Device_ShowNumericKeyboard"})
            hook(activity, name, "()V", [this, &r](auto &) {
                keyboard_ = true;
                r.host->start_text(true);
                return 0;
            });
        hook(activity, "Device_HideKeyboard", "()V", [this, &r](auto &) {
            keyboard_ = false;
            r.host->start_text(false);
            return 0;
        });
        hook(activity, "UI_DidRecieveFocus", "()V", [](auto &) { return 0; });
        hook(activity, "UI_ProcessEvents", "(Ljava/nio/ByteBuffer;)Z", [this, &r, &j](auto &c) {
            if (!c.arguments[0])
                return false;
            auto &buffer = j.get(c.arguments[0]);
            auto address = buffer.fields.at("address").get<Address>(),
                 capacity = buffer.fields.at("capacity").get<size_t>();
            return input_.drain(r.memory.writable(address, capacity));
        });
        hook(activity, "Device_ExitToHome", "()V", [this](auto &) {
            quitting_ = true;
            return 0;
        });
        hook(activity, "Device_KillApplication", "()V", [this](auto &) {
            quitting_ = true;
            return 0;
        });
        // AndroidSurfaceView stores the native renderer's selected scale. Returning a
        // constant breaks the final offscreen-to-surface transform and crops the UI.
        hook(surface, "Graphics_GetGLViewScaleFactor", "()F",
             [this](auto &) { return std::bit_cast<std::uint32_t>(view_scale_.load()); });
        hook(surface, "Graphics_GetPointSizeInPixels", "()F",
             [](auto &) { return std::bit_cast<std::uint32_t>(1.f); });
        hook(surface, "Graphics_CanSetGLViewScaleFactor", "()Z", [](auto &) { return 1; });
        hook(surface, "Graphics_GetGLViewSysFBO", "()I", [](auto &) { return 0; });
        hook(surface, "Graphics_SetGLViewScaleFactor", "(F)V", [this, &r](auto &c) {
            auto scale = std::bit_cast<float>(static_cast<std::uint32_t>(c.arguments[0]));
            require(std::isfinite(scale) && scale > 0 && scale <= 16,
                    "Invalid native GL view scale factor");
            view_scale_ = scale;
            r.message(std::format("[Display] Native GL view scale: {}", scale));
            return 0;
        });
        hook(surface, "Graphics_SetFramerate", "(I)V", [this](auto &c) {
            auto fps = static_cast<int>(c.arguments[0]);
            require(fps > 0 && fps <= 1000, "Invalid game frame rate");
            frame_rate_ = fps;
            return 0;
        });
        for (const auto *name : {"Graphics_GetScreenSizeInPixels", "Graphics_GetScreenSizeInPoints"})
            hook(surface, name, "([I)V", [this, &j](auto &c) {
                auto &bytes = j.get(c.arguments[0]).bytes;
                require(bytes.size() >= 8, "PVZ2 screen array is too small");
                std::memcpy(bytes.data(), &width_, 4);
                std::memcpy(bytes.data() + 4, &height_, 4);
                return 0;
            });
        hook(surface, "Graphics_GetSafeRect", "([I)V", [&j](auto &c) {
            auto &bytes = j.get(c.arguments[0]).bytes;
            require(bytes.size() >= 16, "PVZ2 safe rectangle array is too small");
            std::memset(bytes.data(), 0, 16);
            return 0;
        });
        for (const auto *name : {"Config_ConfigReadBoolean", "Config_ConfigReadInteger",
                                 "Config_ConfigReadString", "Config_ConfigKeyExists"}) {
            auto suffix = std::string_view(name) == "Config_ConfigReadString"    ? "Ljava/lang/String;"
                          : std::string_view(name) == "Config_ConfigReadInteger" ? "I"
                                                                                 : "Z";
            hook(activity, name, "(Ljava/lang/String;)" + std::string(suffix),
                 [this, &j, &r, name](auto &c) -> std::uint64_t {
                     std::lock_guard lock(r.state_mutex);
                     auto key = j.text(c.arguments[0]);
                     if (std::string_view(name) == "Config_ConfigKeyExists")
                         return preferences_.contains(key);
                     if (!preferences_.contains(key))
                         return 0;
                     const auto &value = preferences_[key];
                     if (std::string_view(name) == "Config_ConfigReadString")
                         return j.string(value.get<std::string>());
                     if (std::string_view(name) == "Config_ConfigReadBoolean")
                         return value.get<bool>();
                     return static_cast<std::uint64_t>(value.get<std::int64_t>());
                 });
        }
        for (const auto *type : {"Boolean", "Integer", "String"}) {
            auto sig = std::string("(Ljava/lang/String;") +
                       (std::string_view(type) == "Boolean"   ? "Z"
                        : std::string_view(type) == "Integer" ? "I"
                                                              : "Ljava/lang/String;") +
                       ")Z";
            hook(activity, "Config_ConfigWrite" + std::string(type), sig, [this, &j, &r, type](auto &c) {
                std::lock_guard lock(r.state_mutex);
                auto key = j.text(c.arguments[0]);
                if (std::string_view(type) == "String")
                    preferences_[key] = j.text(c.arguments[1]);
                else if (std::string_view(type) == "Boolean")
                    preferences_[key] = c.arguments[1] != 0;
                else
                    preferences_[key] = static_cast<std::int32_t>(c.arguments[1]);
                write_json(preferences_file_, preferences_);
                return 1;
            });
        }
        hook(activity, "Config_ConfigEraseKey", "(Ljava/lang/String;)V", [this, &j, &r](auto &c) {
            std::lock_guard lock(r.state_mutex);
            preferences_.erase(j.text(c.arguments[0]));
            write_json(preferences_file_, preferences_);
            return 0;
        });
        for (const auto *name : {"Resources_GetFileSystemBlockCount", "Resources_GetFileSystemBlocksFree",
                                 "Resources_GetFileSystemBlockSize"})
            hook(activity, name, "(Ljava/lang/String;)J", [&r, name](auto &) {
                auto space = fs::space(r.options.data);
                return std::string_view(name) == "Resources_GetFileSystemBlockSize" ? 4096ull
                       : std::string_view(name) == "Resources_GetFileSystemBlocksFree"
                           ? space.available / 4096
                           : space.capacity / 4096;
            });
    }
    void start(android::Runtime &r) override {
        r.phase = "game-initialize";
        auto [drawable_width, drawable_height] = r.host->drawable_size();
        if (!options_.render_width) {
            width_ = drawable_width;
            height_ = drawable_height;
        }
        r.graphics->set_guest_size(width_, height_);
        r.graphics->resize(drawable_width, drawable_height);
        viewport_ = host::fit_viewport(drawable_width, drawable_height, width_, height_);
        const std::string signature =
            "(Lcom/popcap/SexyAppFramework/AndroidSurfaceView;Lcom/popcap/SexyAppFramework/"
            "AndroidHttpProxy;Lcom/popcap/SexyAppFramework/cloud/Cloud;Lcom/popcap/SexyAppFramework/"
            "GooglePlay/GooglePlayConnect;Lcom/popcap/SexyAppFramework/GooglePlay/"
            "GooglePlayAchievements;Lcom/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard;Lcom/"
            "popcap/SexyAppFramework/AndroidNotification;Lcom/popcap/SexyAppFramework/"
            "SexyAppFrameworkActivity;)Z";
        // The original CreateGameApp Java caller ignores the native boolean result.
        call(r, activity, "Native_GameAppInitialize", signature,
             {surface_, http_proxy_, r.jni->object("com/popcap/SexyAppFramework/cloud/Cloud"),
              r.jni->object("com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect"),
              r.jni->object("com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements"),
              r.jni->object("com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard"),
              r.jni->object("com/popcap/SexyAppFramework/AndroidNotification"), activity_});
        call(r, activity, "Native_applicationWillFinishLaunching", "()V");
        call(r, activity, "Native_applicationDidFinishLaunching", "()V");
        call(r, surface, "Native_onSurfaceCreated", "()V");
        call(r, surface, "Native_onSurfaceChanged", "(II)V",
             {static_cast<std::uint64_t>(width_), static_cast<std::uint64_t>(height_)});
        call(r, activity, "Native_applicationDidBecomeActive", "()V");
        audio_focus(r, true);
        auto network_status = target(r, "com/popcap/SexyAppFramework/AndroidHttpProxy",
                                     "Native_SetCurrentNetworkStatus", "(I)V");
        r.jni->call_native(r.cpu, network_status,
                           std::array{r.jni->environment(), http_proxy_, Address{0}});
        r.phase = "running";
    }
    void frame(android::Runtime &r) override {
        // AndroidRenderer sleeps to its requested frame rate and clears before native drawing.
        auto fps = options_.fps_limit ? options_.fps_limit : frame_rate_.load();
        auto interval = std::chrono::nanoseconds(1'000'000'000 / fps);
        if (previous_frame_ != std::chrono::steady_clock::time_point{})
            std::this_thread::sleep_until(previous_frame_ + interval);
        previous_frame_ = std::chrono::steady_clock::now();
        r.cpu.call(r.linker.symbol("glClear"), std::array<Address, 1>{0x4000});
        call(r, surface, "Native_onDrawFrame", "()V");
    }
    void event(android::Runtime &r, const host::Event &event) override {
        if (event.type == host::EventType::Focus && audio_focused_ == event.focused)
            return;
        if (event.type == host::EventType::Resize && event.width > 0 && event.height > 0) {
            // The native game caches its intermediate render targets at startup.
            // Keep that surface stable until relaunch; only presentation changes here.
            if (down_)
                input_.push({host::EventType::Focus, 0, 0, 0, 0, false});
            down_ = false;
            viewport_ = host::fit_viewport(event.width, event.height, width_, height_);
            r.graphics->resize(event.width, event.height);
        }
        auto mapped = event;
        if (event.type == host::EventType::PointerDown || event.type == host::EventType::PointerMove ||
            event.type == host::EventType::PointerUp) {
            if ((event.type == host::EventType::PointerDown && !viewport_.contains(event.x, event.y)) ||
                (event.type != host::EventType::PointerDown && !down_))
                return;
            mapped.x = std::clamp(viewport_.guest_x(event.x) * width_, 0.f, float(width_ - 1));
            mapped.y = std::clamp(viewport_.guest_y(event.y) * height_, 0.f, float(height_ - 1));
        }
        input_.push(mapped);
        if (event.type == host::EventType::Focus) {
            if (!event.focused)
                down_ = false;
            glu_lifecycle_(event.focused);
            {
                std::lock_guard lock(r.state_mutex);
                r.jni->get(activity_).fields["host_active"] = event.focused;
            }
            audio_focus(r, event.focused);
        }
        if (event.type == host::EventType::PointerDown)
            down_ = true;
        if (event.type == host::EventType::PointerUp)
            down_ = false;
    }
    void stop(android::Runtime &r) override {
        glu_lifecycle_(false);
        r.wait_callbacks();
        {
            std::lock_guard lock(r.state_mutex);
            r.jni->get(activity_).fields["host_active"] = false;
            r.jni->get(activity_).fields["host_running"] = false;
        }
        if (r.phase == "running") {
            audio_focus(r, false);
            call(r, activity, "Native_applicationWillResignActive", "()V");
            call(r, activity, "Native_applicationDidEnterBackground", "()V");
        }
        std::lock_guard lock(r.state_mutex);
        write_json(preferences_file_, preferences_);
    }
};
} // namespace
std::unique_ptr<android::GameAdapter> pvz2(Pvz2Options options) {
    return std::make_unique<Pvz2>(std::move(options));
}
} // namespace acp::games

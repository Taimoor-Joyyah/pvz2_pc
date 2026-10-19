#include <acp/pvz2.hpp>

#include <iostream>
namespace {
void persistence_test(const acp::fs::path &root, bool writing) {
    using namespace acp;
    android::Runtime runtime({"com.ea.game.pvz2_row", "13.5.1", "en_US", 1063, root, {}, {}});
    auto adapter = games::pvz2();
    adapter->configure(runtime, {});
    auto &j = *runtime.jni;
    auto &m = runtime.memory;
    auto env = j.environment(), table = m.load<Address>(env);
    auto invoke = [&](size_t slot, std::initializer_list<Address> rest) {
        std::vector<Address> args{env};
        args.insert(args.end(), rest);
        return runtime.cpu.call(m.load<Address>(table + slot * 8), args);
    };
    auto method = [&](Address type, std::string_view name, std::string_view signature,
                      bool is_static = false) {
        return invoke(is_static ? 113 : 33, {type, m.string(name), m.string(signature)});
    };
    auto storage = j.class_ref("com/ea/nimble/Persistence$Storage");
    auto document = invoke(145, {storage, invoke(144, {storage, m.string("DOCUMENT"),
                                                       m.string("Lcom/ea/nimble/Persistence$Storage;")})});
    auto service = j.class_ref("com/ea/nimble/PersistenceService");
    auto store = invoke(
        114,
        {service,
         method(service, "getPersistenceForNimbleComponent",
                "(Ljava/lang/String;Lcom/ea/nimble/Persistence$Storage;)Lcom/ea/nimble/Persistence;", true),
         j.string("../../test-component"), document});
    auto type = j.class_ref("com/ea/nimble/Persistence");
    auto key = j.string("setting");
    auto set = method(type, "setValue", "(Ljava/lang/String;Ljava/io/Serializable;)V");
    auto get = method(type, "getValue", "(Ljava/lang/String;)Ljava/io/Serializable;");
    auto long_type = j.class_ref("java/lang/Long");
    if (writing) {
        auto boxed = invoke(114, {long_type, method(long_type, "valueOf", "(J)Ljava/lang/Long;", true),
                                  0x7ffffffffffffffeull});
        require(invoke(32, {boxed, j.class_ref("java/io/Serializable")}) == 1,
                "Boxed values implement Serializable");
        invoke(61, {store, set, key, boxed});
        invoke(61, {store, method(type, "setEncryption", "(Z)V"), 1});
        invoke(61, {store, set, j.string("text"), j.string("SDK settings persist")});
    }
    auto restored = invoke(34, {store, get, key});
    require(invoke(52, {restored, method(long_type, "longValue", "()J")}) == 0x7ffffffffffffffeull,
            "Persistence preserves 64-bit integers across encrypted restart");
    require(invoke(37, {store, method(type, "getEncryption", "()Z")}) == 1,
            "Encryption flag survives restart");
    auto text = invoke(34, {store, method(type, "getStringValue", "(Ljava/lang/String;)Ljava/lang/String;"),
                            j.string("text")});
    require(j.text(text) == "SDK settings persist", "Persistence string survives restart");
    require(invoke(34, {store, method(type, "getStringValue", "(Ljava/lang/String;)Ljava/lang/String;"),
                        key}) == 0,
            "Wrong persistence value type returns null");
    if (!writing) {
        invoke(61, {store, set, key, 0});
        require(invoke(37, {store, method(type, "hasKey", "(Ljava/lang/String;)Z"), key}) == 0,
                "Null persistence value removes key");
    }
    auto array = j.object("[Ljava/lang/String;");
    j.get(array).elements = {j.global_ref(j.string("array")), 0};
    auto decoded = j.deserialize(j.serialize(array));
    require(j.text(j.get(decoded).elements[0]) == "array" && !j.get(decoded).elements[1],
            "Serializable object arrays retain nulls");
    require(invoke(32, {array, j.class_ref("[Ljava/lang/Object;")}) == 1 &&
                invoke(32, {j.object("[I"), j.class_ref("[Ljava/lang/Object;")}) == 0,
            "Java array covariance distinguishes primitive arrays");
}
} // namespace
int main() {
    try {
        using namespace acp;
        games::Pvz2Input input;
        auto before_touch =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        input.push({host::EventType::PointerDown, 100, 200});
        auto after_touch =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        input.push({host::EventType::PointerMove, 150, 250});
        input.push({host::EventType::PointerUp, 150, 250});
        Bytes buffer(16 + 48);
        std::fill(buffer.begin() + 4, buffer.begin() + 16, std::byte{0xa5});
        require(input.drain(buffer), "Partial input drain must report remaining events");
        BinaryView first(buffer);
        require(first.get<unsigned>(0) == 1 && first.get<unsigned>(16) == 0, "Touch header and count");
        require(first.get<unsigned>(20) == 1,
                "The primary desktop touch uses Android pointer zero encoded as nonzero native ID one");
        require(first.get<unsigned>(4) == 0xa5a5a5a5 && first.get<unsigned>(12) == 0xa5a5a5a5,
                "Only the event count is written in the native-owned header, matching Java ProcessEvents");
        require(first.get<int>(24) == 100 && first.get<int>(28) == 200, "Touch coordinates");
        require(first.get<double>(44) >= before_touch && first.get<double>(44) <= after_touch,
                "Touch timestamps use monotonic seconds, matching Java nanoTime divided by 1e9");
        require(first.get<unsigned>(52) == 0, "Touch began phase");
        require(first.get<unsigned>(56) == 0xdeadbeef, "Touch framing sentinel");
        Bytes rest(256);
        require(!input.drain(rest), "Remaining touch events should drain");
        BinaryView second(rest);
        require(second.get<unsigned>(0) == 2 && second.get<unsigned>(52) == 1 &&
                    second.get<unsigned>(100) == 3,
                "Move and release phases");
        input.push({host::EventType::Back});
        input.push({host::EventType::Text, 0, 0, 0, 0, false, "abc"});
        require(!input.drain(rest), "Back and text drain");
        BinaryView text(rest);
        require(text.get<unsigned>(16) == 5 && text.get<unsigned>(32) == 6 && text.get<unsigned>(40) == 3,
                "Back and text wire formats");
        require(text.get<std::uint8_t>(44) == 'a' && text.get<std::uint8_t>(47) == 0, "Text padding");
        games::Pvz2Options input_options;
        input_options.render_width = 1280;
        input_options.render_height = 720;
        auto adapter = games::pvz2(input_options);
        auto root = fs::current_path() / "test-artifacts" / path_from_utf8("pvz2-" + timestamp());
        android::Runtime runtime({"com.ea.game.pvz2_row", "13.5.1", "en_US", 1063, root, {}, {}});
        adapter->configure(runtime, {});
        auto &jni = *runtime.jni;
        auto &memory = runtime.memory;
        auto env = jni.environment(), table = memory.load<Address>(env);
        auto invoke = [&](size_t index, std::initializer_list<Address> rest) {
            std::vector<Address> args{env};
            args.insert(args.end(), rest);
            return runtime.cpu.call(memory.load<Address>(table + index * 8), args);
        };
        auto surface_type = jni.class_ref("com/popcap/SexyAppFramework/AndroidSurfaceView");
        auto surface = jni.object("com/popcap/SexyAppFramework/AndroidSurfaceView");
        auto can_scale = invoke(
            33, {surface_type, memory.string("Graphics_CanSetGLViewScaleFactor"), memory.string("()Z")});
        auto get_scale = invoke(
            33, {surface_type, memory.string("Graphics_GetGLViewScaleFactor"), memory.string("()F")});
        auto set_scale = invoke(
            33, {surface_type, memory.string("Graphics_SetGLViewScaleFactor"), memory.string("(F)V")});
        require(invoke(37, {surface, can_scale}) == 1 &&
                    invoke(55, {surface, get_scale}) == std::bit_cast<unsigned>(0.5f),
                "The original Android surface starts with a mutable 0.5 GL view scale");
        auto scale_argument = memory.allocate(8);
        memory.store(scale_argument, Address(std::bit_cast<unsigned>(1.2808344f)));
        invoke(63, {surface, set_scale, scale_argument});
        require(invoke(55, {surface, get_scale}) == std::bit_cast<unsigned>(1.2808344f),
                "The native renderer can read back its scale through the JNI float ABI");
        auto type = jni.class_ref("com/popcap/SexyAppFramework/SexyAppFrameworkActivity");
        auto instance = jni.object("com/popcap/SexyAppFramework/SexyAppFrameworkActivity");
        auto ui_method = invoke(
            33, {type, memory.string("UI_ProcessEvents"), memory.string("(Ljava/nio/ByteBuffer;)Z")});
        auto ui_bytes = memory.allocate(512);
        auto ui_buffer = jni.object("java/nio/ByteBuffer");
        jni.get(ui_buffer).fields = {{"address", ui_bytes}, {"capacity", 512}};
        auto drain_ui = [&] { invoke(37, {instance, ui_method, ui_buffer}); };
        std::vector<bool> audio_focus;
        unsigned activity_restarts{};
        for (auto focused : {false, true}) {
            auto callback = runtime.traps.add("test/audio-focus",
                                              [&, focused](auto &) { audio_focus.push_back(focused); });
            jni.register_native("com/popcap/SexyAppFramework/SexyAppFrameworkActivity",
                                focused ? "Native_NotifyAudioGainedFocus" : "Native_NotifyAudioLostFocus",
                                "()V", callback);
        }
        for (auto name : {"Native_applicationDidBecomeActive", "Native_applicationWillResignActive"})
            jni.register_native(
                "com/popcap/SexyAppFramework/SexyAppFrameworkActivity", name, "()V",
                runtime.traps.add("test/activity-restart", [&](auto &) { ++activity_restarts; }));
        for (auto focused : {true, true, false, false, true})
            adapter->event(runtime, {host::EventType::Focus, 0, 0, 0, 0, focused});
        require(audio_focus == std::vector<bool>{true, false, true} && activity_restarts == 0,
                "Window focus changes notify audio once without restarting the active application");
        adapter->event(runtime, {host::EventType::Resize, 0, 0, 720, 1280});
        adapter->event(runtime, {host::EventType::PointerDown, 360, 100});
        adapter->event(runtime, {host::EventType::PointerUp, 360, 100});
        drain_ui();
        require(memory.load<unsigned>(ui_bytes) == 0, "Letterbox clicks must not reach the game");
        adapter->event(runtime, {host::EventType::PointerDown, 360, 639.5f});
        adapter->event(runtime, {host::EventType::PointerMove, 900, 1400});
        adapter->event(runtime, {host::EventType::Resize, 0, 0, 1280, 720});
        adapter->event(runtime, {host::EventType::PointerUp, 900, 700});
        drain_ui();
        auto mapped_ui = memory.readable(ui_bytes, 512);
        BinaryView mapped_events(mapped_ui);
        require(mapped_events.get<unsigned>(0) == 3 && mapped_events.get<int>(24) == 640 &&
                    mapped_events.get<int>(28) == 360 && mapped_events.get<int>(72) == 1279 &&
                    mapped_events.get<int>(76) == 719 && mapped_events.get<unsigned>(148) == 4,
                "Resized input maps to guest pixels, clamps drags, and cancels a touch on resize");
        runtime.options.application_metadata = {{"present", true}};
        auto config = jni.class_ref("com/ea/nimble/NimbleApplicationConfiguration");
        auto config_default = invoke(
            113, {config, memory.string("getConfigValueAsInt"), memory.string("(Ljava/lang/String;I)I")});
        require(invoke(129, {config, config_default, jni.string("absent"), 42}) == 42,
                "Missing configuration keys preserve the supplied default in a nonempty manifest");
        auto purchases = jni.class_ref("com/ea/nimble/mtx/NimbleMTX");
        auto purchase_component = invoke(113, {purchases, memory.string("getComponent"),
                                               memory.string("()Lcom/ea/nimble/mtx/INimbleMTX;")});
        auto billing = invoke(114, {purchases, purchase_component});
        auto billing_interface = jni.class_ref("com/ea/nimble/mtx/INimbleMTX");
        auto catalog = invoke(33, {billing_interface, memory.string("getAvailableCatalogItems"),
                                   memory.string("()Ljava/util/List;")});
        require(billing != 0 && invoke(34, {billing, catalog}) == 0,
                "Unavailable billing cannot expose a fabricated product catalog");
        unsigned failed_catalogs{};
        runtime.linker.provide(
            "Java_com_ea_nimble_bridge_BaseNativeCallback_nativeCallback", [&](auto &cpu) {
                auto payload = jni.get(jni.get(cpu.x(3)).elements.at(1)).elements;
                require(cpu.x(2) == 7 && jni.text(payload[0]) == "result" && jni.text(payload[1]) == "0" &&
                            jni.text(payload[2]) == "error" &&
                            jni.get(payload[3]).fields.at("com/ea/nimble/Error#m_codeI") == 103,
                        "Catalog refresh reports the SDK's NOT_AVAILABLE error without granting products");
                ++failed_catalogs;
                cpu.x(0, 0);
            });
        auto receiver = jni.object("com/ea/nimble/bridge/BaseNativeCallback");
        jni.get(receiver).fields["com/ea/nimble/bridge/BaseNativeCallback#m_idI"] = 7;
        runtime.broadcast_receivers.emplace("nimble.notification.mtx.refreshcatalogfinished",
                                            jni.global_ref(receiver));
        auto refresh = invoke(
            33, {billing_interface, memory.string("refreshAvailableCatalogItems"), memory.string("()V")});
        invoke(61, {billing, refresh});
        runtime.wait_callbacks();
        require(failed_catalogs == 1, "Unavailable catalog refresh completes asynchronously");
        auto analytics_factory_type = jni.class_ref("com/glu/plugins/gluanalytics/AnalyticsFactory");
        auto analytics_factory = jni.object("com/glu/plugins/gluanalytics/AnalyticsFactory");
        auto create_analytics = invoke(
            33,
            {analytics_factory_type, memory.string("createAnalytics"),
             memory.string("(Ljava/lang/String;Ljava/util/Map;)Lcom/glu/plugins/gluanalytics/Analytics;")});
        auto analytics = invoke(
            34, {analytics_factory, create_analytics, jni.string("{}"), jni.object("java/util/HashMap")});
        auto analytics_type = jni.class_ref("com/glu/plugins/gluanalytics/Analytics");
        auto custom_properties = jni.object("java/util/HashMap");
        auto disabled_setting = jni.object("java/lang/Boolean");
        jni.get(disabled_setting).fields = {{"kind", "Z"}, {"bits", 0}};
        jni.get(custom_properties).elements = {jni.global_ref(jni.string("usageSharing")),
                                               jni.global_ref(disabled_setting)};
        auto set_properties = invoke(33, {analytics_type, memory.string("setCustomProperties"),
                                          memory.string("(Ljava/util/Map;)V")});
        auto get_properties = invoke(
            33, {analytics_type, memory.string("getCustomProperties"), memory.string("()Ljava/util/Map;")});
        invoke(61, {analytics, set_properties, custom_properties});
        jni.delete_ref(jni.get(custom_properties).elements[1]);
        jni.get(custom_properties).elements[1] = jni.global_ref(jni.string("changed"));
        auto local_properties = invoke(34, {analytics, get_properties});
        require(jni.get(local_properties).elements.size() == 4 &&
                    jni.get(jni.get(local_properties).elements.at(1)).fields.at("bits") == 0 &&
                    jni.get(jni.get(local_properties).elements.at(3)).fields.at("bits") == 1,
                "Local SDK properties survive input-map changes without sending analytics");
        auto size_method = invoke(33, {type, memory.string("Resources_GetAssetFileSize"),
                                       memory.string("(Ljava/lang/String;)J")});
        require(invoke(52, {instance, size_method, jni.string("ASSET:main.pak")}) == UINT64_MAX,
                "Missing assets return the Java -1 sentinel, not an empty file size");
        auto network_class = jni.class_ref("com/ea/nimble/Network");
        auto network_get = invoke(113, {network_class, memory.string("getComponent"),
                                        memory.string("()Lcom/ea/nimble/INetwork;")});
        auto network = invoke(114, {network_class, network_get});
        auto network_interface = jni.class_ref("com/ea/nimble/INetwork");
        auto get_status = invoke(33, {network_interface, memory.string("getStatus"),
                                      memory.string("()Lcom/ea/nimble/Network$Status;")});
        auto status = invoke(34, {network, get_status});
        auto status_class = jni.class_ref("com/ea/nimble/Network$Status");
        auto ordinal = invoke(33, {status_class, memory.string("ordinal"), memory.string("()I")});
        require(invoke(49, {status, ordinal}) == 1, "Offline network status is the APK's NONE enum value");
        auto ads_type = jni.class_ref("csdk/gluads/GluAdsNativeBridge");
        auto ads_constructor =
            invoke(33, {ads_type, memory.string("<init>"), memory.string("(JLjava/lang/String;)V")});
        auto ads_bridge = invoke(28, {ads_type, ads_constructor, 0x404040, jni.string("8.3.2-7c6263b")});
        auto create_ads =
            invoke(33, {ads_type, memory.string("createGluAds"),
                        memory.string("(Ljava/lang/String;Ljava/util/Map;)Lcsdk/gluads/IAdvertising;")});
        auto advertising =
            invoke(34, {ads_bridge, create_ads, jni.string("{}"), jni.object("java/util/HashMap")});
        auto listen = invoke(33, {ads_type, memory.string("setAdvertisingListener"), memory.string("()V")});
        invoke(61, {ads_bridge, listen});
        auto advertising_type = jni.class_ref("csdk/gluads/IAdvertising");
        auto loaded = invoke(33, {advertising_type, memory.string("isLoaded"),
                                  memory.string("(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)Z")});
        require(invoke(37, {advertising, loaded, jni.string("rewarded"), jni.string("test"), 0}) == 0,
                "Disabled advertising never reports an available ad");
        auto reward =
            invoke(33, {advertising_type, memory.string("getReward"),
                        memory.string("(Ljava/lang/String;Ljava/lang/String;)Lcsdk/gluads/Reward;")});
        require(invoke(34, {advertising, reward, jni.string("rewarded"), jni.string("test")}) == 0,
                "Unavailable ads cannot fabricate a reward");
        unsigned unavailable_ads = 0;
        runtime.linker.provide("Java_csdk_gluads_GluAdsNativeBridge_onPlacementEvent", [&](auto &cpu) {
            require(
                cpu.x(2) == 0x404040 && jni.text(cpu.x(5)) == "loadFinished" &&
                    jni.text(cpu.x(6)).find("offline") != std::string::npos && cpu.argument(8) == 0,
                "Unavailable-ad callback preserves its native context, failure status and stack argument");
            ++unavailable_ads;
            cpu.x(0, 0);
        });
        auto load_ad =
            invoke(33, {advertising_type, memory.string("load"),
                        memory.string("(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V")});
        invoke(61, {advertising, load_ad, jni.string("rewarded"), jni.string("test"), 0});
        require(unavailable_ads == 1, "Unavailable ads complete with a failure callback");
        persistence_test(root / "persistence", true);
        persistence_test(root / "persistence", false);
        auto kill = invoke(33, {type, memory.string("Device_KillApplication"), memory.string("()V")});
        require(!adapter->quit_requested(), "Adapters begin without an exit request");
        invoke(61, {instance, kill});
        require(adapter->quit_requested(), "Android application exit requests an orderly host shutdown");
        std::cout << "PVZ2 input, resource and persistence contract checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

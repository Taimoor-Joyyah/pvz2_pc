#include <acp/pvz2.hpp>
namespace acp::games {
namespace {
constexpr auto bridge = "csdk/gluads/GluAdsNativeBridge";
constexpr auto disabled = "csdk/gluads/impl/DummyAdvertising";
constexpr std::pair<const char *, const char *> disabled_methods[] = {
    {"destroy", "()V"},
    {"discard", "(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V"},
    {"internal_onNewSession", "()V"},
    {"internal_setGemsData", "(Ljava/lang/String;Ljava/util/Map;)V"},
    {"internal_updateAdsFrequency", "(Ljava/util/Map;)V"},
    {"internal_updateCrossPromotion", "(Ljava/lang/String;Ljava/util/Map;Ljava/util/Map;)V"},
    {"internal_updatePrivacyStatus", "(Lcsdk/gluads/PrivacyStatus;)V"},
    {"internal_updateRewardList", "(Ljava/util/Map;)V"},
    {"onAdvertisementClick", "(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V"},
    {"onAdvertisementImpression", "(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V"},
    {"onPause", "()V"},
    {"onResume", "()V"},
    {"setCustomProperties", "(Ljava/util/Map;)V"},
    {"setUserIdentifier", "(Ljava/lang/String;Ljava/util/Map;)V"},
};
} // namespace
void install_pvz2_ads(android::Runtime &r) {
    auto &j = *r.jni;
    auto native_data = std::make_shared<Address>();
    j.set_interfaces(disabled, {"csdk/gluads/IAdvertising"});
    j.hook(bridge, "<init>", "(JLjava/lang/String;)V", [&j, &r, native_data](auto &c) {
        std::lock_guard lock(r.state_mutex);
        require(j.text(c.arguments[1]) == "8.3.2-7c6263b",
                "Unsupported Glu ads bridge version; adapter bindings require review");
        *native_data = c.arguments[0];
        j.field(bridge, "mNativeData", "J", *native_data);
        return 0;
    });
    j.hook(
        bridge, "createGluAds", "(Ljava/lang/String;Ljava/util/Map;)Lcsdk/gluads/IAdvertising;",
        [&j, &r](auto &c) {
            std::lock_guard lock(r.state_mutex);
            auto advertising = j.object(disabled);
            j.get(c.receiver).fields["advertising"] = j.global_ref(advertising);
            r.message(
                "[Glu] Advertising unavailable in offline mode; using the SDK's DummyAdvertising behavior");
            return advertising;
        });
    j.hook(bridge, "setAdvertisingListener", "()V", [&j, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        auto advertising = j.get(c.receiver).fields.at("advertising").template get<Address>();
        auto &fields = j.get(advertising).fields;
        j.delete_ref(fields.value("listener", Address{}));
        fields["listener"] = j.global_ref(c.receiver);
        return 0;
    });
    j.hook(bridge, "getNativeData", "()J", [&r, native_data](auto &) {
        std::lock_guard lock(r.state_mutex);
        return *native_data;
    });
    j.hook(bridge, "destroy", "()V", [&j, &r, native_data](auto &c) {
        Address data;
        {
            std::lock_guard lock(r.state_mutex);
            data = std::exchange(*native_data, 0);
            j.field(bridge, "mNativeData", "J", 0);
        }
        if (data) {
            auto destroy = j.native(bridge, "destroy", "(J)V");
            require(destroy != 0, "Glu ads cleanup callback binding is missing");
            j.call_native(c.cpu, destroy, std::array{j.environment(c.cpu), j.class_ref(bridge), data});
        }
        return 0;
    });
    for (const auto *type : {disabled, "csdk/gluads/IAdvertising"}) {
        for (const auto &[name, signature] : disabled_methods)
            j.hook(type, name, signature, [](auto &) { return 0; });
        j.hook(type, "isLoaded", "(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)Z",
               [](auto &) { return false; });
        j.hook(type, "getReward", "(Ljava/lang/String;Ljava/lang/String;)Lcsdk/gluads/Reward;",
               [](auto &) { return 0; });
        j.hook(type, "setAdvertisingListener", "(Lcsdk/gluads/IAdvertisingListener;)V", [&j, &r](auto &c) {
            std::lock_guard lock(r.state_mutex);
            auto &fields = j.get(c.receiver).fields;
            j.delete_ref(fields.value("listener", Address{}));
            fields["listener"] = j.global_ref(c.arguments[0]);
            return 0;
        });
        for (bool showing : {false, true})
            j.hook(type, showing ? "show" : "load",
                   "(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V",
                   [&j, &r, native_data, showing](auto &c) {
                       Address data;
                       {
                           std::lock_guard lock(r.state_mutex);
                           if (!j.get(c.receiver).fields.value("listener", Address{}))
                               return 0;
                           data = *native_data;
                       }
                       if (!data)
                           return 0;
                       auto callback = j.native(bridge, "onPlacementEvent",
                                                "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/"
                                                "String;Ljava/lang/String;Ljava/util/Map;J)V");
                       require(callback != 0, "Glu unavailable-ad callback binding is missing");
                       j.call_native(c.cpu, callback,
                                     std::array{j.environment(c.cpu), j.class_ref(bridge), data,
                                                c.arguments[0], c.arguments[1],
                                                j.string(showing ? "showFinished" : "loadFinished"),
                                                j.string("Advertising unavailable in offline mode"),
                                                c.arguments[2], Address{0}});
                       return 0;
                   });
    }
}
} // namespace acp::games

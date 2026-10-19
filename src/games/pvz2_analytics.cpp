#include <acp/pvz2.hpp>

namespace acp::games {
namespace {
constexpr auto analytics = "com/glu/plugins/gluanalytics/Analytics";
// Public event/control methods on the SDK's empty-backend (disabled) Analytics object.
constexpr std::pair<const char *, const char *> disabled_methods[] = {
    {"destroy", "()V"},
    {"init", "(Lcom/glu/plugins/gluanalytics/IAnalyticsCallback;)V"},
    {"internal_firebaseAllowPersonalizedAds", "(Z)V"},
    {"internal_firebaseInitialize", "(Ljava/util/Map;Lcom/glu/plugins/gluanalytics/IAnalyticsCallback;)V"},
    {"internal_firebaseUpdateUnderAgeUserStatus", "(Z)V"},
    {"internal_logCpuEvent", "(Ljava/util/Map;)V"},
    {"internal_logEvent", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/"
                          "lang/Long;Ljava/lang/Long;Ljava/util/Map;)V"},
    {"internal_logEvent_adImpression", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
                                       "String;Ljava/math/BigDecimal;Ljava/lang/Long;Ljava/util/Map;)V"},
    {"internal_logFirebaseEvent", "(Ljava/lang/String;Ljava/util/Map;)V"},
    {"internal_perfEvent", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/"
                           "lang/String;Ljava/util/Map;)V"},
    {"internal_pinpointAttribute",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V"},
    {"internal_setRevID", "(Ljava/lang/String;Ljava/lang/String;Z)V"},
    {"internal_trackRevenueInUsd",
     "(DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V"},
    {"internal_updatePSValue", "(Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Ljava/lang/"
                               "String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"},
    {"logAdvertisementOpportunity", "(Ljava/lang/String;Ljava/lang/String;JZLjava/util/Map;)V"},
    {"logCurrencySink", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/"
                        "lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logCurrencySource", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/"
                          "lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logEvent", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
                 "Long;Ljava/lang/Long;Ljava/util/Map;)V"},
    {"logInAppPurchaseInCents",
     "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logInAppPurchaseInUsd",
     "(DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logMissionCompleted", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logMissionFailed",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logMissionStarted", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V"},
    {"logTutorialStepCompleted", "(Ljava/lang/String;JLjava/lang/String;JLjava/util/Map;)V"},
    {"onPause", "()V"},
    {"onResume", "()V"},
    {"perfSampleEnd", "(Ljava/lang/String;Ljava/util/Map;)V"},
    {"perfSampleStart", "(Ljava/lang/String;ILjava/util/Map;)V"},
    {"setCustomProperties", "(Ljava/util/Map;)V"},
    {"setEATargetedMarketing", "(Z)V"},
    {"setUserIdentifier", "(Ljava/lang/String;)V"},
    {"shortcircuitEATargetedMarketingOff", "()V"},
};
} // namespace
void install_pvz2_analytics(android::Runtime &r) {
    auto &j = *r.jni;
    auto group = std::string("glu-analytics");
    auto property_file =
        r.options.data / "preferences/pc-runtime-glu" / (sha256(std::as_bytes(std::span(group))) + ".json");
    auto properties =
        std::make_shared<Json>(fs::exists(property_file) ? read_json(property_file) : Json::object());
    require(properties->is_object(), "Invalid local analytics preferences");
    j.set_interfaces(analytics, {"com/glu/plugins/gluanalytics/IAnalytics",
                                 "com/glu/plugins/gluanalytics/IAnalyticsParametersGetter"});
    j.hook("com/glu/plugins/gluanalytics/AnalyticsFactory", "<init>", "(Landroid/content/Context;)V",
           [](auto &) { return 0; });
    j.hook("com/glu/plugins/gluanalytics/AnalyticsFactory", "createAnalytics",
           "(Ljava/lang/String;Ljava/util/Map;)Lcom/glu/plugins/gluanalytics/Analytics;",
           [&j, &r](auto &c) {
               require(c.arguments[0] != 0, "Glu analytics configuration is null");
               r.message(
                   "[Glu] Analytics delivery unavailable in offline mode; retaining local SDK properties");
               auto result = j.object(analytics);
               j.get(result).fields["shortcircuit_marketing"] = false;
               return result;
           });
    for (const auto *type : {analytics, "com/glu/plugins/gluanalytics/IAnalytics"}) {
        for (const auto &[name, signature] : disabled_methods)
            j.hook(type, name, signature, [](auto &) { return 0; });
        for (const auto *name : {"getAnalyticsApplicationName", "getAnalyticsDeviceIdentifier",
                                 "getAnalyticsEnvironment", "getRevenueIdentifier", "getSessionIdentifier"})
            j.hook(type, name, "()Ljava/lang/String;", [&j](auto &) { return j.string(""); });
        j.hook(type, "setCustomProperties", "(Ljava/util/Map;)V",
               [&j, &r, properties, property_file](auto &c) {
                   std::lock_guard lock(r.state_mutex);
                   if (!c.arguments[0])
                       return 0;
                   auto incoming = j.get(c.arguments[0]).elements;
                   for (size_t i = 0; i < incoming.size(); i += 2) {
                       auto key = j.text(incoming[i]);
                       if (key != "usageSharing" && key != "internalTargetedAds")
                           continue;
                       auto &value = j.get(incoming[i + 1]);
                       require(value.class_name == "java/lang/Boolean",
                               "Analytics settings require Boolean values");
                       (*properties)[key] = value.fields.at("bits").template get<std::uint64_t>() != 0;
                   }
                   write_json(property_file, *properties);
                   return 0;
               });
        j.hook(type, "getCustomProperties", "()Ljava/util/Map;", [&j, &r, properties](auto &c) {
            std::lock_guard lock(r.state_mutex);
            auto copy = j.object("java/util/HashMap");
            // GluAnalytics.getCustomProperties always includes both keys, with SDK defaults true.
            for (const auto *key : {"usageSharing", "internalTargetedAds"}) {
                bool enabled = properties->value(key, true);
                if (std::string_view(key) == "internalTargetedAds" &&
                    j.get(c.receiver).fields.value("shortcircuit_marketing", false))
                    enabled = false;
                auto value = j.object("java/lang/Boolean");
                j.get(value).fields = {{"kind", "Z"}, {"bits", enabled ? 1 : 0}};
                j.get(copy).elements.push_back(j.global_ref(j.string(key)));
                j.get(copy).elements.push_back(j.global_ref(value));
            }
            return copy;
        });
        j.hook(type, "setEATargetedMarketing", "(Z)V", [&r, properties, property_file](auto &c) {
            std::lock_guard lock(r.state_mutex);
            (*properties)["internalTargetedAds"] = c.arguments[0] != 0;
            write_json(property_file, *properties);
            return 0;
        });
        j.hook(type, "shortcircuitEATargetedMarketingOff", "()V", [&r, &j](auto &c) {
            std::lock_guard lock(r.state_mutex);
            j.get(c.receiver).fields["shortcircuit_marketing"] = true;
            return 0;
        });
        j.hook(type, "getGeoLocation", "()Lcom/glu/plugins/gluanalytics/GeoLocation;",
               [](auto &) { return 0; });
        j.hook(type, "perfSampleAction", "(Ljava/lang/String;I)Z", [](auto &) { return false; });
        j.hook(type, "perfGetValues", "()Lcom/glu/plugins/gluanalytics/PerfMeasure;",
               [](auto &) { return 0; });
    }
}
} // namespace acp::games

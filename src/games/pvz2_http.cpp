#include <acp/pvz2.hpp>
namespace acp::games {
namespace {
constexpr auto transaction = "com/popcap/SexyAppFramework/AndroidHttpTransaction";
constexpr auto native_field = "com/popcap/SexyAppFramework/AndroidHttpTransaction#mNativeTransactionJ";
} // namespace
void install_pvz2_http(android::Runtime &r) {
    auto &j = *r.jni;
    auto hook = [&j](const char *name, const char *signature, android::JavaHandler handler) {
        j.hook(transaction, name, signature, std::move(handler));
    };
    hook("<init>", "(JLjava/lang/String;Ljava/lang/String;)V", [&j, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        j.get(c.receiver).fields = {{native_field, c.arguments[0]},
                                    {"method", j.text(c.arguments[1])},
                                    {"url", j.text(c.arguments[2])},
                                    {"timeout", 30},
                                    {"started", false}};
        return 0;
    });
    hook("GetHumanReadableUrl", "()Ljava/lang/String;", [&j](auto &c) {
        return j.string(j.get(c.receiver).fields.at("url").template get<std::string>());
    });
    hook("GetStatusCode", "()I", [](auto &) { return 0; });
    hook("GetStatusLine", "()Ljava/lang/String;", [](auto &) { return 0; });
    hook("GetResponseLength", "()I", [](auto &) { return UINT64_MAX; });
    hook("GetResponseHeader", "(Ljava/lang/String;)Ljava/lang/String;", [](auto &) { return 0; });
    hook("SetRequestHeader", "(Ljava/lang/String;Ljava/lang/String;)V", [&j, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        auto &fields = j.get(c.receiver).fields;
        if (!fields.contains("headers"))
            fields["headers"] = Json::object();
        fields["headers"][j.text(c.arguments[0])] = j.text(c.arguments[1]);
        return 0;
    });
    hook("SetRequestBody", "([B)V", [&j, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        auto &fields = j.get(c.receiver).fields;
        j.delete_ref(fields.value("body", Address{}));
        fields["body"] = j.global_ref(c.arguments[0]);
        return 0;
    });
    hook("GetRequestBody", "()[B", [&j, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        return j.local_ref(j.get(c.receiver).fields.value("body", Address{}));
    });
    hook("SetTimeout", "(I)V", [&j, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        j.get(c.receiver).fields["timeout"] = c.arguments[0];
        return 0;
    });
    hook("SetBasicAuth", "(Ljava/lang/String;Ljava/lang/String;)V",
         [](auto &) { return 0; }); // No request is transmitted while offline.
    auto start = [&j, &r](android::JavaCall &c) {
        {
            std::lock_guard lock(r.state_mutex);
            auto &fields = j.get(c.receiver).fields;
            if (fields.at("started").get<bool>())
                return std::uint64_t{};
            fields["started"] = true;
        }
        auto receiver = j.global_ref(c.receiver);
        r.post_callback([&r, receiver](guest::Cpu &cpu) {
            auto &jni = *r.jni;
            Address data;
            {
                std::lock_guard lock(r.state_mutex);
                data = jni.get(receiver).fields.at(native_field).get<Address>();
            }
            if (data) {
                auto target = jni.native(transaction, "HttpTransactionError", "(J)V");
                require(target != 0, "PopCap HTTP failure callback binding is missing");
                jni.call_native(cpu, target, std::array{jni.environment(cpu), receiver, data});
            }
            jni.delete_ref(receiver);
        });
        return std::uint64_t{};
    };
    for (const auto *name : {"Start", "run"})
        j.hook_suppressed(transaction, name, "()V", "online-http",
                          "One asynchronous HttpTransactionError callback per transaction; no request sent", start);
    hook("Release", "()V", [&j, &r](auto &c) {
        Address data;
        {
            std::lock_guard lock(r.state_mutex);
            auto &fields = j.get(c.receiver).fields;
            data = fields.at(native_field).template get<Address>();
            fields[native_field] = 0;
            j.delete_ref(fields.value("body", Address{}));
            fields["body"] = 0;
        }
        if (data) {
            auto target = j.native(transaction, "HttpTransactionCleanup", "(J)V");
            require(target != 0, "PopCap HTTP cleanup callback binding is missing");
            j.call_native(c.cpu, target, std::array{j.environment(c.cpu), c.receiver, data});
        }
        return 0;
    });
}
} // namespace acp::games

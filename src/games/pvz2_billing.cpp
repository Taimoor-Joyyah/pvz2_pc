#include <acp/pvz2.hpp>

namespace acp::games {
namespace {
constexpr auto api = "com/ea/nimble/mtx/INimbleMTX";
Address unavailable(android::Jni &j) {
    auto value = j.object("com/ea/nimble/Error");
    j.get(value).fields = {
        {"com/ea/nimble/Error#m_domainLjava/lang/String;", j.global_ref(j.string("NimbleError"))},
        {"com/ea/nimble/Error#m_codeI", 103},
        {"message", "Billing is unavailable in the desktop runtime"},
        {"cause", 0}};
    return value;
}
} // namespace
Address install_pvz2_billing(android::Runtime &r, std::function<void(std::string, Address)> broadcast) {
    auto &j = *r.jni;
    auto hook = [&r, &j](const char *name, const char *signature, auto handler) {
        j.hook_suppressed(api, name, signature, "online-billing",
                         "Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants",
                         [&r, handler](android::JavaCall &call) -> Address {
            std::lock_guard lock(r.state_mutex);
            return handler(call);
        });
    };
    for (auto name : {"getPendingTransactions", "getPurchasedTransactions", "getRecoveredTransactions"})
        hook(name, "()Ljava/util/List;", [&j](auto &) { return j.object("java/util/ArrayList"); });
    hook("getAvailableCatalogItems", "()Ljava/util/List;", [](auto &) { return 0; });
    hook("getTransaction", "(Ljava/lang/String;)Lcom/ea/nimble/mtx/NimbleMTXTransaction;",
         [](auto &) { return 0; });
    hook("setPlatformParameters", "(Ljava/util/Map;)V", [](auto &) { return 0; });
    for (bool restore : {false, true})
        hook(restore ? "restorePurchasedTransactions" : "refreshAvailableCatalogItems", "()V",
             [&j, broadcast, restore](auto &) {
                 auto data = j.object("java/util/HashMap");
                 j.get(data).elements = {j.global_ref(j.string("result")), j.global_ref(j.string("0")),
                                         j.global_ref(j.string("error")), j.global_ref(unavailable(j))};
                 broadcast(restore ? "nimble.notification.mtx.restorepurchasedtransactionsfinished"
                                   : "nimble.notification.mtx.refreshcatalogfinished",
                           data);
                 return 0;
             });
    auto reject = [&j](auto &) { return unavailable(j); };
    hook("purchaseItem",
         "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Ljava/lang/"
         "String;)Lcom/ea/nimble/Error;",
         reject);
    hook("itemGranted",
         "(Ljava/lang/String;Lcom/ea/nimble/mtx/NimbleCatalogItem$ItemType;Lcom/ea/nimble/mtx/"
         "INimbleMTX$ItemGrantedCallback;)Lcom/ea/nimble/Error;",
         reject);
    hook("finalizeTransaction",
         "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/"
         "Error;",
         reject);
    hook("resumeTransaction",
         "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Lcom/ea/nimble/"
         "mtx/INimbleMTX$ItemGrantedCallback;Lcom/ea/nimble/mtx/"
         "INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;",
         reject);
    constexpr auto callback = "com/ea/nimble/bridge/MTXNativeCallback";
    j.set_interfaces(callback, {"com/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback",
                                "com/ea/nimble/mtx/INimbleMTX$ItemGrantedCallback",
                                "com/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback"});
    j.hook(callback, "<init>", "(I)V", [&j](auto &call) {
        j.get(call.receiver).fields["com/ea/nimble/bridge/MTXNativeCallback#m_idI"] =
            static_cast<std::uint32_t>(call.arguments[0]);
        return 0;
    });
    auto component = j.global_ref(j.object("com/ea/nimble/mtx/googleplay/GooglePlay"));
    j.set_superclass("com/ea/nimble/mtx/googleplay/GooglePlay", "com/ea/nimble/Component");
    j.set_interfaces("com/ea/nimble/mtx/googleplay/GooglePlay", {api});
    j.get(component).fields["component_id"] = "com.ea.nimble.mtx.googleplay";
    j.hook("com/ea/nimble/mtx/NimbleMTX", "getComponent", "()Lcom/ea/nimble/mtx/INimbleMTX;",
           [&j, component](auto &) { return j.local_ref(component); });
    r.message("[PopCap] Billing unavailable; purchases are disabled");
    return component;
}
} // namespace acp::games

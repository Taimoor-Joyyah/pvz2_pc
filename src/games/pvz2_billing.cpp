#include <acp/pvz2.hpp>

#include <chrono>
namespace acp::games {
namespace {
constexpr auto api = "com/ea/nimble/mtx/INimbleMTX";
constexpr auto transaction = "com/ea/nimble/mtx/NimbleMTXTransaction";
constexpr auto item = "com/ea/nimble/mtx/NimbleCatalogItem";
constexpr auto callback_type = "com/ea/nimble/bridge/MTXNativeCallback";
struct Billing : std::enable_shared_from_this<Billing> {
    android::Runtime &r;
    android::Jni &j;
    Pvz2Options options;
    std::function<void(std::string, Address)> broadcast;
    fs::path file;
    Json ledger{
        {"schema", 1}, {"provider", "local-test"}, {"next_id", 1}, {"transactions", Json::object()}};
    std::map<std::string, Json> catalog;
    Billing(android::Runtime &runtime, const Pvz2Options &opts,
            std::function<void(std::string, Address)> notify)
        : r(runtime), j(*r.jni), options(opts), broadcast(std::move(notify)),
          file(r.options.data / "preferences/pc-runtime/billing.json") {
        if (fs::exists(file)) {
            ledger = read_json(file);
            require(ledger.at("schema") == 1 && ledger.at("provider") == "local-test" &&
                        ledger.at("transactions").is_object() && ledger.at("next_id").is_number_unsigned(),
                    "Invalid local purchase ledger: " + utf8(file));
        }
        for (const auto &entry : options.catalog) {
            auto sku = entry.at("sku").get<std::string>();
            require(!sku.empty() && sku.size() <= 4096, "Invalid offline catalog SKU");
            auto type = entry.value("type", "consumable");
            require(type == "consumable" || type == "nonconsumable" || type == "subscription",
                    "Offline catalog type must be consumable, nonconsumable or subscription");
            require(catalog.emplace(sku, entry).second, "Duplicate offline catalog SKU: " + sku);
        }
        for (const auto &[_, value] : ledger.at("transactions").items()) {
            auto sku = value.at("sku").get<std::string>();
            auto state = value.at("state").get<int>(), type = value.at("item_type").get<int>();
            require(!sku.empty() && state >= 5 && state <= 7 && type >= 0 && type <= 3,
                    "Invalid transaction in the local purchase ledger");
            catalog.try_emplace(sku, Json{{"sku", sku},
                                          {"type", type == 1   ? "nonconsumable"
                                                   : type == 3 ? "subscription"
                                                               : "consumable"}});
        }
    }
    bool enabled() const {
        return options.purchases == PurchaseMode::AutoComplete;
    }
    Address error(std::string message, int code = 103) {
        auto value = j.object("com/ea/nimble/Error");
        j.get(value).fields = {{"com/ea/nimble/Error#m_domainLjava/lang/String;",
                                j.global_ref(j.string(code >= 20000 ? "NimbleMTXError" : "NimbleError"))},
                               {"com/ea/nimble/Error#m_codeI", code},
                               {"message", std::move(message)},
                               {"cause", 0}};
        return value;
    }
    Address enumeration(std::string type, std::string name, int ordinal) {
        j.set_superclass(type, "java/lang/Enum");
        auto value = j.object(std::move(type));
        j.get(value).fields = {{"name", std::move(name)}, {"ordinal", ordinal}};
        return value;
    }
    Address transaction_object(const std::string &id) {
        auto value = j.object("pc/pvz2/LocalTransaction");
        j.get(value).fields["billing_id"] = id;
        return value;
    }
    const Json &record(Address object) {
        return ledger.at("transactions").at(j.get(object).fields.at("billing_id").get<std::string>());
    }
    void persist() {
        write_json(file, ledger);
    }
    void complete(Address callback, const std::string &id, bool purchase) {
        require(callback != 0, "Missing local purchase callback");
        auto native_id = j.get(callback).fields.at(std::string(callback_type) + "#m_idI").get<Address>();
        auto self = shared_from_this();
        r.post_callback([self, native_id, id, purchase](guest::Cpu &cpu) {
            auto &jni = self->j;
            Address payload{}, function{}, type{};
            {
                std::lock_guard lock(self->r.state_mutex);
                payload = jni.object("[Ljava/lang/Object;");
                jni.get(payload).elements.push_back(jni.global_ref(self->transaction_object(id)));
                if (purchase) {
                    auto completed = jni.object("java/lang/Boolean");
                    jni.get(completed).fields = {{"kind", "Z"}, {"bits", 1}};
                    jni.get(payload).elements.push_back(jni.global_ref(completed));
                }
                type = jni.class_ref("com/ea/nimble/bridge/BaseNativeCallback");
                function = jni.native("com/ea/nimble/bridge/BaseNativeCallback", "nativeCallback",
                                      "(I[Ljava/lang/Object;)V");
            }
            require(function != 0, "Native purchase callback binding is missing");
            jni.call_native(cpu, function, std::array{jni.environment(cpu), type, native_id, payload});
        });
    }
    void event(bool restore, Address failure = 0) {
        auto data = j.object("java/util/HashMap");
        j.get(data).elements = {j.global_ref(j.string("result")),
                                j.global_ref(j.string(failure ? "0" : "1"))};
        if (failure) {
            j.get(data).elements.push_back(j.global_ref(j.string("error")));
            j.get(data).elements.push_back(j.global_ref(failure));
        }
        broadcast(restore ? "nimble.notification.mtx.restorepurchasedtransactionsfinished"
                          : "nimble.notification.mtx.refreshcatalogfinished",
                  data);
    }
    Address list(std::string_view kind) {
        auto result = j.object("java/util/ArrayList");
        if (!enabled())
            return result;
        for (const auto &[id, value] : ledger.at("transactions").items()) {
            int state = value.at("state");
            if ((kind == "getPendingTransactions" && state < 5) ||
                (kind == "getPurchasedTransactions" && state >= 5 && state < 7) ||
                (kind == "getRecoveredTransactions" && value.value("recovered", false) && state < 7))
                j.get(result).elements.push_back(j.global_ref(transaction_object(id)));
        }
        return result;
    }
    void install() {
        auto self = shared_from_this();
        auto hook = [this, self](const char *type, const char *name, const char *signature, auto function) {
            j.hook(type, name, signature, [self, function](android::JavaCall &call) -> Address {
                std::lock_guard lock(self->r.state_mutex);
                return function(*self, call);
            });
        };
        j.set_interfaces("pc/pvz2/LocalTransaction", {transaction});
        j.set_superclass("pc/pvz2/LocalCatalogItem", item);
        j.set_interfaces(callback_type, {"com/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback",
                                         "com/ea/nimble/mtx/INimbleMTX$ItemGrantedCallback",
                                         "com/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback"});
        hook(callback_type, "<init>", "(I)V", [](auto &b, auto &c) {
            b.j.get(c.receiver).fields[std::string(callback_type) + "#m_idI"] =
                static_cast<std::uint32_t>(c.arguments[0]);
            return 0;
        });
        for (auto name : {"getPendingTransactions", "getPurchasedTransactions", "getRecoveredTransactions"})
            hook(api, name, "()Ljava/util/List;", [name](auto &b, auto &) { return b.list(name); });
        hook(api, "setPlatformParameters", "(Ljava/util/Map;)V", [](auto &, auto &) { return 0; });
        hook(api, "getAvailableCatalogItems", "()Ljava/util/List;", [](auto &b, auto &) -> Address {
            if (!b.enabled())
                return 0;
            auto result = b.j.object("java/util/ArrayList");
            for (const auto &[sku, entry] : b.catalog) {
                auto value = b.j.object("pc/pvz2/LocalCatalogItem");
                b.j.get(value).fields = entry;
                b.j.get(result).elements.push_back(b.j.global_ref(value));
            }
            return result;
        });
        for (bool restore : {false, true})
            hook(api, restore ? "restorePurchasedTransactions" : "refreshAvailableCatalogItems", "()V",
                 [restore](auto &b, auto &) {
                     if (!b.enabled())
                         b.event(restore, b.error("Purchases are disabled in game.ini"));
                     else {
                         if (restore) {
                             for (auto &[_, value] : b.ledger["transactions"].items()) {
                                 // Completed consumables are never granted again on restore.
                                 if (value.at("state") != 7 || value.at("item_type") == 1) {
                                     value["recovered"] = true;
                                     if (value.at("state") == 7)
                                         value["state"] = 5;
                                     value["type"] = 1;
                                 }
                             }
                             b.persist();
                         }
                         b.event(restore);
                     }
                     return 0;
                 });
        hook(api, "purchaseItem",
             "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Ljava/lang/"
             "String;)Lcom/ea/nimble/Error;",
             [](auto &b, auto &c) -> Address {
                 if (!b.enabled())
                     return b.error("Purchases are disabled in game.ini");
                 auto sku = b.j.text(c.arguments[0]);
                 if (sku.empty() || !c.arguments[1])
                     return b.error("A purchase requires a SKU and callback");
                 auto entry = b.catalog.contains(sku) ? b.catalog.at(sku)
                                                      : Json{{"sku", sku}, {"type", "consumable"}};
                 b.catalog.try_emplace(sku, entry);
                 auto type = entry.value("type", "consumable");
                 auto item_type = type == "nonconsumable" ? 1 : type == "subscription" ? 3 : 2;
                 for (const auto &[_, value] : b.ledger["transactions"].items())
                     if (item_type != 2 && value.at("sku") == sku)
                         return b.error("Local item is already owned", 20001);
                 auto next = b.ledger.at("next_id").template get<std::uint64_t>();
                 require(next < UINT64_MAX, "Local purchase ID counter exhausted");
                 auto id = "local-test-" + timestamp() + "-" + std::to_string(next);
                 b.ledger["next_id"] = next + 1;
                 b.ledger["transactions"][id] = {
                     {"sku", sku},
                     {"state", 5},
                     {"type", 0},
                     {"item_type", item_type},
                     {"time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()},
                     {"recovered", false}};
                 b.persist();
                 b.complete(c.arguments[1], id, true);
                 return 0;
             });
        for (bool finalize : {false, true})
            hook(api, finalize ? "finalizeTransaction" : "itemGranted",
                 finalize ? "(Ljava/lang/String;Lcom/ea/nimble/mtx/"
                            "INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;"
                          : "(Ljava/lang/String;Lcom/ea/nimble/mtx/NimbleCatalogItem$ItemType;Lcom/ea/"
                            "nimble/mtx/INimbleMTX$ItemGrantedCallback;)Lcom/ea/nimble/Error;",
                 [finalize](auto &b, auto &c) -> Address {
                     if (!b.enabled())
                         return b.error("Purchases are disabled in game.ini");
                     auto id = b.j.text(c.arguments[0]);
                     if (!b.ledger["transactions"].contains(id))
                         return b.error("Unknown local transaction", 20010);
                     auto callback = c.arguments[finalize ? 1 : 2];
                     if (!callback)
                         return b.error("Missing local transaction callback");
                     auto &value = b.ledger["transactions"][id];
                     if ((finalize && value.at("state") != 6 && value.at("state") != 7) ||
                         (!finalize && value.at("state") != 5))
                         return b.error("Invalid local transaction state", 20011);
                     if (!finalize && c.arguments[1])
                         value["item_type"] = b.j.get(c.arguments[1]).fields.at("ordinal");
                     value["state"] = finalize ? 7 : 6;
                     b.persist();
                     b.complete(callback, id, false);
                     return 0;
                 });
        hook(api, "resumeTransaction",
             "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Lcom/ea/nimble/"
             "mtx/INimbleMTX$ItemGrantedCallback;Lcom/ea/nimble/mtx/"
             "INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;",
             [](auto &b, auto &c) -> Address {
                 if (!b.enabled())
                     return b.error("Purchases are disabled in game.ini");
                 auto id = b.j.text(c.arguments[0]);
                 if (!b.ledger["transactions"].contains(id))
                     return b.error("Unknown local transaction", 20010);
                 auto state = b.ledger["transactions"][id]["state"].template get<int>();
                 auto callback = c.arguments[state == 5 ? 1 : state == 6 ? 2 : 3];
                 if (!callback)
                     return b.error("Missing resume callback");
                 b.complete(callback, id, state == 5);
                 return 0;
             });
        hook(api, "getTransaction", "(Ljava/lang/String;)Lcom/ea/nimble/mtx/NimbleMTXTransaction;",
             [](auto &b, auto &c) -> Address {
                 auto id = b.j.text(c.arguments[0]);
                 return b.enabled() && b.ledger["transactions"].contains(id) ? b.transaction_object(id) : 0;
             });
        for (auto name : {"getItemSku", "getItemSellId", "getTransactionId", "getReceipt"})
            hook(transaction, name, "()Ljava/lang/String;", [name](auto &b, auto &c) -> Address {
                auto id = b.j.get(c.receiver).fields.at("billing_id").template get<std::string>();
                if (std::string_view(name) == "getTransactionId")
                    return b.j.string(id);
                if (std::string_view(name) == "getReceipt")
                    return b.j.string(Json{
                        {"provider", "local-test"},
                        {"transaction_id", id},
                        {"sku", b.record(c.receiver).at("sku")},
                        {"charged",
                         false}}.dump());
                return b.j.string(b.record(c.receiver).at("sku").template get<std::string>());
            });
        hook(transaction, "getError", "()Ljava/lang/Exception;", [](auto &, auto &) { return 0; });
        hook(transaction, "getPriceDecimal", "()F", [](auto &, auto &) { return 0; });
        hook(transaction, "getTimeStamp", "()Ljava/util/Date;", [](auto &b, auto &c) {
            auto result = b.j.object("java/util/Date");
            b.j.get(result).fields["milliseconds"] = b.record(c.receiver).at("time_ms");
            return result;
        });
        hook(transaction, "getTransactionState",
             "()Lcom/ea/nimble/mtx/NimbleMTXTransaction$TransactionState;", [](auto &b, auto &c) {
                 int state = b.record(c.receiver).at("state");
                 constexpr const char *names[] = {"UNDEFINED",
                                                  "USER_INITIATED",
                                                  "WAITING_FOR_PREPURCHASE_INFO",
                                                  "WAITING_FOR_PLATFORM_RESPONSE",
                                                  "WAITING_FOR_VERIFICATION",
                                                  "WAITING_FOR_GAME_TO_CONFIRM_ITEM_GRANT",
                                                  "WAITING_FOR_PLATFORM_CONSUMPTION",
                                                  "COMPLETE"};
                 require(state >= 0 && state < 8, "Invalid purchase ledger state");
                 return b.enumeration(std::string(transaction) + "$TransactionState", names[state], state);
             });
        hook(transaction, "getTransactionType",
             "()Lcom/ea/nimble/mtx/NimbleMTXTransaction$TransactionType;", [](auto &b, auto &c) {
                 int type = b.record(c.receiver).at("type");
                 return b.enumeration(std::string(transaction) + "$TransactionType",
                                      type ? "RESTORE" : "PURCHASE", type);
             });
        for (auto name : {"getSku", "getSellId", "getTitle", "getDescription", "getMetaDataUrl",
                          "getPriceWithCurrencyAndFormat"})
            hook(item, name, "()Ljava/lang/String;", [name](auto &b, auto &c) {
                auto &fields = b.j.get(c.receiver).fields;
                std::string_view method(name);
                auto sku = fields.at("sku").template get<std::string>();
                return b.j.string(method == "getSku" || method == "getSellId" ? sku
                                  : method == "getTitle"                      ? fields.value("title", sku)
                                  : method == "getDescription"
                                      ? fields.value("description", "Local simulated purchase")
                                  : method == "getPriceWithCurrencyAndFormat" ? "Local test"
                                                                              : "");
            });
        hook(item, "getPriceDecimal", "()F", [](auto &, auto &) { return 0; });
        hook(item, "isFree", "()Z", [](auto &, auto &) { return 1; });
        hook(item, "getItemType", "()Lcom/ea/nimble/mtx/NimbleCatalogItem$ItemType;", [](auto &b, auto &c) {
            auto type = b.j.get(c.receiver).fields.value("type", "consumable");
            return b.enumeration(std::string(item) + "$ItemType",
                                 type == "nonconsumable"  ? "NONCONSUMABLE"
                                 : type == "subscription" ? "SUBSCRIPTION"
                                                          : "CONSUMABLE",
                                 type == "nonconsumable"  ? 1
                                 : type == "subscription" ? 3
                                                          : 2);
        });
        for (auto type : {item, transaction})
            hook(type, "getAdditionalInfo", "()Ljava/util/Map;", [](auto &b, auto &) {
                auto result = b.j.object("java/util/HashMap");
                b.j.get(result).elements = {b.j.global_ref(b.j.string("provider")),
                                            b.j.global_ref(b.j.string("local-test"))};
                return result;
            });
    }
};
} // namespace
Address install_pvz2_billing(android::Runtime &r, const Pvz2Options &options,
                             std::function<void(std::string, Address)> broadcast) {
    auto billing = std::make_shared<Billing>(r, options, std::move(broadcast));
    billing->install();
    auto &j = *r.jni;
    auto component = j.global_ref(j.object("com/ea/nimble/mtx/googleplay/GooglePlay"));
    j.set_superclass("com/ea/nimble/mtx/googleplay/GooglePlay", "com/ea/nimble/Component");
    j.set_interfaces("com/ea/nimble/mtx/googleplay/GooglePlay", {api});
    j.get(component).fields["component_id"] = "com.ea.nimble.mtx.googleplay";
    j.hook("com/ea/nimble/mtx/NimbleMTX", "getComponent", "()Lcom/ea/nimble/mtx/INimbleMTX;",
           [&j, component](auto &) { return j.local_ref(component); });
    r.message(billing->enabled()
                  ? "[PopCap] Local purchase simulation enabled; no real payments or store receipts"
                  : "[PopCap] Purchases disabled in game.ini");
    return component;
}
} // namespace acp::games

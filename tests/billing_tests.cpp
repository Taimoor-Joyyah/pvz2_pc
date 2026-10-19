#include <acp/pvz2.hpp>

#include <iostream>
namespace {
using namespace acp;
void disabled_billing(const fs::path &root) {
    // Old provider files are deliberately invalid: disabled billing must not open them.
    auto old_ledger = root / "preferences/pc-runtime/billing.json";
    constexpr std::string_view sentinel = "existing profile data must remain untouched";
    write_bytes(old_ledger, std::as_bytes(std::span(sentinel)));
    auto original = sha256(old_ledger);
    auto adapter = games::pvz2();
    android::Runtime r({"com.ea.game.pvz2_row", "13.5.1", "en_US", 1063, root, {}, {}});
    adapter->configure(r, {});
    auto &j = *r.jni;
    auto &m = r.memory;
    auto invoke = [&](size_t slot, std::initializer_list<Address> values) {
        auto env = j.environment(), table = m.load<Address>(env);
        std::vector<Address> args{env};
        args.insert(args.end(), values);
        return r.cpu.call(m.load<Address>(table + slot * 8), args);
    };
    auto call = [&](Address object, const char *type, const char *name, const char *signature, size_t slot,
                    std::initializer_list<Address> arguments = {}) {
        auto method = invoke(33, {j.class_ref(type), m.string(name), m.string(signature)});
        auto values = m.allocate(std::max<size_t>(8, arguments.size() * 8));
        size_t index{};
        for (auto argument : arguments)
            m.store(values + index++ * 8, argument);
        auto result = invoke(slot + 2, {object, method, values});
        m.release(values);
        return result;
    };
    auto type = j.class_ref("com/ea/nimble/mtx/NimbleMTX");
    auto get = invoke(113, {type, m.string("getComponent"), m.string("()Lcom/ea/nimble/mtx/INimbleMTX;")});
    auto component = invoke(114, {type, get});
    constexpr auto api = "com/ea/nimble/mtx/INimbleMTX";
    auto error = [&](Address value) {
        require(value && j.get(value).fields.at("com/ea/nimble/Error#m_codeI") == 103,
                "Unavailable billing must return the SDK NOT_AVAILABLE error");
    };
    unsigned callbacks{};
    r.linker.provide("Java_com_ea_nimble_bridge_BaseNativeCallback_nativeCallback",
                     [&](auto &) { ++callbacks; });
    auto callback = j.object("com/ea/nimble/bridge/MTXNativeCallback");
    call(callback, "com/ea/nimble/bridge/MTXNativeCallback", "<init>", "(I)V", 61, {7});
    error(call(component, api, "purchaseItem",
               "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;"
               "Ljava/lang/String;)Lcom/ea/nimble/Error;",
               34, {j.string("fixture.item"), callback, j.string("context")}));
    error(call(component, api, "itemGranted",
               "(Ljava/lang/String;Lcom/ea/nimble/mtx/NimbleCatalogItem$ItemType;Lcom/ea/nimble/"
               "mtx/INimbleMTX$ItemGrantedCallback;)Lcom/ea/nimble/Error;",
               34, {j.string("old-id"), 0, callback}));
    error(call(component, api, "finalizeTransaction",
               "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback;)"
               "Lcom/ea/nimble/Error;",
               34, {j.string("old-id"), callback}));
    error(call(component, api, "resumeTransaction",
               "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Lcom/"
               "ea/nimble/mtx/INimbleMTX$ItemGrantedCallback;Lcom/ea/nimble/mtx/"
               "INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;",
               34, {j.string("old-id"), callback, callback, callback}));
    for (auto name : {"getPendingTransactions", "getPurchasedTransactions", "getRecoveredTransactions"})
        require(j.get(call(component, api, name, "()Ljava/util/List;", 34)).elements.empty(),
                "Disabled billing exposes no transactions");
    require(call(component, api, "getAvailableCatalogItems", "()Ljava/util/List;", 34) == 0 &&
                call(component, api, "getTransaction",
                     "(Ljava/lang/String;)Lcom/ea/nimble/mtx/NimbleMTXTransaction;", 34,
                     {j.string("old-id")}) == 0,
            "Disabled billing exposes no catalog or prior transactions");
    r.wait_callbacks();
    require(callbacks == 0 && sha256(old_ledger) == original,
            "Rejected purchases never invoke completion callbacks or modify old provider data");
}
} // namespace
int main() {
    try {
        auto root = acp::fs::current_path() / "test-artifacts" /
                    acp::path_from_utf8("billing-disabled-" + acp::timestamp());
        disabled_billing(root);
        std::cout << "Disabled billing rejects purchases, grants, finalization and recovery\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

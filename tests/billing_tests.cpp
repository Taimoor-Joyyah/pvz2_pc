#include <acp/pvz2.hpp>

#include <iostream>
namespace {
using namespace acp;
constexpr auto api = "com/ea/nimble/mtx/INimbleMTX";
constexpr auto txn = "com/ea/nimble/mtx/NimbleMTXTransaction";
constexpr auto purchase = "(Ljava/lang/String;Lcom/ea/nimble/mtx/"
                          "INimbleMTX$PurchaseTransactionCallback;Ljava/lang/String;)Lcom/ea/nimble/Error;";
constexpr auto finalize =
    "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;";
constexpr auto grant = "(Ljava/lang/String;Lcom/ea/nimble/mtx/NimbleCatalogItem$ItemType;Lcom/ea/nimble/"
                       "mtx/INimbleMTX$ItemGrantedCallback;)Lcom/ea/nimble/Error;";
struct Fixture {
    std::unique_ptr<android::GameAdapter> adapter;
    android::Runtime r;
    android::Jni &j;
    guest::Memory &m;
    Address component{};
    struct Delivery {
        Address id, transaction;
        bool purchase;
    };
    std::vector<Delivery> deliveries;
    Fixture(const fs::path &root, games::Pvz2Options options)
        : adapter(games::pvz2(options)), r({"com.ea.game.pvz2_row", "13.5.1", "en_US", 1063, root, {}, {}}),
          j(*r.jni), m(r.memory) {
        adapter->configure(r, {});
        r.linker.provide("Java_com_ea_nimble_bridge_BaseNativeCallback_nativeCallback", [this](auto &cpu) {
            std::lock_guard lock(r.state_mutex);
            auto elements = j.get(cpu.x(3)).elements;
            require(elements.size() == 1 || elements.size() == 2, "Transaction callback shape");
            if (elements.size() == 2)
                require(j.get(elements[1]).fields.at("bits") == 1,
                        "purchaseComplete carries true, per the APK bridge");
            deliveries.push_back({cpu.x(2), j.global_ref(elements[0]), elements.size() == 2});
            cpu.x(0, 0);
        });
        auto type = j.class_ref("com/ea/nimble/mtx/NimbleMTX");
        auto method =
            invoke(113, {type, m.string("getComponent"), m.string("()Lcom/ea/nimble/mtx/INimbleMTX;")});
        component = invoke(114, {type, method});
    }
    Address invoke(size_t slot, std::initializer_list<Address> values) {
        auto env = j.environment(), table = m.load<Address>(env);
        std::vector<Address> args{env};
        args.insert(args.end(), values);
        return r.cpu.call(m.load<Address>(table + slot * 8), args);
    }
    Address call(std::string_view type, Address object, std::string_view method, std::string_view signature,
                 size_t slot = 34, std::initializer_list<Address> values = {}) {
        auto id = invoke(33, {j.class_ref(std::string(type)), m.string(method), m.string(signature)});
        auto array = m.allocate(std::max<size_t>(8, values.size() * 8));
        size_t offset = 0;
        for (auto value : values) {
            m.store(array + offset, value);
            offset += 8;
        }
        auto result = invoke(slot + 2, {object, id, array});
        m.release(array);
        return result;
    }
    Address callback(unsigned id) {
        auto type = j.class_ref("com/ea/nimble/bridge/MTXNativeCallback");
        auto constructor = invoke(33, {type, m.string("<init>"), m.string("(I)V")});
        return invoke(28, {type, constructor, id});
    }
    Address buy(const char *sku, unsigned id) {
        return call(api, component, "purchaseItem", purchase, 34,
                    {j.string(sku), callback(id), j.string("local-test")});
    }
    std::string identifier(Address object) {
        return j.text(call(txn, object, "getTransactionId", "()Ljava/lang/String;"));
    }
    Address state(Address object) {
        auto value = call(txn, object, "getTransactionState",
                          "()Lcom/ea/nimble/mtx/NimbleMTXTransaction$TransactionState;");
        return call("java/lang/Enum", value, "ordinal", "()I", 49);
    }
    size_t list_size(const char *name) {
        return j.get(call(api, component, name, "()Ljava/util/List;")).elements.size();
    }
};
games::Pvz2Options simulated() {
    games::Pvz2Options options;
    options.purchases = games::PurchaseMode::AutoComplete;
    options.catalog = {{{"sku", "fixture.coins"}, {"type", "consumable"}},
                       {{"sku", "fixture.item"}, {"type", "nonconsumable"}}};
    return options;
}
void config(const fs::path &root) {
    auto ini = root / "game.ini";
    require(games::read_pvz2_options(ini).purchases == games::PurchaseMode::Disabled,
            "New INI disables purchases");
    auto initial = sha256(ini);
    games::read_pvz2_options(ini);
    require(sha256(ini) == initial, "Loading INI preserves its contents");
    auto write = [&](std::string_view text) { write_bytes(ini, std::as_bytes(std::span(text))); };
    write("[purchases]\nmode=auto_complete\n");
    require(games::read_pvz2_options(ini).purchases == games::PurchaseMode::AutoComplete,
            "INI enables local completion");
    write("[purchases]\nmode=disabled\ncatalog_file=missing.json\n");
    require(games::read_pvz2_options(ini).purchases == games::PurchaseMode::Disabled,
            "Disabling purchases removes any dependency on optional catalog files");
    write("[purchases]\nmode=typo\n");
    bool rejected = false;
    try {
        games::read_pvz2_options(ini);
    } catch (const Error &) {
        rejected = true;
    }
    require(rejected, "Invalid purchase mode fails clearly");
}
void play_independence(Fixture &f) {
    auto connection = f.j.object("com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect");
    auto achievements = f.j.object("com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements");
    auto leaderboards = f.j.object("com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard");
    for (auto name : {"Play_Connect", "Play_Connect_Silent", "Play_Disconnect"})
        f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect", connection, name, "()V", 61);
    require(f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect", connection,
                   "Play_IsConnected", "()Z", 37) == 0,
            "Gameplay never requires or advertises a Google Play Games account");
    for (float percentage : {0.f, 50.5f, 100.f})
        f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements", achievements,
               "Play_QueueAchievement_Percentage", "(Ljava/lang/String;F)V", 61,
               {f.j.string("fixture.achievement"), std::bit_cast<std::uint32_t>(percentage)});
    for (auto name : {"Play_ResetAchievements", "Play_ShowAchievementView"})
        f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements", achievements, name, "()V",
               61);
    f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard", leaderboards,
           "Play_SubmitScoreToLeaderboard", "(ILjava/lang/String;)V", 61,
           {123, f.j.string("fixture.board")});
    f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard", leaderboards,
           "Play_ShowLeaderboardView", "(Ljava/lang/String;)V", 61, {f.j.string("fixture.board")});
    f.call("com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard", leaderboards,
           "Play_ShowLeaderboardViewAll", "()V", 61);
}
void transactions(const fs::path &root) {
    std::string interrupted;
    {
        Fixture f(root, simulated());
        play_independence(f);
        require(f.list_size("getPendingTransactions") == 0, "No pending purchases initially");
        auto catalog = f.call(api, f.component, "getAvailableCatalogItems", "()Ljava/util/List;");
        require(f.j.get(catalog).elements.size() == 2, "Offline catalog is available without Google Play");
        require(f.buy("fixture.coins", 10) == 0, "Local purchase accepted");
        f.r.wait_callbacks();
        require(f.deliveries.size() == 1 && f.deliveries.back().id == 10 && f.deliveries.back().purchase,
                "Local purchase completes through native callback");
        auto value = f.deliveries.back().transaction;
        require(f.state(value) == 5, "Purchase waits for the game's grant confirmation");
        auto receipt = Json::parse(f.j.text(f.call(txn, value, "getReceipt", "()Ljava/lang/String;")));
        require(receipt.at("provider") == "local-test" && receipt.at("charged") == false,
                "Receipt is explicitly a local simulation");
        auto date = f.call(txn, value, "getTimeStamp", "()Ljava/util/Date;");
        require(f.call("java/util/Date", date, "getTime", "()J", 52) > 1'700'000'000'000ull,
                "Transaction timestamp uses Java milliseconds");
        interrupted = f.identifier(value);
        require(f.call(api, f.component, "itemGranted", grant, 34,
                       {f.j.string(interrupted), 0, f.callback(11)}) == 0,
                "Grant acknowledgement accepted");
        f.r.wait_callbacks();
        require(f.state(value) == 6 && !f.deliveries.back().purchase,
                "Acknowledged grant waits for finalization");
    }
    {
        Fixture f(root, simulated());
        f.call(api, f.component, "restorePurchasedTransactions", "()V", 61);
        auto recovered = f.call(api, f.component, "getRecoveredTransactions", "()Ljava/util/List;");
        require(f.j.get(recovered).elements.size() == 1 && f.state(f.j.get(recovered).elements[0]) == 6,
                "Restart preserves grant acknowledgement and does not grant consumables twice");
        auto resume = "(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Lcom/"
                      "ea/nimble/mtx/INimbleMTX$ItemGrantedCallback;Lcom/ea/nimble/mtx/"
                      "INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;";
        require(f.call(api, f.component, "resumeTransaction", resume, 34,
                       {f.j.string(interrupted), f.callback(20), f.callback(21), f.callback(22)}) == 0,
                "Interrupted transaction resumes");
        f.r.wait_callbacks();
        require(f.deliveries.size() == 1 && f.deliveries.back().id == 21 && !f.deliveries.back().purchase,
                "Resume delivers only the pending stage");
        require(f.call(api, f.component, "finalizeTransaction", finalize, 34,
                       {f.j.string(interrupted), f.callback(23)}) == 0,
                "Local transaction finalizes");
        f.r.wait_callbacks();
        require(f.list_size("getPurchasedTransactions") == 0 &&
                    f.list_size("getRecoveredTransactions") == 0,
                "Finalized consumables leave active transaction lists");
        f.call(api, f.component, "restorePurchasedTransactions", "()V", 61);
        require(f.list_size("getRecoveredTransactions") == 0, "Restore excludes completed consumables");
        require(f.buy("fixture.item", 30) == 0, "Nonconsumable purchase accepted");
        f.r.wait_callbacks();
        require(f.buy("fixture.item", 31) != 0, "Duplicate nonconsumable purchase is rejected");
        require(f.call(api, f.component, "finalizeTransaction", finalize, 34,
                       {f.j.string("unknown"), f.callback(32)}) != 0,
                "Unknown transactions cannot finalize");
    }
    auto hash = sha256(root / "preferences/pc-runtime/billing.json");
    {
        Fixture f(root, {});
        play_independence(f);
        require(f.buy("fixture.coins", 40) != 0, "Disabled mode rejects purchases");
        f.r.wait_callbacks();
        require(f.deliveries.empty() && f.list_size("getPurchasedTransactions") == 0,
                "Disabled mode cannot complete a purchase");
    }
    require(sha256(root / "preferences/pc-runtime/billing.json") == hash,
            "Disabling purchases preserves the local ledger");
}
} // namespace
int main() {
    try {
        auto root = fs::current_path() / "test-artifacts" / path_from_utf8("billing-" + timestamp());
        config(root);
        transactions(root / "profile");
        std::cout << "Play independence, INI, local purchase callbacks and recovery checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

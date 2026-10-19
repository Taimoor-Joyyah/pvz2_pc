#include <acp/pvz2.hpp>

namespace acp::games {
namespace {
constexpr auto persistence = "com/ea/nimble/Persistence";
constexpr auto storage_type = "com/ea/nimble/Persistence$Storage";
constexpr auto service_type = "com/ea/nimble/PersistenceService";
struct Store {
    std::string identifier;
    unsigned storage{};
    bool encryption{}, backup{};
    Json values = Json::object();
    Address object{};
};
struct Persistence {
    android::Runtime &runtime;
    android::Jni &jni;
    std::array<Address, 3> storage{};
    std::map<std::string, Store> stores;
    explicit Persistence(android::Runtime &r) : runtime(r), jni(*r.jni) {}
    unsigned mode(Address value) {
        require(value != 0, "Nimble persistence storage is null");
        auto ordinal = jni.get(value).fields.at("ordinal").get<unsigned>();
        require(ordinal < storage.size(), "Invalid Nimble persistence storage");
        return ordinal;
    }
    std::string key(std::string_view identifier, unsigned storage) {
        return std::to_string(storage) + ":" + std::string(identifier);
    }
    std::string guest_path(const Store &store) {
        auto hash = sha256(std::as_bytes(std::span(store.identifier)));
        // Reserve a host-format namespace: never overwrite Java ObjectOutputStream files.
        return "/data/data/" + runtime.options.package + (store.storage == 0 ? "/files/" : "/cache/") +
               "pc-runtime/nimble/" + std::to_string(store.storage) + "/" + hash + ".json";
    }
    fs::path path(const Store &store) {
        return runtime.path(guest_path(store), true);
    }
    void save(const Store &store) {
        Json data{{"format", 1},
                  {"identifier", store.identifier},
                  {"storage", store.storage},
                  {"backup", store.backup},
                  {"encryption", store.encryption}};
        if (store.encryption) {
            auto text = store.values.dump();
            auto encrypted = host::protect_data(std::as_bytes(std::span(text)));
            data["protected"] = Json::array();
            for (auto byte : encrypted)
                data["protected"].push_back(std::to_integer<unsigned>(byte));
        } else
            data["values"] = store.values;
        write_json(path(store), data);
    }
    void load(Store &store) {
        if (!fs::is_regular_file(path(store)))
            return;
        auto data = read_json(path(store));
        require(data.at("format") == 1 && data.at("identifier") == store.identifier &&
                    data.at("storage") == store.storage,
                "Mismatched Nimble persistence record");
        store.encryption = data.at("encryption").get<bool>();
        store.backup = data.at("backup").get<bool>();
        if (store.encryption) {
            Bytes encrypted;
            for (auto value : data.at("protected")) {
                auto byte = value.get<unsigned>();
                require(byte <= 255, "Invalid protected persistence byte");
                encrypted.push_back(static_cast<std::byte>(byte));
            }
            auto text = host::unprotect_data(encrypted);
            store.values = Json::parse(text.begin(), text.end());
        } else
            store.values = data.at("values");
        require(store.values.is_object(), "Invalid Nimble persistence values");
    }
    Address get(std::string identifier, Address value, Address instance = 0) {
        if (identifier.empty())
            return 0;
        auto storage_mode = mode(value);
        auto id = key(identifier, storage_mode);
        auto [it, inserted] = stores.try_emplace(id);
        auto &store = it->second;
        if (inserted) {
            store.identifier = std::move(identifier);
            store.storage = storage_mode;
            auto object = instance ? instance : jni.object(persistence);
            store.object = jni.global_ref(object);
            jni.get(object).fields["store"] = id;
            load(store);
        } else if (instance)
            jni.get(instance).fields["store"] = id;
        return jni.local_ref(store.object);
    }
    Store &receiver(Address object) {
        return stores.at(jni.get(object).fields.at("store").get<std::string>());
    }
    void set(Store &store, Address key, Address value) {
        if (!key || jni.get(key).class_name != "java/lang/String")
            return;
        auto name = jni.text(key);
        if (name.empty())
            return;
        if (value)
            store.values[name] = jni.serialize(value);
        else
            store.values.erase(name);
    }
};
} // namespace
void install_pvz2_persistence(android::Runtime &r) {
    auto backend = std::make_shared<Persistence>(r);
    auto &j = *r.jni;
    j.set_superclass(storage_type, "java/lang/Enum");
    unsigned ordinal = 0;
    for (const auto *name : {"DOCUMENT", "CACHE", "TEMP"}) {
        auto value = j.object(storage_type);
        j.get(value).fields = {{"name", name}, {"ordinal", ordinal}};
        backend->storage[ordinal++] = j.global_ref(value);
        j.field(storage_type, name, "Lcom/ea/nimble/Persistence$Storage;", value);
    }
    j.set_superclass("com/ea/nimble/PersistenceServiceImpl", "com/ea/nimble/Component");
    j.set_interfaces("com/ea/nimble/PersistenceServiceImpl", {"com/ea/nimble/IPersistenceService"});
    auto service = j.global_ref(j.object("com/ea/nimble/PersistenceServiceImpl"));
    auto hook = [&j, backend](std::string type, std::string name, std::string signature, auto handler) {
        j.hook(std::move(type), std::move(name), std::move(signature),
               [backend, handler](android::JavaCall &call) -> std::uint64_t {
                   std::lock_guard lock(backend->runtime.state_mutex);
                   return handler(*backend, call);
               });
    };
    hook(service_type, "getComponent", "()Lcom/ea/nimble/IPersistenceService;",
         [service](auto &b, auto &) { return b.jni.local_ref(service); });
    hook(service_type, "getAppPersistence",
         "(Lcom/ea/nimble/Persistence$Storage;)Lcom/ea/nimble/Persistence;",
         [](auto &b, auto &c) { return b.get("[APPLICATION]", c.arguments[0]); });
    hook(service_type, "getPersistenceForNimbleComponent",
         "(Ljava/lang/String;Lcom/ea/nimble/Persistence$Storage;)Lcom/ea/nimble/Persistence;",
         [](auto &b, auto &c) {
             auto id = b.jni.text(c.arguments[0]);
             return id.empty() ? 0 : b.get("[COMPONENT]" + id, c.arguments[1]);
         });
    for (const auto *type : {"com/ea/nimble/IPersistenceService", "com/ea/nimble/PersistenceServiceImpl"}) {
        hook(type, "getPersistence",
             "(Ljava/lang/String;Lcom/ea/nimble/Persistence$Storage;)Lcom/ea/nimble/Persistence;",
             [](auto &b, auto &c) { return b.get(b.jni.text(c.arguments[0]), c.arguments[1]); });
        hook(type, "synchronize", "()V", [](auto &b, auto &) {
            for (const auto &[_, store] : b.stores)
                b.save(store);
            return 0;
        });
    }
    hook(persistence, "<init>",
         "(Ljava/lang/String;Lcom/ea/nimble/Persistence$Storage;Lcom/ea/nimble/Encryptor;)V",
         [](auto &b, auto &c) {
             b.get(b.jni.text(c.arguments[0]), c.arguments[1], c.receiver);
             return 0;
         });
    for (const auto *method : {"getValue", "getStringValue", "getBoolValue"}) {
        auto signature = std::string("(Ljava/lang/String;)") +
                         (std::string_view(method) == "getValue"         ? "Ljava/io/Serializable;"
                          : std::string_view(method) == "getStringValue" ? "Ljava/lang/String;"
                                                                         : "Z");
        hook(persistence, method, signature, [method](auto &b, auto &c) -> std::uint64_t {
            auto &values = b.receiver(c.receiver).values;
            if (!c.arguments[0])
                return 0;
            auto it = values.find(b.jni.text(c.arguments[0]));
            if (it == values.end())
                return 0;
            auto type = it->at("class").template get<std::string>();
            if (std::string_view(method) == "getStringValue" && type != "java/lang/String")
                return 0;
            if (std::string_view(method) == "getBoolValue")
                return type == "java/lang/Boolean" && it->at("bits").template get<std::uint64_t>() != 0;
            return b.jni.deserialize(*it);
        });
    }
    hook(persistence, "setValue", "(Ljava/lang/String;Ljava/io/Serializable;)V", [](auto &b, auto &c) {
        auto &store = b.receiver(c.receiver);
        b.set(store, c.arguments[0], c.arguments[1]);
        b.save(store);
        return 0;
    });
    hook(persistence, "hasKey", "(Ljava/lang/String;)Z", [](auto &b, auto &c) {
        return c.arguments[0] && b.receiver(c.receiver).values.contains(b.jni.text(c.arguments[0]));
    });
    hook(persistence, "synchronize", "()V", [](auto &b, auto &c) {
        b.save(b.receiver(c.receiver));
        return 0;
    });
    hook(persistence, "restore", "(ZLandroid/content/Context;)V", [](auto &b, auto &c) {
        b.load(b.receiver(c.receiver));
        return 0;
    });
    hook(persistence, "clean", "()V", [](auto &b, auto &c) {
        auto &store = b.receiver(c.receiver);
        store.values.clear();
        fs::remove(b.path(store));
        return 0;
    });
    hook(persistence, "getIdentifier", "()Ljava/lang/String;",
         [](auto &b, auto &c) { return b.jni.string(b.receiver(c.receiver).identifier); });
    hook(persistence, "getLogSourceTitle", "()Ljava/lang/String;",
         [](auto &b, auto &) { return b.jni.string("Persistence"); });
    hook(persistence, "getStorage", "()Lcom/ea/nimble/Persistence$Storage;",
         [](auto &b, auto &c) { return b.jni.local_ref(b.storage[b.receiver(c.receiver).storage]); });
    for (const auto *signature : {"()Ljava/lang/String;", "(Landroid/content/Context;)Ljava/lang/String;"})
        hook(persistence, "getPersistencePath", signature,
             [](auto &b, auto &c) { return b.jni.string(b.guest_path(b.receiver(c.receiver))); });
    hook(persistence, "getBackUp", "()Z", [](auto &b, auto &c) { return b.receiver(c.receiver).backup; });
    hook(persistence, "setBackUp", "(Z)V", [](auto &b, auto &c) {
        auto &store = b.receiver(c.receiver);
        if (store.storage == 0) {
            store.backup = c.arguments[0] != 0;
            b.save(store);
        }
        return 0;
    });
    hook(persistence, "getEncryption", "()Z",
         [](auto &b, auto &c) { return b.receiver(c.receiver).encryption; });
    hook(persistence, "setEncryption", "(Z)V", [](auto &b, auto &c) {
        auto &store = b.receiver(c.receiver);
        store.encryption = c.arguments[0] != 0;
        b.save(store);
        return 0;
    });
    hook(persistence, "addEntries", "([Ljava/lang/Object;)V", [](auto &b, auto &c) {
        const auto &items = b.jni.get(c.arguments[0]).elements;
        auto &store = b.receiver(c.receiver);
        for (size_t i = 0; i + 1 < items.size(); i += 2)
            b.set(store, items[i], items[i + 1]);
        b.save(store);
        return 0;
    });
    hook(persistence, "addEntriesFromMap", "(Ljava/util/Map;)V", [](auto &b, auto &c) {
        const auto &items = b.jni.get(c.arguments[0]).elements;
        auto &store = b.receiver(c.receiver);
        for (size_t i = 0; i + 1 < items.size(); i += 2)
            b.set(store, items[i], items[i + 1]);
        b.save(store);
        return 0;
    });
}
} // namespace acp::games

#include <acp/android.hpp>

#include <cmath>

namespace acp::android {
namespace {
struct Primitive {
    const char *name;
    char code;
};
constexpr Primitive primitives[] = {{"Boolean", 'Z'}, {"Byte", 'B'},  {"Short", 'S'},  {"Integer", 'I'},
                                    {"Long", 'J'},    {"Float", 'F'}, {"Double", 'D'}, {"Character", 'C'}};
double numeric(const Object &object) {
    auto bits = object.fields.at("bits").get<std::uint64_t>();
    switch (object.fields.at("kind").get<std::string>()[0]) {
    case 'D':
        return std::bit_cast<double>(bits);
    case 'F':
        return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
    case 'B':
        return static_cast<std::int8_t>(bits);
    case 'S':
        return static_cast<std::int16_t>(bits);
    case 'I':
        return static_cast<std::int32_t>(bits);
    case 'J':
        return static_cast<double>(static_cast<std::int64_t>(bits));
    case 'C':
        return static_cast<std::uint16_t>(bits);
    default:
        return bits != 0;
    }
}
std::int64_t integral(const Object &object, bool wide) {
    const auto kind = object.fields.at("kind").get<std::string>()[0];
    const auto bits = object.fields.at("bits").get<std::uint64_t>();
    if (kind == 'J')
        return wide ? static_cast<std::int64_t>(bits) : static_cast<std::int32_t>(bits);
    const auto value = numeric(object);
    if (std::isnan(value))
        return 0;
    const auto maximum = wide ? static_cast<double>(INT64_MAX) : static_cast<double>(INT32_MAX);
    const auto minimum = wide ? static_cast<double>(INT64_MIN) : static_cast<double>(INT32_MIN);
    if (value >= maximum)
        return wide ? INT64_MAX : INT32_MAX;
    if (value <= minimum)
        return wide ? INT64_MIN : INT32_MIN;
    return static_cast<std::int64_t>(value);
}
void primitive_fields(Object &object, char code, std::uint64_t bits) {
    object.fields["bits"] = bits;
    object.fields["kind"] = std::string(1, code);
    object.fields[object.class_name + "#value" + code] = bits;
}
} // namespace
void install_java_values(Runtime &r) {
    auto &j = *r.jni;
    j.hook("java/util/Date", "<init>", "()V", [&j](auto &c) {
        j.get(c.receiver).fields["milliseconds"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count();
        return 0;
    });
    for (auto name : {"<init>", "setTime"})
        j.hook("java/util/Date", name, "(J)V", [&j](auto &c) {
            j.get(c.receiver).fields["milliseconds"] = std::bit_cast<std::int64_t>(c.arguments[0]);
            return 0;
        });
    j.hook("java/util/Date", "getTime", "()J", [&j](auto &c) {
        return static_cast<Address>(
            j.get(c.receiver).fields.at("milliseconds").template get<std::int64_t>());
    });
    j.set_superclass("java/lang/Exception", "java/lang/Throwable");
    j.set_superclass("java/lang/RuntimeException", "java/lang/Exception");
    j.set_superclass("java/lang/Error", "java/lang/Throwable");
    for (const auto *type :
         {"java/lang/Throwable", "java/lang/Exception", "java/lang/RuntimeException", "java/lang/Error"}) {
        for (const auto *signature :
             {"()V", "(Ljava/lang/String;)V", "(Ljava/lang/String;Ljava/lang/Throwable;)V"})
            j.hook(type, "<init>", signature, [&j](auto &c) {
                auto &fields = j.get(c.receiver).fields;
                fields["message"] =
                    c.arguments.empty() || !c.arguments[0] ? Json(nullptr) : Json(j.text(c.arguments[0]));
                fields["cause"] = c.arguments.size() > 1 ? j.global_ref(c.arguments[1]) : 0;
                return 0;
            });
    }
    for (const auto *method : {"getMessage", "getLocalizedMessage"})
        j.hook("java/lang/Throwable", method, "()Ljava/lang/String;", [&j](auto &c) -> Address {
            const auto &fields = j.get(c.receiver).fields;
            return !fields.contains("message") || fields.at("message").is_null()
                       ? 0
                       : j.string(fields.at("message").template get<std::string>());
        });
    j.hook("java/lang/Throwable", "getCause", "()Ljava/lang/Throwable;",
           [&j](auto &c) { return j.local_ref(j.get(c.receiver).fields.value("cause", Address{})); });
    j.hook("java/lang/Throwable", "toString", "()Ljava/lang/String;", [&j](auto &c) {
        const auto &object = j.get(c.receiver);
        auto name = object.class_name;
        std::replace(name.begin(), name.end(), '/', '.');
        if (object.fields.contains("message") && !object.fields.at("message").is_null())
            name += ": " + object.fields.at("message").template get<std::string>();
        return j.string(name);
    });
    j.set_interfaces("java/lang/String",
                     {"java/io/Serializable", "java/lang/CharSequence", "java/lang/Comparable"});
    j.set_interfaces("java/lang/Number", {"java/io/Serializable"});
    for (const auto *interface :
         {"java/io/Serializable", "java/lang/Cloneable", "java/lang/CharSequence", "java/lang/Comparable",
          "java/lang/Iterable", "java/util/Map", "java/util/List", "java/util/Set", "java/util/Collection"})
        j.set_superclass(interface, "");
    j.set_interfaces("java/util/List", {"java/util/Collection"});
    j.set_interfaces("java/util/Set", {"java/util/Collection"});
    j.set_interfaces("java/util/Collection", {"java/lang/Iterable"});
    j.set_interfaces("java/util/HashMap", {"java/util/Map", "java/io/Serializable", "java/lang/Cloneable"});
    j.set_interfaces("java/util/ArrayList",
                     {"java/util/List", "java/io/Serializable", "java/lang/Cloneable"});
    j.hook("java/lang/Object", "getClass", "()Ljava/lang/Class;",
           [&j](auto &c) { return j.class_ref(j.get(c.receiver).class_name); });
    auto cache = std::make_shared<std::map<std::pair<std::string, std::uint64_t>, Address>>();
    for (auto primitive : primitives) {
        auto type = "java/lang/" + std::string(primitive.name);
        auto descriptor = std::string(1, primitive.code);
        if (primitive.code != 'Z' && primitive.code != 'C')
            j.set_superclass(type, "java/lang/Number");
        j.set_interfaces(type, {"java/io/Serializable", "java/lang/Comparable"});
        j.hook(type, "<init>", "(" + descriptor + ")V", [&j, primitive](auto &c) {
            primitive_fields(j.get(c.receiver), primitive.code, c.arguments[0]);
            return 0;
        });
        j.hook(type, "valueOf", "(" + descriptor + ")L" + type + ";",
               [&j, &r, cache, type, primitive](auto &c) {
                   std::lock_guard lock(r.state_mutex);
                   auto bits = c.arguments[0];
                   if (primitive.code == 'Z')
                       bits = bits != 0;
                   auto key = std::pair{type, bits};
                   const bool cached =
                       primitive.code == 'Z' ||
                       ((primitive.code == 'B' || primitive.code == 'S' || primitive.code == 'I' ||
                         primitive.code == 'J') &&
                        static_cast<std::int64_t>(bits) >= -128 && static_cast<std::int64_t>(bits) <= 127);
                   if (cached && cache->contains(key))
                       return j.local_ref(cache->at(key));
                   auto out = j.object(type);
                   primitive_fields(j.get(out), primitive.code, bits);
                   if (cached)
                       (*cache)[key] = j.global_ref(out);
                   return out;
               });
        for (auto [method, code] : std::vector<std::pair<std::string, char>>{{"intValue", 'I'},
                                                                             {"longValue", 'J'},
                                                                             {"floatValue", 'F'},
                                                                             {"doubleValue", 'D'},
                                                                             {"byteValue", 'B'},
                                                                             {"shortValue", 'S'}})
            if (primitive.code != 'Z' && primitive.code != 'C')
                j.hook(type, method, "()" + std::string(1, code), [&j, code](auto &c) -> std::uint64_t {
                    const auto &value = j.get(c.receiver);
                    if (code == 'D')
                        return std::bit_cast<std::uint64_t>(numeric(value));
                    if (code == 'F')
                        return std::bit_cast<std::uint32_t>(static_cast<float>(numeric(value)));
                    auto integer = integral(value, code == 'J');
                    if (code == 'B')
                        return static_cast<std::int8_t>(integer);
                    if (code == 'S')
                        return static_cast<std::int16_t>(integer);
                    return integer;
                });
    }
    j.hook("java/lang/Boolean", "booleanValue", "()Z", [&j](auto &c) {
        return j.get(c.receiver).fields.at("bits").template get<std::uint64_t>() != 0;
    });
    j.hook("java/lang/Character", "charValue", "()C", [&j](auto &c) {
        return j.get(c.receiver).fields.at("bits").template get<std::uint64_t>() & 65535;
    });
    j.hook("java/lang/Enum", "ordinal", "()I",
           [&j](auto &c) { return j.get(c.receiver).fields.at("ordinal").template get<std::uint64_t>(); });
    for (const auto *method : {"name", "toString"})
        j.hook("java/lang/Enum", method, "()Ljava/lang/String;", [&j](auto &c) {
            return j.string(j.get(c.receiver).fields.at("name").template get<std::string>());
        });
}
Json Jni::serialize(Address value) const {
    std::lock_guard lock(runtime_.state_mutex);
    std::set<const Object *> active;
    std::function<Json(Address, unsigned)> encode = [&](Address address, unsigned depth) -> Json {
        if (!address)
            return nullptr;
        require(depth < 64, "Java persistence value exceeds nesting limit");
        const auto &object = get(address);
        require(active.insert(&object).second, "Cyclic Java persistence values are unsupported");
        Json result{{"class", object.class_name}};
        if (object.class_name == "java/lang/String")
            result["text"] = text(address);
        else if (object.fields.contains("bits")) {
            result["bits"] = object.fields.at("bits");
            result["kind"] = object.fields.at("kind");
        } else if (object.class_name.starts_with('[') || object.class_name == "java/util/HashMap" ||
                   object.class_name == "java/util/ArrayList") {
            result["fields"] = object.fields;
            result["bytes"] = Json::array();
            for (auto byte : object.bytes)
                result["bytes"].push_back(std::to_integer<unsigned>(byte));
            result["elements"] = Json::array();
            for (auto element : object.elements)
                result["elements"].push_back(encode(element, depth + 1));
        } else
            throw Error("Unsupported serializable Java class: " + object.class_name);
        active.erase(&object);
        return result;
    };
    return encode(value, 0);
}
Address Jni::deserialize(const Json &value) {
    std::lock_guard lock(runtime_.state_mutex);
    std::function<Address(const Json &, unsigned)> decode = [&](const Json &data,
                                                                unsigned depth) -> Address {
        if (data.is_null())
            return 0;
        require(depth < 64, "Stored Java value exceeds nesting limit");
        auto type = data.at("class").get<std::string>();
        if (type == "java/lang/String")
            return string(data.at("text").get<std::string>());
        auto out = object(type);
        auto &value = get(out);
        if (data.contains("bits"))
            primitive_fields(value, data.at("kind").get<std::string>().at(0),
                             data.at("bits").get<std::uint64_t>());
        else {
            require(type.starts_with('[') || type == "java/util/HashMap" || type == "java/util/ArrayList",
                    "Invalid stored Java class");
            value.fields = data.at("fields");
            for (auto byte : data.at("bytes")) {
                auto number = byte.get<unsigned>();
                require(number <= 255, "Invalid stored Java array byte");
                value.bytes.push_back(static_cast<std::byte>(number));
            }
            for (const auto &element : data.at("elements"))
                value.elements.push_back(global_ref(decode(element, depth + 1)));
        }
        return out;
    };
    return decode(value, 0);
}
} // namespace acp::android

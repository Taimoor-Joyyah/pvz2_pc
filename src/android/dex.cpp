#include <acp/package.hpp>
namespace acp::android {
std::vector<NativeMethod> dex_native_methods(std::span<const std::byte> bytes) {
    BinaryView v(bytes);
    require(v.size() >= 112 && v.get<std::uint32_t>(0) == 0x0a786564, "Invalid DEX header");
    require(v.get<std::uint32_t>(32) == bytes.size() && v.get<std::uint32_t>(40) == 0x12345678,
            "Invalid DEX size or endianness");
    auto strings_n = v.get<std::uint32_t>(56), strings_o = v.get<std::uint32_t>(60),
         types_n = v.get<std::uint32_t>(64), types_o = v.get<std::uint32_t>(68),
         protos_n = v.get<std::uint32_t>(72), protos_o = v.get<std::uint32_t>(76),
         methods_n = v.get<std::uint32_t>(88), methods_o = v.get<std::uint32_t>(92),
         classes_n = v.get<std::uint32_t>(96), classes_o = v.get<std::uint32_t>(100);
    require(strings_n < 2'000'000 && types_n < 1'000'000 && methods_n < 2'000'000 && classes_n < 1'000'000,
            "Excessive DEX table count");
    v.slice(strings_o, std::uint64_t(strings_n) * 4);
    v.slice(types_o, std::uint64_t(types_n) * 4);
    v.slice(methods_o, std::uint64_t(methods_n) * 8);
    v.slice(classes_o, std::uint64_t(classes_n) * 32);
    v.slice(protos_o, std::uint64_t(protos_n) * 12);
    auto uleb = [&](std::uint64_t &p) {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            auto b = v.get<std::uint8_t>(p++);
            require(shift != 28 || b < 16, "DEX ULEB overflow");
            value |= std::uint32_t(b & 127) << shift;
            if (!(b & 128))
                return value;
        }
        throw Error("Invalid DEX ULEB");
    };
    std::vector<std::string> strings;
    strings.reserve(strings_n);
    for (size_t i = 0; i < strings_n; ++i) {
        std::uint64_t p = v.get<std::uint32_t>(strings_o + i * 4);
        uleb(p);
        strings.push_back(v.cstring(p));
    }
    auto string_at = [&](std::uint32_t i) -> const std::string & {
        require(i < strings.size(), "Invalid DEX string index");
        return strings[i];
    };
    std::vector<std::string> types;
    types.reserve(types_n);
    for (size_t i = 0; i < types_n; ++i)
        types.push_back(string_at(v.get<std::uint32_t>(types_o + i * 4)));
    auto type_at = [&](std::uint32_t i) -> const std::string & {
        require(i < types.size(), "Invalid DEX type index");
        return types[i];
    };
    auto signature = [&](std::uint32_t index) {
        require(index < protos_n, "Invalid DEX prototype");
        auto p = protos_o + index * 12;
        auto args = v.get<std::uint32_t>(p + 8);
        std::string text = "(";
        if (args) {
            auto n = v.get<std::uint32_t>(args);
            require(n < 65536, "Invalid DEX argument count");
            for (size_t i = 0; i < n; ++i)
                text += type_at(v.get<std::uint16_t>(args + 4 + i * 2));
        }
        return text + ")" + type_at(v.get<std::uint32_t>(p + 4));
    };
    std::vector<NativeMethod> result;
    for (size_t i = 0; i < classes_n; ++i) {
        std::uint64_t p = v.get<std::uint32_t>(classes_o + i * 32 + 24);
        if (!p)
            continue;
        auto sf = uleb(p), inf = uleb(p), dm = uleb(p), vm = uleb(p);
        require(std::uint64_t(sf) + inf + dm + vm < 4'000'000, "Invalid DEX class member counts");
        for (std::uint64_t n = 0; n < std::uint64_t(sf) + inf; ++n) {
            uleb(p);
            uleb(p);
        }
        for (auto count : {dm, vm}) {
            std::uint64_t index = 0;
            for (size_t n = 0; n < count; ++n) {
                index += uleb(p);
                auto flags = uleb(p);
                uleb(p);
                require(index < methods_n, "Invalid DEX method index");
                if (flags & 0x100) {
                    auto method = methods_o + index * 8;
                    result.push_back({type_at(v.get<std::uint16_t>(method)),
                                      string_at(v.get<std::uint32_t>(method + 4)),
                                      signature(v.get<std::uint16_t>(method + 2))});
                }
            }
        }
    }
    return result;
}
} // namespace acp::android

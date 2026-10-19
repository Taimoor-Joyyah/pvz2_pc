#include <acp/package.hpp>
namespace acp::android {
std::vector<JavaConstant> dex_constants(std::span<const std::byte> bytes) {
    BinaryView v(bytes);
    require(v.size() >= 112 && v.get<std::uint32_t>(0) == 0x0a786564, "Invalid DEX header");
    require(v.get<std::uint32_t>(32) == bytes.size() && v.get<std::uint32_t>(40) == 0x12345678,
            "Invalid DEX size or endianness");
    auto strings_n = v.get<std::uint32_t>(56), strings_o = v.get<std::uint32_t>(60);
    auto types_n = v.get<std::uint32_t>(64), types_o = v.get<std::uint32_t>(68);
    auto fields_n = v.get<std::uint32_t>(80), fields_o = v.get<std::uint32_t>(84);
    auto classes_n = v.get<std::uint32_t>(96), classes_o = v.get<std::uint32_t>(100);
    require(strings_n < 2'000'000 && types_n < 1'000'000 && fields_n < 2'000'000 && classes_n < 1'000'000,
            "Excessive DEX constant table size");
    v.slice(strings_o, std::uint64_t(strings_n) * 4);
    v.slice(types_o, std::uint64_t(types_n) * 4);
    v.slice(fields_o, std::uint64_t(fields_n) * 8);
    v.slice(classes_o, std::uint64_t(classes_n) * 32);
    auto uleb = [&](std::uint64_t &p) {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            auto byte = v.get<std::uint8_t>(p++);
            require(shift != 28 || byte < 16, "DEX constant ULEB overflow");
            value |= std::uint32_t(byte & 127) << shift;
            if (!(byte & 128))
                return value;
        }
        throw Error("Invalid DEX constant ULEB");
    };
    auto string = [&](unsigned index) {
        require(index < strings_n, "Invalid DEX constant string index");
        std::uint64_t offset = v.get<std::uint32_t>(strings_o + index * 4ull);
        uleb(offset);
        return v.cstring(offset);
    };
    auto type = [&](unsigned index) {
        require(index < types_n, "Invalid DEX constant type index");
        return string(v.get<std::uint32_t>(types_o + index * 4ull));
    };
    std::function<Json(std::uint64_t &, unsigned)> value = [&](std::uint64_t &p, unsigned depth) -> Json {
        require(depth < 32, "DEX encoded value nesting limit");
        auto header = v.get<std::uint8_t>(p++);
        auto kind = header & 31, argument = header >> 5;
        if (kind == 0x1f) {
            require(argument < 2, "Invalid encoded boolean");
            return std::uint64_t(argument);
        }
        if (kind == 0x1e) {
            require(!argument, "Invalid encoded null");
            return nullptr;
        }
        if (kind == 0x1c || kind == 0x1d) {
            require(!argument, "Invalid encoded container value");
            if (kind == 0x1d)
                uleb(p);
            auto count = uleb(p);
            require(count < 2'000'000, "Excessive DEX encoded values");
            for (size_t i = 0; i < count; ++i) {
                if (kind == 0x1d)
                    uleb(p);
                value(p, depth + 1);
            }
            return nullptr;
        }
        unsigned width = argument + 1;
        unsigned limit = (kind == 0 ? 1 : kind == 2 || kind == 3 ? 2 : kind == 6 || kind == 0x11 ? 8 : 4);
        require(width <= limit, "Invalid DEX encoded scalar width");
        std::uint64_t bits = 0;
        for (unsigned i = 0; i < width; ++i)
            bits |= std::uint64_t(v.get<std::uint8_t>(p++)) << (i * 8);
        if (kind == 0x17)
            return string(static_cast<unsigned>(bits));
        if (kind == 0x10 || kind == 0x11)
            return bits << ((limit - width) * 8);
        if (kind == 0 || kind == 2 || kind == 4 || kind == 6) {
            if (width < 8 && (bits & (1ull << (width * 8 - 1))))
                bits |= UINT64_MAX << (width * 8);
            return bits;
        }
        if (kind == 3)
            return bits;
        require(kind == 0x15 || kind == 0x16 || kind == 0x18 || kind == 0x19 || kind == 0x1a ||
                    kind == 0x1b,
                "Unknown DEX encoded value type");
        return nullptr;
    };
    std::vector<JavaConstant> result;
    for (size_t i = 0; i < classes_n; ++i) {
        auto record = classes_o + i * 32ull;
        std::uint64_t fields = v.get<std::uint32_t>(record + 24),
                      values = v.get<std::uint32_t>(record + 28);
        if (!fields || !values)
            continue;
        auto count = uleb(fields);
        uleb(fields);
        uleb(fields);
        uleb(fields);
        auto initialized = uleb(values);
        require(initialized <= count && count < 2'000'000, "Invalid DEX static field value count");
        std::uint64_t index = 0;
        for (size_t n = 0; n < initialized; ++n) {
            index += uleb(fields);
            auto flags = uleb(fields);
            require(index < fields_n, "Invalid DEX static field index");
            auto initial = value(values, 0);
            if (!(flags & 0x10) || initial.is_null())
                continue;
            auto field = fields_o + index * 8;
            auto signature = type(v.get<std::uint16_t>(field + 2));
            if (signature != "Ljava/lang/String;" &&
                (signature.size() != 1 ||
                 std::string_view("ZBCSIJFD").find(signature[0]) == std::string_view::npos))
                continue;
            result.push_back({type(v.get<std::uint16_t>(field)), string(v.get<std::uint32_t>(field + 4)),
                              std::move(signature), std::move(initial)});
        }
    }
    return result;
}
} // namespace acp::android

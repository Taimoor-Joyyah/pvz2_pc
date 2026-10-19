#include <acp/guest.hpp>

#include <set>
namespace acp::guest {
ElfImage::ElfImage(Bytes bytes) : bytes_(std::move(bytes)) {
    BinaryView v(bytes_);
    require(v.size() >= 64, "Truncated ELF header");
    require(v.get<std::uint32_t>(0) == 0x464c457f && v.get<std::uint8_t>(4) == 2 &&
                v.get<std::uint8_t>(5) == 1,
            "Only ELF64 little-endian libraries are supported");
    require(v.get<std::uint16_t>(18) == 183 && v.get<std::uint16_t>(16) == 3,
            "Expected an AArch64 ELF shared library");
    auto phoff = v.get<std::uint64_t>(32), shoff = v.get<std::uint64_t>(40);
    auto phsize = v.get<std::uint16_t>(54), phcount = v.get<std::uint16_t>(56),
         shsize = v.get<std::uint16_t>(58), shcount = v.get<std::uint16_t>(60);
    require(phsize >= 56 && phcount > 0 && shsize >= 64 && shcount > 0,
            "ELF program and section tables are required");
    for (size_t i = 0; i < phcount; ++i) {
        auto p = phoff + i * phsize;
        Segment s{v.get<std::uint32_t>(p),      v.get<std::uint32_t>(p + 4),  v.get<std::uint64_t>(p + 8),
                  v.get<std::uint64_t>(p + 16), v.get<std::uint64_t>(p + 32), v.get<std::uint64_t>(p + 40),
                  v.get<std::uint64_t>(p + 48)};
        require(s.file_size <= s.memory_size || s.type != 1, "Invalid ELF segment size");
        v.slice(s.offset, s.file_size);
        require(s.address <= UINT64_MAX - s.memory_size, "ELF segment address overflow");
        segments_.push_back(s);
    }
    auto offset_of = [&](std::uint64_t address) {
        for (const auto &s : segments_)
            if (address >= s.address && address - s.address < s.file_size)
                return s.offset + address - s.address;
        throw Error("ELF dynamic pointer is outside file segments");
    };
    std::vector<std::uint64_t> needed_offsets;
    for (const auto &s : segments_)
        if (s.type == 2) {
            require(s.file_size % 16 == 0, "Invalid ELF dynamic table");
            for (auto p = s.offset; p < s.offset + s.file_size; p += 16) {
                auto tag = v.get<std::int64_t>(p);
                if (!tag)
                    break;
                auto value = v.get<std::uint64_t>(p + 8);
                if (tag == 1)
                    needed_offsets.push_back(value);
                else
                    dynamic_[tag] = value;
            }
        }
    require(dynamic_.contains(5) && dynamic_.contains(10), "ELF dynamic string table missing");
    auto strbase = offset_of(dynamic_.at(5));
    auto strsize = dynamic_.at(10);
    v.slice(strbase, strsize);
    auto string_at = [&](std::uint64_t offset) {
        require(offset < strsize, "Invalid ELF string index");
        return v.cstring(strbase + offset, static_cast<size_t>(strsize - offset));
    };
    for (auto offset : needed_offsets)
        needed_.push_back(string_at(offset));
    bool found_symbols = false;
    for (size_t i = 0; i < shcount; ++i) {
        auto s = shoff + i * shsize;
        auto type = v.get<std::uint32_t>(s + 4);
        auto offset = v.get<std::uint64_t>(s + 24);
        auto size = v.get<std::uint64_t>(s + 32);
        auto stride = v.get<std::uint64_t>(s + 56);
        if (type == 11) {
            require(!found_symbols && stride == 24 && size % stride == 0 && size / stride < 2'000'000,
                    "Invalid ELF dynamic symbol table");
            found_symbols = true;
            for (auto p = offset; p < offset + size; p += stride) {
                auto info = v.get<std::uint8_t>(p + 4);
                symbols_.push_back({string_at(v.get<std::uint32_t>(p)), v.get<std::uint64_t>(p + 8),
                                    v.get<std::uint64_t>(p + 16), v.get<std::uint16_t>(p + 6),
                                    static_cast<std::uint8_t>(info >> 4),
                                    static_cast<std::uint8_t>(info & 15), v.get<std::uint8_t>(p + 5)});
            }
        } else if (type == 4) {
            require(stride == 24 && size % stride == 0 && size / stride < 4'000'000,
                    "Invalid ELF relocation table");
            for (auto p = offset; p < offset + size; p += stride) {
                auto info = v.get<std::uint64_t>(p + 8);
                relocations_.push_back({v.get<std::uint64_t>(p), static_cast<std::uint32_t>(info >> 32),
                                        static_cast<std::uint32_t>(info), v.get<std::int64_t>(p + 16)});
            }
        } else if (type == 19 || type == 0x6fffff00 || type == 0x60000001 || type == 0x60000002) {
            if (size)
                throw Error("Packed ELF relocations are not implemented for this binary");
        }
    }
    require(found_symbols, "ELF dynamic symbols are missing");
    for (const auto &r : relocations_)
        require(r.symbol < symbols_.size(), "Invalid ELF relocation symbol");
    require(image_size() < (1ull << 30), "ELF image is too large");
}
std::optional<std::uint64_t> ElfImage::dynamic(std::int64_t tag) const {
    auto it = dynamic_.find(tag);
    return it == dynamic_.end() ? std::nullopt : std::optional(it->second);
}
std::uint64_t ElfImage::image_size() const {
    std::uint64_t size = 0;
    for (const auto &s : segments_)
        if (s.type == 1)
            size = std::max(size, s.address + s.memory_size);
    return align_up(size, 4096);
}
Json ElfImage::describe(bool include_symbols) const {
    Json j{{"abi", "arm64-v8a"},         {"image_size", image_size()},   {"needed", needed_},
           {"undefined", Json::array()}, {"jni_exports", Json::array()}, {"relocations", Json::object()}};
    for (const auto &s : symbols_) {
        if (!s.defined() && !s.name.empty())
            j["undefined"].push_back({{"name", s.name}, {"weak", s.binding == 2}, {"type", s.type}});
        if (s.defined() && (s.name == "JNI_OnLoad" || s.name.starts_with("Java_")))
            j["jni_exports"].push_back(s.name);
    }
    std::map<unsigned, size_t> counts;
    for (const auto &r : relocations_)
        ++counts[r.type];
    for (const auto &[type, count] : counts)
        j["relocations"][std::to_string(type)] = count;
    j["symbol_count"] = symbols_.size();
    if (include_symbols) {
        j["exports"] = Json::array();
        for (const auto &s : symbols_)
            if (s.defined() && s.binding)
                j["exports"].push_back(s.name);
    }
    return j;
}
} // namespace acp::guest

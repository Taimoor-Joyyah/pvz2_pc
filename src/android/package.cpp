#include <acp/guest.hpp>
#include <acp/package.hpp>

#include <set>
namespace acp::android {
Json Manifest::json() const {
    return {{"package", package},       {"version_name", version_name}, {"version_code", version_code},
            {"split", split},           {"activity", activity},         {"min_sdk", min_sdk},
            {"target_sdk", target_sdk}, {"metadata", metadata}};
}
Manifest parse_manifest(std::span<const std::byte> bytes) {
    BinaryView v(bytes);
    require(v.get<std::uint16_t>(0) == 3 && v.get<std::uint32_t>(4) == bytes.size(),
            "Expected binary Android XML manifest");
    std::vector<std::string> strings;
    Manifest result;
    std::string activity;
    std::vector<std::string> elements;
    auto str = [&](std::uint32_t i) -> std::string {
        require(i < strings.size(), "Invalid Android XML string index");
        return strings[i];
    };
    for (std::uint64_t p = v.get<std::uint16_t>(2); p < bytes.size();) {
        auto type = v.get<std::uint16_t>(p), header = v.get<std::uint16_t>(p + 2);
        auto length = v.get<std::uint32_t>(p + 4);
        require(header >= 8 && length >= header, "Invalid Android XML chunk");
        v.slice(p, length);
        if (type == 1) {
            require(header >= 28, "Invalid Android string pool");
            auto count = v.get<std::uint32_t>(p + 8), flags = v.get<std::uint32_t>(p + 16),
                 start = v.get<std::uint32_t>(p + 20);
            require(count < 200000 && std::uint64_t(header) + std::uint64_t(count) * 4 <= length,
                    "Invalid Android string count");
            strings.clear();
            for (size_t i = 0; i < count; ++i) {
                auto pos = p + start + v.get<std::uint32_t>(p + header + i * 4);
                require(pos < p + length, "Invalid Android string offset");
                if (flags & 0x100) {
                    auto len8 = [&]() {
                        auto first = v.get<std::uint8_t>(pos++);
                        return first & 0x80 ? ((first & 0x7f) << 8) | v.get<std::uint8_t>(pos++) : first;
                    };
                    len8();
                    auto n = len8();
                    auto data = v.slice(pos, n);
                    require(pos + n < p + length && v.get<std::uint8_t>(pos + n) == 0,
                            "Invalid Android UTF8 string");
                    strings.emplace_back(reinterpret_cast<const char *>(data.data()), data.size());
                } else {
                    auto n = std::uint32_t(v.get<std::uint16_t>(pos));
                    pos += 2;
                    if (n & 0x8000) {
                        n = ((n & 0x7fff) << 16) | v.get<std::uint16_t>(pos);
                        pos += 2;
                    }
                    auto data = v.slice(pos, std::uint64_t(n) * 2);
                    require(pos + std::uint64_t(n) * 2 + 2 <= p + length, "Invalid Android UTF16 string");
                    std::wstring wide(n, 0);
                    std::memcpy(wide.data(), data.data(), data.size());
                    strings.push_back(narrow(wide));
                }
            }
        } else if (type == 0x102) {
            require(header >= 16 && length >= 36, "Invalid Android XML element");
            auto tag = str(v.get<std::uint32_t>(p + 20));
            auto start = v.get<std::uint16_t>(p + 24), stride = v.get<std::uint16_t>(p + 26),
                 count = v.get<std::uint16_t>(p + 28);
            require(stride >= 20 && 16ull + start + std::uint64_t(count) * stride <= length,
                    "Invalid Android attributes");
            std::map<std::string, std::string> attributes;
            Json typed = Json::object();
            for (size_t i = 0; i < count; ++i) {
                auto a = p + 16 + start + i * stride;
                auto name = str(v.get<std::uint32_t>(a + 4));
                auto raw = v.get<std::uint32_t>(a + 8), data = v.get<std::uint32_t>(a + 16);
                auto kind = v.get<std::uint8_t>(a + 15);
                attributes[name] = raw != UINT32_MAX ? str(raw)
                                   : kind == 3       ? str(data)
                                                     : std::to_string(data);
                if (kind == 3)
                    typed[name] = str(data);
                else if (kind == 0x12)
                    typed[name] = data != 0;
                else if (kind == 0x10 || kind == 0x11)
                    typed[name] = static_cast<std::int32_t>(data);
                else if (kind == 1)
                    typed[name] = {{"resource_id", data}};
                else
                    typed[name] = attributes[name];
            }
            auto number = [&](const char *key) {
                auto it = attributes.find(key);
                return it == attributes.end() ? 0ull : std::stoull(it->second);
            };
            if (tag == "manifest") {
                result.package = attributes["package"];
                result.version_name = attributes["versionName"];
                result.version_code = number("versionCode");
                result.split = attributes["split"];
            }
            if (tag == "uses-sdk") {
                result.min_sdk = static_cast<unsigned>(number("minSdkVersion"));
                result.target_sdk = static_cast<unsigned>(number("targetSdkVersion"));
            }
            if (tag == "activity" || tag == "activity-alias")
                activity = attributes["name"];
            if (tag == "action" && attributes["name"] == "android.intent.action.MAIN")
                result.activity = activity;
            if (tag == "meta-data" && !elements.empty() && elements.back() == "application" &&
                attributes.contains("name")) {
                if (typed.contains("value"))
                    result.metadata[attributes["name"]] = typed["value"];
                else if (typed.contains("resource"))
                    result.metadata[attributes["name"]] = typed["resource"];
            }
            elements.push_back(tag);
        } else if (type == 0x103) {
            require(!elements.empty(), "Unbalanced Android XML end element");
            elements.pop_back();
        }
        p += length;
    }
    require(!result.package.empty() && result.package.find('/') == std::string::npos &&
                result.package.find('\\') == std::string::npos &&
                result.package.find(':') == std::string::npos && result.package != "..",
            "Invalid Android package identity");
    if (result.activity.starts_with('.'))
        result.activity = result.package + result.activity;
    return result;
}
Json NativeMethod::json() const {
    return {{"class", class_name}, {"name", name}, {"signature", signature}};
}
Json PackageInfo::json() const {
    auto j = manifest.json();
    j["sha256"] = digest;
    j["abis"] = abis;
    j["libraries"] = libraries;
    j["native_methods"] = Json::array();
    for (const auto &m : native_methods)
        j["native_methods"].push_back(m.json());
    return j;
}
PackageInfo inspect_package(const fs::path &path) {
    Archive archive(path);
    PackageInfo result;
    result.manifest = parse_manifest(archive.read("AndroidManifest.xml"));
    result.digest = sha256(path);
    std::set<std::string> abis;
    for (const auto &name : archive.names()) {
        if (name.starts_with("lib/") && name.ends_with(".so")) {
            auto end = name.find('/', 4);
            if (end == name.npos)
                continue;
            auto abi = name.substr(4, end - 4);
            abis.insert(abi);
            if (abi == "arm64-v8a") {
                auto bytes = archive.read(name);
                guest::ElfImage elf(bytes);
                auto j = elf.describe();
                j["sha256"] = sha256(bytes);
                result.libraries[name.substr(end + 1)] = std::move(j);
            }
        }
        if (name.starts_with("classes") && name.ends_with(".dex") && name.find('/') == name.npos) {
            auto methods = dex_native_methods(archive.read(name));
            result.native_methods.insert(result.native_methods.end(), methods.begin(), methods.end());
        }
    }
    result.abis.assign(abis.begin(), abis.end());
    return result;
}
} // namespace acp::android

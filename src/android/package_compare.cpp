#include <acp/package.hpp>

#include <set>
namespace acp::android {
namespace {
Json difference(const std::set<std::string> &before, const std::set<std::string> &after) {
    std::vector<std::string> added, removed;
    std::ranges::set_difference(after, before, std::back_inserter(added));
    std::ranges::set_difference(before, after, std::back_inserter(removed));
    return {{"added", added}, {"removed", removed}};
}
std::set<std::string> symbols(const Json &values, bool objects = false) {
    std::set<std::string> out;
    for (const auto &value : values)
        out.insert(objects ? value.at("name").get<std::string>() : value.get<std::string>());
    return out;
}
std::set<std::string> native_methods(const Json &values) {
    std::set<std::string> out;
    for (const auto &method : values)
        out.insert(method.at("class").get<std::string>() + "#" + method.at("name").get<std::string>() +
                   method.at("signature").get<std::string>());
    return out;
}
} // namespace
Json inspect_package_set(const std::vector<fs::path> &paths) {
    require(!paths.empty(), "Inspection requires at least one APK");
    std::vector<PackageInfo> packages;
    for (const auto &path : paths)
        packages.push_back(inspect_package(path));
    const PackageInfo *base = nullptr;
    std::set<std::string> splits, abis;
    std::map<std::string, std::string> fingerprints;
    for (const auto &package : packages) {
        require(splits.insert(package.manifest.split).second, "Duplicate APK split identity");
        if (package.manifest.split.empty())
            base = &package;
        fingerprints[package.manifest.split] = package.digest;
    }
    require(base, "APK set has no base APK");
    Json result = base->json();
    result["apks"] = Json::array();
    result["native_methods"] = Json::array();
    for (size_t i = 0; i < packages.size(); ++i) {
        const auto &package = packages[i];
        require(package.manifest.package == base->manifest.package &&
                    package.manifest.version_code == base->manifest.version_code,
                "APK splits have different package identities or versions");
        result["apks"].push_back({{"file", utf8(paths[i].filename())},
                                  {"split", package.manifest.split},
                                  {"sha256", package.digest}});
        for (const auto &[name, lib] : package.libraries.items()) {
            require(!result["libraries"].contains(name) ||
                        result["libraries"][name]["sha256"] == lib["sha256"],
                    "APK splits contain conflicting native libraries");
            result["libraries"][name] = lib;
        }
        for (const auto &method : package.native_methods)
            result["native_methods"].push_back(method.json());
        abis.insert(package.abis.begin(), package.abis.end());
    }
    result["abis"] = abis;
    std::string fingerprint;
    for (const auto &[_, digest] : fingerprints)
        fingerprint += digest;
    result["package_set_sha256"] = sha256(std::as_bytes(std::span(fingerprint)));
    result["support_status"] = "unverified";
    return result;
}
Json compare_package_contracts(const Json &previous, const Json &current) {
    const auto &old_manifest = previous.contains("manifest") ? previous.at("manifest") : previous;
    const auto &new_manifest = current.contains("manifest") ? current.at("manifest") : current;
    require(old_manifest.at("package") == new_manifest.at("package"),
            "Cannot compare different Android packages");
    Json changes = Json::object();
    const auto &old_libraries = previous.at("libraries"), &new_libraries = current.at("libraries");
    std::set<std::string> names, old_imports, new_imports;
    for (const auto &[name, _] : old_libraries.items())
        names.insert(name);
    for (const auto &[name, _] : new_libraries.items())
        names.insert(name);
    for (const auto &name : names) {
        auto old = old_libraries.find(name), next = new_libraries.find(name);
        if (old != old_libraries.end()) {
            auto imports = symbols(old->at("undefined"), true);
            old_imports.insert(imports.begin(), imports.end());
        }
        if (next != new_libraries.end()) {
            auto imports = symbols(next->at("undefined"), true);
            new_imports.insert(imports.begin(), imports.end());
        }
        if (old == old_libraries.end())
            changes[name] = {{"status", "added"}, {"sha256", next->at("sha256")}};
        else if (next == new_libraries.end())
            changes[name] = {{"status", "removed"}, {"sha256", old->at("sha256")}};
        else
            changes[name] = {
                {"status", old->at("sha256") == next->at("sha256") ? "unchanged" : "changed"},
                {"previous_sha256", old->at("sha256")},
                {"sha256", next->at("sha256")},
                {"imports",
                 difference(symbols(old->at("undefined"), true), symbols(next->at("undefined"), true))},
                {"dependencies", difference(symbols(old->at("needed")), symbols(next->at("needed")))},
                {"jni_exports",
                 difference(symbols(old->at("jni_exports")), symbols(next->at("jni_exports")))}};
    }
    return {{"previous_version", old_manifest.at("version_name")},
            {"version", new_manifest.at("version_name")},
            {"libraries", changes},
            {"native_imports", difference(old_imports, new_imports)},
            {"native_methods", difference(native_methods(previous.at("native_methods")),
                                          native_methods(current.at("native_methods")))},
            {"support_status", "unverified"},
            {"regression_testing_required", true}};
}
} // namespace acp::android

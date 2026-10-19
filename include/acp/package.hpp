#pragma once
#include <acp/common.hpp>
namespace acp::android {
struct Manifest {
    std::string package, version_name, split, activity;
    std::uint64_t version_code{};
    unsigned min_sdk{}, target_sdk{};
    Json metadata = Json::object();
    Json json() const;
};
Manifest parse_manifest(std::span<const std::byte> bytes);
struct NativeMethod {
    std::string class_name, name, signature;
    Json json() const;
};
std::vector<NativeMethod> dex_native_methods(std::span<const std::byte> bytes);
struct JavaConstant {
    std::string class_name, name, signature;
    Json value;
};
std::vector<JavaConstant> dex_constants(std::span<const std::byte> bytes);
struct PackageInfo {
    Manifest manifest;
    std::vector<std::string> abis;
    Json libraries = Json::object();
    std::vector<NativeMethod> native_methods;
    std::string digest;
    Json json() const;
};
PackageInfo inspect_package(const fs::path &path);
Json inspect_package_set(const std::vector<fs::path> &paths);
Json compare_package_contracts(const Json &previous, const Json &current);
struct Installation {
    fs::path root, directory, data_directory;
    Json metadata;
    std::string id() const;
    std::string package() const;
    fs::path library(std::string_view name) const;
};
Installation import_package(const fs::path &root, const std::vector<fs::path> &apks, const fs::path &obb,
                            const std::optional<fs::path> &data = {});
Installation open_installation(const fs::path &root, std::optional<std::string> id = {});
Json installation_list(const fs::path &root);
void backup_before_launch(const Installation &installation);
} // namespace acp::android

#include <acp/host.hpp>
#include <acp/package.hpp>

#include <set>
namespace acp::android {
std::string Installation::id() const {
    return metadata.at("id").get<std::string>();
}
std::string Installation::package() const {
    return metadata.at("manifest").at("package").get<std::string>();
}
fs::path Installation::library(std::string_view name) const {
    require(is_safe_relative(name) && path_from_utf8(name).filename() == path_from_utf8(name),
            "Invalid library name");
    return directory / "lib" / path_from_utf8(name);
}
namespace {
void copy_tree(const fs::path &source, const fs::path &destination) {
    require(fs::is_directory(source), "Downloaded data directory does not exist");
    auto input = fs::weakly_canonical(source), output = fs::weakly_canonical(destination);
    auto relative = output.lexically_relative(input);
    require(relative.empty() || *relative.begin() == "..",
            "Data destination must not be inside the import source");
    fs::create_directories(destination);
    for (const auto &item : fs::recursive_directory_iterator(source)) {
        require(!item.is_symlink() && !fs::is_other(item.symlink_status()),
                "Linked or special files are not accepted in imported data");
        auto rel = item.path().lexically_relative(source);
        auto target = destination / rel;
        if (item.is_directory())
            fs::create_directories(target);
        else if (item.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(item.path(), target, fs::copy_options::none);
        }
    }
}
void import_downloaded_data(const fs::path &source, const fs::path &user_data, std::string_view package) {
    require(fs::is_directory(source), "Downloaded data directory does not exist");
    auto destination = user_data / "external" / "Android" / "data" / path_from_utf8(package);
    auto input = fs::weakly_canonical(source), output = fs::weakly_canonical(destination);
    auto relative = output.lexically_relative(input);
    require(relative.empty() || *relative.begin() == "..",
            "Data destination must not be inside the import source");
    std::vector<std::pair<fs::path, fs::path>> files;
    // Check every collision before copying. Re-imports may add content but never replace saves.
    for (const auto &item : fs::recursive_directory_iterator(source)) {
        require(!item.is_symlink() && !fs::is_other(item.symlink_status()),
                "Linked or special files are not accepted in imported data");
        auto target = destination / item.path().lexically_relative(source);
        auto resolved = fs::weakly_canonical(target).lexically_relative(output);
        require(!resolved.empty() && *resolved.begin() != "..",
                "Downloaded data target escapes its destination");
        if (item.is_directory()) {
            require(!fs::exists(target) || fs::is_directory(target),
                    "Downloaded directory conflicts with an existing file: " + utf8(target));
        } else if (item.is_regular_file()) {
            if (fs::exists(target)) {
                require(fs::is_regular_file(target) && fs::file_size(target) == item.file_size() &&
                            sha256(target) == sha256(item.path()),
                        "Downloaded data conflicts with existing content; use a new --root to preserve "
                        "saves: " +
                            utf8(target));
            } else
                files.emplace_back(item.path(), std::move(target));
        }
    }
    for (const auto &[file, target] : files) {
        fs::create_directories(target.parent_path());
        fs::copy_file(file, target, fs::copy_options::none);
    }
}
} // namespace
Installation import_package(const fs::path &root, const std::vector<fs::path> &apks, const fs::path &obb,
                            const std::optional<fs::path> &data) {
    require(!apks.empty(), "Import requires at least one APK");
    require(obb.empty() || fs::is_regular_file(obb), "OBB file does not exist");
    std::vector<PackageInfo> packages;
    for (const auto &path : apks)
        packages.push_back(inspect_package(path));
    const PackageInfo *base = nullptr;
    std::set<std::string> splits;
    for (const auto &package : packages) {
        require(splits.insert(package.manifest.split).second, "Duplicate APK split identity");
        if (package.manifest.split.empty()) {
            require(!base, "Multiple base APKs");
            base = &package;
        }
    }
    require(base != nullptr, "APK set has no base APK");
    for (const auto &package : packages)
        require(package.manifest.package == base->manifest.package &&
                    package.manifest.version_code == base->manifest.version_code,
                "APK splits have different package identities or versions");
    auto expected = std::format("main.{}.{}.obb", base->manifest.version_code, base->manifest.package);
    require(obb.empty() || utf8(obb.filename()) == expected,
            "OBB filename must match the imported package: " + expected);
    auto size = obb.empty() ? 0 : fs::file_size(obb);
    require(obb.empty() || size > 16, "OBB is empty or truncated");
    std::string fingerprints;
    std::map<std::string, std::string> ordered_fingerprints;
    for (const auto &p : packages)
        ordered_fingerprints[p.manifest.split] = p.digest;
    for (const auto &[_, digest] : ordered_fingerprints)
        fingerprints += digest;
    auto digest = sha256(std::as_bytes(std::span(fingerprints)));
    auto id =
        std::format("{}-{}-{}", base->manifest.package, base->manifest.version_code, digest.substr(0, 12));
    auto absolute_root = fs::absolute(root).lexically_normal(),
         directory = absolute_root / "installations" / path_from_utf8(id);
    if (fs::exists(directory)) {
        auto existing = read_json(directory / "installation.json");
        require(obb.empty()
                    ? existing.at("obb").is_null()
                    : !existing.at("obb").is_null() && existing.at("obb").at("sha256") == sha256(obb),
                "Existing installation has different OBB content; use a separate --root");
        auto user_data = absolute_root / "userdata" / path_from_utf8(base->manifest.package);
        if (data)
            import_downloaded_data(*data, user_data, base->manifest.package);
        write_json(absolute_root / "active.json", {{"id", id}});
        return {absolute_root, directory,
                absolute_root / "userdata" / path_from_utf8(base->manifest.package), std::move(existing)};
    }
    fs::create_directories(absolute_root / "installations");
    auto staging = directory;
    staging += path_from_utf8(".staging-" + timestamp());
    require(fs::create_directory(staging), "An import staging directory already exists");
    fs::create_directories(staging / "lib");
    fs::create_directories(staging / "apks");
    fs::create_directories(staging / "obb");
    Json metadata{{"schema", 1},
                  {"id", id},
                  {"manifest", base->manifest.json()},
                  {"imported_at", timestamp()},
                  {"status", "unverified"},
                  {"obb", nullptr},
                  {"apks", Json::array()},
                  {"libraries", Json::object()},
                  {"native_methods", Json::array()}};
    std::map<std::string, std::string> libraries;
    try {
        for (size_t i = 0; i < apks.size(); ++i) {
            const auto &package = packages[i];
            auto apk_name = std::format("{:03}.apk", i);
            fs::copy_file(apks[i], staging / "apks" / apk_name);
            require(sha256(staging / "apks" / apk_name) == package.digest, "APK changed during import");
            metadata["apks"].push_back({{"file", "apks/" + apk_name},
                                        {"sha256", package.digest},
                                        {"split", package.manifest.split}});
            for (const auto &method : package.native_methods)
                metadata["native_methods"].push_back(method.json());
            Archive archive(apks[i]);
            for (const auto &name : archive.names())
                if (name.starts_with("lib/arm64-v8a/") && name.ends_with(".so")) {
                    auto filename = name.substr(14);
                    require(!filename.empty() && filename.find('/') == filename.npos,
                            "Invalid native library path");
                    auto bytes = archive.read(name);
                    auto hash = sha256(bytes);
                    auto [it, inserted] = libraries.emplace(filename, hash);
                    require(inserted || it->second == hash,
                            "APK splits contain conflicting native libraries");
                    if (inserted)
                        write_bytes(staging / "lib" / path_from_utf8(filename), bytes);
                    metadata["libraries"][filename] = package.libraries.at(filename);
                }
        }
        require(!libraries.empty(), "APK set has no ARM64 libraries");
        if (!obb.empty()) {
            fs::copy_file(obb, staging / "obb" / obb.filename());
            auto obb_hash = sha256(staging / "obb" / obb.filename());
            require(sha256(obb) == obb_hash, "OBB changed during import");
            metadata["obb"] = {
                {"file", "obb/" + utf8(obb.filename())}, {"sha256", obb_hash}, {"size", size}};
        }
        auto user_data = absolute_root / "userdata" / path_from_utf8(base->manifest.package);
        if (data)
            import_downloaded_data(*data, user_data, base->manifest.package);
        fs::create_directories(user_data);
        write_json(staging / "installation.json", metadata);
        fs::rename(staging, directory);
        write_json(absolute_root / "active.json", {{"id", id}});
        return {absolute_root, directory, user_data, std::move(metadata)};
    } catch (...) {
        // Preserve partial data for diagnosis. A failed import never becomes active.
        try {
            write_json(staging / "import-failed.json", {{"status", "failed"}, {"id", id}});
        } catch (...) {
        }
        throw;
    }
}
Installation open_installation(const fs::path &root, std::optional<std::string> id) {
    auto absolute = fs::absolute(root).lexically_normal();
    if (!id)
        id = read_json(absolute / "active.json").at("id").get<std::string>();
    require(is_safe_relative(*id) && path_from_utf8(*id).filename() == path_from_utf8(*id),
            "Invalid installation identifier");
    auto dir = absolute / "installations" / path_from_utf8(*id);
    auto metadata = read_json(dir / "installation.json");
    if (!metadata.at("manifest").contains("metadata")) {
        for (const auto &apk : metadata.at("apks"))
            if (apk.at("split").get<std::string>().empty()) {
                Archive archive(dir / path_from_utf8(apk.at("file").get<std::string>()));
                metadata["manifest"]["metadata"] =
                    parse_manifest(archive.read("AndroidManifest.xml")).metadata;
                break;
            }
    }
    require(metadata.at("id") == *id && metadata.at("schema") == 1, "Installation metadata mismatch");
    auto package = metadata.at("manifest").at("package").get<std::string>();
    require(is_safe_relative(package) && path_from_utf8(package).filename() == path_from_utf8(package),
            "Invalid installed package");
    return {absolute, dir, absolute / "userdata" / path_from_utf8(package), std::move(metadata)};
}
Json installation_list(const fs::path &root) {
    Json out = Json::array();
    auto directory = root / "installations";
    if (!fs::is_directory(directory))
        return out;
    for (const auto &item : fs::directory_iterator(directory))
        if (item.is_directory() && fs::is_regular_file(item.path() / "installation.json"))
            out.push_back(read_json(item.path() / "installation.json"));
    return out;
}
void backup_before_launch(const Installation &installation) {
    auto state = installation.data_directory / ".runtime-version.json";
    if (fs::exists(state)) {
        auto previous = read_json(state).at("id").get<std::string>();
        if (previous != installation.id()) {
            auto backup = installation.root / "backups" / path_from_utf8(installation.package()) /
                          path_from_utf8(timestamp());
            copy_tree(installation.data_directory, backup);
        }
    }
    fs::create_directories(installation.data_directory);
    write_json(state, {{"id", installation.id()}, {"opened_at", timestamp()}});
}
} // namespace acp::android

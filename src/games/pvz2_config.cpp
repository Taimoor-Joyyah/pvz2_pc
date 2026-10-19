#include <acp/pvz2.hpp>

#include <sstream>
namespace acp::games {
Pvz2Options read_pvz2_options(const fs::path &file, bool create) {
    constexpr std::string_view defaults =
        "; PVZ2 PC: Google Play Games is disabled; gameplay uses local saves.\n"
        "[purchases]\n"
        "; disabled or auto_complete. auto_complete simulates local purchases; no money is charged.\n"
        "mode=disabled\n"
        "; Optional offline product metadata JSON, relative to this INI.\n"
        "catalog_file=\n";
    Pvz2Options options;
    if (!fs::exists(file)) {
        if (create)
            write_bytes(file, std::as_bytes(std::span(defaults)));
        return options;
    }
    auto bytes = read_bytes(file, 1 << 20);
    std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf"))
        text.erase(0, 3);
    auto trim = [](std::string value) {
        auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
        return first == value.npos ? std::string{} : value.substr(first, last - first + 1);
    };
    std::string section, line;
    fs::path catalog_file;
    std::set<std::string> seen;
    std::istringstream input(text);
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with(';') || line.starts_with('#'))
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            require(section == "purchases", "Unknown game.ini section: " + section);
            continue;
        }
        auto split = line.find('=');
        require(split != line.npos && section == "purchases",
                "Expected a [purchases] key=value in game.ini");
        auto key = trim(line.substr(0, split)), value = trim(line.substr(split + 1));
        require(seen.insert(key).second, "Duplicate game.ini option: " + key);
        if (key == "mode") {
            require(value == "disabled" || value == "auto_complete",
                    "purchases.mode must be disabled or auto_complete");
            options.purchases =
                value == "auto_complete" ? PurchaseMode::AutoComplete : PurchaseMode::Disabled;
        } else if (key == "catalog_file") {
            if (!value.empty())
                catalog_file = file.parent_path() / path_from_utf8(value);
        } else
            throw Error("Unknown game.ini purchases option: " + key);
    }
    if (options.purchases == PurchaseMode::AutoComplete && !catalog_file.empty())
        options.catalog = read_json(catalog_file);
    require(options.catalog.is_array(), "The offline purchase catalog must be a JSON array");
    return options;
}
} // namespace acp::games

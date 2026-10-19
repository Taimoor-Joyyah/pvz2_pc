#include <acp/guest.hpp>

#include <set>
namespace acp::guest {
void Linker::provide(std::string name, Handler handler) {
    auto address = traps_.add(name, std::move(handler));
    require(provided_.emplace(std::move(name), address).second, "Duplicate host import");
    ++revision_;
}
void Linker::provide_data(std::string name, Address a) {
    require(provided_.emplace(std::move(name), a).second, "Duplicate host data import");
    ++revision_;
}
void Linker::system_library(std::string name) {
    system_libraries_.push_back(std::move(name));
}
LoadedImage &Linker::load(const fs::path &path) {
    for (auto &image : images_)
        if (image->path.filename() == path.filename())
            return *image;
    auto item = std::make_unique<LoadedImage>(LoadedImage{path, ElfImage(read_bytes(path)), 0, false, {}});
    auto &image = *item;
    image.base = memory_.allocate(static_cast<size_t>(image.elf.image_size()), Read | Write, 4096);
    for (const auto &s : image.elf.segments())
        if (s.type == 1)
            memory_.write(image.base + s.address,
                          BinaryView(image.elf.bytes()).slice(s.offset, s.file_size));
    for (const auto &symbol : image.elf.symbols())
        if (symbol.defined() && symbol.binding) {
            auto address = symbol.section == 0xfff1 ? symbol.value : image.base + symbol.value;
            image.exports.try_emplace(symbol.name, address);
        }
    for (const auto &[name, address] : image.exports)
        if (address)
            symbols_.try_emplace(name, address);
    images_.push_back(std::move(item));
    ++revision_;
    for (const auto &dependency : image.elf.needed()) {
        if (std::ranges::find(system_libraries_, dependency) != system_libraries_.end())
            continue;
        require(is_safe_relative(dependency) &&
                    path_from_utf8(dependency).filename() == path_from_utf8(dependency),
                "Unsafe ELF dependency name");
        auto file = path.parent_path() / path_from_utf8(dependency);
        require(fs::is_regular_file(file), "Missing ELF dependency: " + dependency);
        load(file);
    }
    return image;
}
Address Linker::image_symbol(const LoadedImage &image, std::string_view name) const {
    auto it = image.exports.find(name);
    return it == image.exports.end() ? 0 : it->second;
}
Address Linker::symbol(std::string_view name) const {
    // Guest definitions (especially libc++/libc++abi) retain the Android ABI.
    if (auto it = symbols_.find(name); it != symbols_.end())
        return it->second;
    auto it = provided_.find(name);
    return it == provided_.end() ? 0 : it->second;
}
std::vector<std::string> Linker::unresolved() const {
    std::set<std::string> names;
    for (const auto &image : images_)
        for (const auto &s : image->elf.symbols())
            if (!s.defined() && s.binding != 2 && !s.name.empty() && !symbol(s.name))
                names.insert(s.name);
    return {names.begin(), names.end()};
}
void Linker::relocate() {
    auto missing = unresolved();
    require(missing.empty(), std::format("{} unresolved required imports; first: {}", missing.size(),
                                         missing.empty() ? "" : missing.front()));
    for (const auto &image : images_)
        for (const auto &r : image->elf.relocations()) {
            const auto &s = image->elf.symbols().at(r.symbol);
            Address address = 0;
            if (r.symbol) {
                if (s.defined() && (!s.binding || (s.visibility & 3) != 0))
                    address = image->base + s.value;
                else
                    address = symbol(s.name);
                require(address || s.binding == 2, "Unresolved relocation: " + s.name);
            }
            auto destination = image->base + r.offset;
            switch (r.type) {
            case 0:
                break;
            case 257:
            case 1025:
            case 1026:
                memory_.store(destination, address + static_cast<std::uint64_t>(r.addend));
                break;
            case 1027:
                memory_.store(destination, image->base + static_cast<std::uint64_t>(r.addend));
                break;
            default:
                throw Error(std::format("Unsupported AArch64 relocation {} in {}", r.type,
                                        utf8(image->path.filename())));
            }
        }
    for (const auto &image : images_) {
        for (const auto &s : image->elf.segments())
            if (s.type == 1) {
                unsigned permission =
                    (s.flags & 4 ? Read : 0) | (s.flags & 2 ? Write : 0) | (s.flags & 1 ? Execute : 0);
                memory_.protect(image->base + s.address, static_cast<size_t>(s.memory_size), permission);
            }
    }
}
void Linker::initialize(Cpu &cpu, LoadedImage *selected) {
    std::function<void(LoadedImage &)> init = [&](LoadedImage &image) {
        if (image.initialized)
            return;
        image.initialized = true;
        for (const auto &name : image.elf.needed())
            for (auto &dependency : images_)
                if (utf8(dependency->path.filename()) == name)
                    init(*dependency);
        if (auto f = image.elf.dynamic(12); f && *f) {
            current_initializer = utf8(image.path.filename()) + ":DT_INIT";
            cpu.call(image.base + *f);
        }
        auto array = image.elf.dynamic(25), bytes = image.elf.dynamic(27);
        if (array && bytes) {
            require(*bytes % 8 == 0 && *bytes < 1 << 20, "Invalid ELF constructor array");
            for (size_t i = 0; i < *bytes; i += 8) {
                auto f = memory_.load<Address>(image.base + *array + i);
                if (f && f != UINT64_MAX) {
                    current_initializer = std::format("{}:init_array[{}] at {}",
                                                      utf8(image.path.filename()), i / 8, locate(f));
                    cpu.call(f);
                }
            }
        }
    };
    if (selected)
        init(*selected);
    else
        for (auto &image : images_)
            init(*image);
    current_initializer.clear();
}
std::string Linker::locate(Address a) const {
    for (const auto &image : images_)
        if (a >= image->base && a - image->base < image->elf.image_size())
            return std::format("{}+{:#x}", utf8(image->path.filename()), a - image->base);
    return std::format("{:#x}", a);
}
} // namespace acp::guest

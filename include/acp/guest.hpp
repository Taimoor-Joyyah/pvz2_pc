#pragma once
#include <acp/common.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace acp::guest {
enum Permission : unsigned { Read = 1, Write = 2, Execute = 4 };
template <class T = void> struct Pointer {
    Address address{};
    explicit operator bool() const {
        return address != 0;
    }
};
class Memory {
  public:
    static constexpr size_t page_size = 4096;
    static constexpr std::uint64_t capacity = 1ull << 32;
    Memory();
    ~Memory();
    Memory(const Memory &) = delete;
    Address allocate(size_t size, unsigned permissions = Read | Write, size_t alignment = 16);
    void release(Address address);
    void protect(Address address, size_t size, unsigned permissions);
    bool valid(Address address, size_t size, unsigned permissions = Read) const;
    void read(Address address, std::span<std::byte> out) const;
    void write(Address address, std::span<const std::byte> in);
    std::span<std::byte> writable(Address address, size_t size);
    std::span<const std::byte> readable(Address address, size_t size) const;
    template <class T> T load(Address address) const {
        T value{};
        read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    template <class T> void store(Address address, const T &value) {
        write(address, std::as_bytes(std::span(&value, 1)));
    }
    std::string string(Address address, size_t maximum = 1 << 20) const;
    Address string(std::string_view text);
    void **page_table() {
        return pages_.data();
    }
    size_t allocation_size(Address address) const;
    Json statistics() const;
    bool compare_exchange(Address address, std::span<const std::byte> expected,
                          std::span<const std::byte> desired);

  private:
    std::byte *arena_{};
    Address next_ = 0x10000;
    std::vector<void *> pages_;
    std::vector<unsigned char> permissions_;
    std::map<Address, size_t> allocations_, free_;
    std::map<size_t, std::set<Address>> free_sizes_;
    std::uint64_t allocation_calls_{}, live_bytes_{};
    void add_free(Address address, size_t size);
    void erase_free(std::map<Address, size_t>::iterator item);
    mutable std::recursive_mutex mutex_;
};

struct Segment {
    std::uint32_t type{}, flags{};
    std::uint64_t offset{}, address{}, file_size{}, memory_size{}, alignment{};
};
struct Symbol {
    std::string name;
    std::uint64_t value{}, size{};
    std::uint16_t section{};
    std::uint8_t binding{}, type{}, visibility{};
    bool defined() const {
        return section != 0;
    }
};
struct Relocation {
    std::uint64_t offset{};
    std::uint32_t symbol{}, type{};
    std::int64_t addend{};
};
class ElfImage {
  public:
    explicit ElfImage(Bytes bytes);
    const Bytes &bytes() const {
        return bytes_;
    }
    const auto &segments() const {
        return segments_;
    }
    const auto &symbols() const {
        return symbols_;
    }
    const auto &relocations() const {
        return relocations_;
    }
    const auto &needed() const {
        return needed_;
    }
    std::optional<std::uint64_t> dynamic(std::int64_t tag) const;
    std::uint64_t image_size() const;
    Json describe(bool include_symbols = false) const;

  private:
    Bytes bytes_;
    std::vector<Segment> segments_;
    std::vector<Symbol> symbols_;
    std::vector<Relocation> relocations_;
    std::vector<std::string> needed_;
    std::unordered_map<std::int64_t, std::uint64_t> dynamic_;
};
class Cpu;
using Handler = std::function<void(Cpu &)>;
struct Trap {
    std::string name;
    Handler handler;
    Address address{};
};
class TrapTable {
  public:
    explicit TrapTable(Memory &memory) : memory_(memory) {}
    Address add(std::string name, Handler handler);
    const Trap &at(std::uint32_t id) const;
    std::vector<std::string> names() const;

  private:
    Memory &memory_;
    std::vector<Trap> traps_;
};
class Cpu {
  public:
    Cpu(Memory &memory, TrapTable &traps, size_t processor_id = 0);
    ~Cpu();
    Cpu(const Cpu &) = delete;
    std::function<void()> checkpoint;
    std::uint64_t call(Address function, std::span<const std::uint64_t> arguments = {},
                       std::uint64_t budget = 200'000'000,
                       std::chrono::milliseconds timeout = std::chrono::seconds(30));
    std::uint64_t x(size_t index) const;
    void x(size_t index, std::uint64_t value);
    double f64(size_t index) const;
    void f64(size_t index, double value);
    float f32(size_t index) const;
    void f32(size_t index, float value);
    std::uint64_t argument(size_t index) const;
    Address sp() const;
    void sp(Address value);
    Address pc() const;
    void pc(Address address);
    Address tls() const;
    void tls(Address value);
    void invalidate(Address address, size_t size);
    void request_stop();
    Json statistics() const;
    Memory &memory() {
        return memory_;
    }
    std::string current_import;
    Json last_fault;
    std::function<void(Cpu &)> syscall;

  private:
    struct Impl;
    Memory &memory_;
    std::unique_ptr<Impl> impl_;
};
struct LoadedImage {
    fs::path path;
    ElfImage elf;
    Address base{};
    bool initialized{};
};
class Linker {
  public:
    Linker(Memory &memory, TrapTable &traps) : memory_(memory), traps_(traps) {}
    void provide(std::string name, Handler handler);
    void provide_data(std::string name, Address address);
    void system_library(std::string name);
    LoadedImage &load(const fs::path &path);
    void relocate();
    void initialize(Cpu &cpu, LoadedImage *image = nullptr);
    Address symbol(std::string_view name) const;
    Address image_symbol(const LoadedImage &image, std::string_view name) const;
    std::vector<std::string> unresolved() const;
    std::string locate(Address address) const;
    const auto &images() const {
        return images_;
    }
    const auto &provided() const {
        return provided_;
    }
    std::string current_initializer;

  private:
    Memory &memory_;
    TrapTable &traps_;
    std::vector<std::unique_ptr<LoadedImage>> images_;
    std::unordered_map<std::string, Address> provided_;
    std::vector<std::string> system_libraries_;
};
} // namespace acp::guest

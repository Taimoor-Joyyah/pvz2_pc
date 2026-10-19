#include <acp/guest.hpp>

#include <Windows.h>
namespace acp::guest {
Memory::Memory() : pages_(capacity / page_size), permissions_(capacity / page_size) {
    arena_ = static_cast<std::byte *>(VirtualAlloc(nullptr, capacity, MEM_RESERVE, PAGE_NOACCESS));
    require(arena_ != nullptr, "Cannot reserve 4 GiB guest address space");
}
Memory::~Memory() {
    if (arena_)
        VirtualFree(arena_, 0, MEM_RELEASE);
}
Address Memory::allocate(size_t size, unsigned permissions, size_t alignment) {
    std::lock_guard lock(mutex_);
    size = static_cast<size_t>(align_up(std::max<size_t>(size, 1), 16));
    require(size < capacity, "Guest allocation exceeds address space");
    Address address{};
    if (alignment <= 16 && permissions == (Read | Write)) {
        auto fit = free_sizes_.lower_bound(size);
        if (fit != free_sizes_.end()) {
            address = *fit->second.begin();
            auto remaining = fit->first - size;
            erase_free(free_.find(address));
            if (remaining)
                add_free(address + size, remaining);
        }
    }
    if (!address) {
        address = align_up(next_, alignment);
        require(address < capacity && size <= capacity - address, "Guest address space exhausted");
        next_ = address + size;
    }
    auto page_begin = address & ~(page_size - 1ull);
    auto page_end = align_up(address + size, page_size);
    for (auto p = page_begin; p < page_end;) {
        if (permissions_[p / page_size]) {
            p += page_size;
            continue;
        }
        auto begin = p;
        do {
            p += page_size;
        } while (p < page_end && !permissions_[p / page_size]);
        require(VirtualAlloc(arena_ + begin, p - begin, MEM_COMMIT, PAGE_READWRITE) != nullptr,
                "Cannot commit guest memory");
    }
    for (auto p = page_begin; p < page_end; p += page_size) {
        auto index = p / page_size;
        permissions_[index] |= static_cast<unsigned char>(permissions);
        pages_[index] = ((permissions_[index] & (Read | Write)) == (Read | Write)) ? arena_ + p : nullptr;
    }
    allocations_.emplace(address, size);
    ++allocation_calls_;
    live_bytes_ += size;
    std::memset(arena_ + address, 0, size);
    return address;
}
void Memory::release(Address address) {
    if (!address)
        return;
    std::lock_guard lock(mutex_);
    auto it = allocations_.find(address);
    require(it != allocations_.end(), "Invalid or duplicate guest free");
    auto size = it->second;
    live_bytes_ -= size;
    allocations_.erase(it);
    release_range(address, size);
}
void Memory::shrink_pages(Address address, size_t size) {
    std::lock_guard lock(mutex_);
    auto it = allocations_.find(address);
    require(it != allocations_.end() && size && size <= it->second && !(address % page_size) &&
                !(size % page_size) && !(it->second % page_size), "Invalid page allocation shrink");
    auto tail = it->second - size;
    if (!tail) return;
    protect(address + size, tail, 0);
    it->second = size;
    live_bytes_ -= tail;
    release_range(address + size, tail);
}
void Memory::release_range(Address address, size_t size) {
    auto after = free_.lower_bound(address);
    if (after != free_.end() && address + size == after->first) {
        size += after->second;
        auto item = after++;
        erase_free(item);
    }
    if (after != free_.begin()) {
        auto before = std::prev(after);
        if (before->first + before->second == address) {
            address = before->first;
            size += before->second;
            erase_free(before);
        }
    }
    add_free(address, size);
}
void Memory::add_free(Address address, size_t size) {
    free_.emplace(address, size);
    free_sizes_[size].insert(address);
}
void Memory::erase_free(std::map<Address, size_t>::iterator item) {
    auto sizes = free_sizes_.find(item->second);
    sizes->second.erase(item->first);
    if (sizes->second.empty())
        free_sizes_.erase(sizes);
    free_.erase(item);
}
Json Memory::statistics() const {
    std::lock_guard lock(mutex_);
    return {{"allocation_calls", allocation_calls_},
            {"live_allocations", allocations_.size()},
            {"live_bytes", live_bytes_},
            {"address_high_water", next_},
            {"free_ranges", free_.size()}};
}
void Memory::protect(Address address, size_t size, unsigned permissions) {
    std::lock_guard lock(mutex_);
    require(address < capacity && size <= capacity - address, "Guest protection range overflow");
    for (auto p = address & ~(page_size - 1ull); p < align_up(address + size, page_size); p += page_size) {
        auto index = p / page_size;
        require(permissions_[index] != 0, "Protecting unmapped guest memory");
        // Keep an allocation marker for PROT_NONE pages so mprotect can restore access.
        // Zero denotes a page that has never been committed; access checks use the low rwx bits.
        permissions_[index] = static_cast<unsigned char>(permissions | 8);
        pages_[index] = ((permissions & (Read | Write)) == (Read | Write)) ? arena_ + p : nullptr;
    }
}
bool Memory::valid(Address address, size_t size, unsigned permissions) const {
    if (!size)
        return address <= capacity;
    if (address < 0x10000 || address >= capacity || size > capacity - address)
        return false;
    for (auto page = address / page_size; page <= (address + size - 1) / page_size; ++page)
        if ((permissions_[page] & permissions) != permissions)
            return false;
    return true;
}
std::span<const std::byte> Memory::readable(Address address, size_t size) const {
    if (!valid(address, size))
        throw Error(std::format("Guest read fault at {:#x}, {} bytes", address, size));
    return {arena_ + address, size};
}
std::span<std::byte> Memory::writable(Address address, size_t size) {
    if (!valid(address, size, Write))
        throw Error(std::format("Guest write fault at {:#x}, {} bytes", address, size));
    return {arena_ + address, size};
}
void Memory::read(Address address, std::span<std::byte> out) const {
    auto source = readable(address, out.size());
    if (!out.empty())
        std::memcpy(out.data(), source.data(), out.size());
}
void Memory::write(Address address, std::span<const std::byte> in) {
    auto dest = writable(address, in.size());
    if (!in.empty())
        std::memcpy(dest.data(), in.data(), in.size());
}
std::string Memory::string(Address address, size_t maximum) const {
    std::string result;
    while (result.size() < maximum) {
        auto current = address + result.size();
        auto chunk = std::min(maximum - result.size(), page_size - size_t(current % page_size));
        auto bytes = readable(current, chunk);
        auto data = reinterpret_cast<const char *>(bytes.data());
        auto end = static_cast<const char *>(std::memchr(data, 0, chunk));
        result.append(data, end ? static_cast<size_t>(end - data) : chunk);
        if (end)
            return result;
    }
    throw Error("Unterminated guest string");
}
Address Memory::string(std::string_view text) {
    auto address = allocate(text.size() + 1);
    write(address, std::as_bytes(std::span(text)));
    return address;
}
size_t Memory::allocation_size(Address address) const {
    std::lock_guard lock(mutex_);
    auto it = allocations_.find(address);
    require(it != allocations_.end(), "Unknown guest allocation");
    return it->second;
}
bool Memory::compare_exchange(Address address, std::span<const std::byte> expected,
                              std::span<const std::byte> desired) {
    std::lock_guard lock(mutex_);
    require(expected.size() == desired.size(), "Exclusive memory size mismatch");
    auto dest = writable(address, desired.size());
    if (!std::ranges::equal(dest, expected))
        return false;
    std::memcpy(dest.data(), desired.data(), desired.size());
    return true;
}
} // namespace acp::guest

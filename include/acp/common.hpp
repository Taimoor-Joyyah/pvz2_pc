#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace acp {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<std::byte>;
using Address = std::uint64_t;
inline fs::path path_from_utf8(std::string_view value) {
    std::u8string text(value.size(), u8'\0');
    std::memcpy(text.data(), value.data(), value.size());
    return fs::path(text);
}
class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
inline void require(bool condition, std::string_view message) {
    if (!condition)
        throw Error(std::string(message));
}
inline std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    require(alignment && std::has_single_bit(alignment), "Alignment must be a power of two");
    require(value <= UINT64_MAX - alignment + 1, "Alignment overflow");
    return (value + alignment - 1) & ~(alignment - 1);
}
class BinaryView {
  public:
    explicit BinaryView(std::span<const std::byte> data) : data_(data) {}
    [[nodiscard]] auto size() const {
        return data_.size();
    }
    [[nodiscard]] auto slice(std::uint64_t offset, std::uint64_t count) const {
        require(offset <= data_.size() && count <= data_.size() - offset, "Truncated binary input");
        return data_.subspan(static_cast<size_t>(offset), static_cast<size_t>(count));
    }
    template <class T> [[nodiscard]] T get(std::uint64_t offset) const {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(std::endian::native == std::endian::little);
        T value;
        auto bytes = slice(offset, sizeof(T));
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }
    [[nodiscard]] std::string cstring(std::uint64_t offset, size_t maximum = 1 << 20) const {
        auto bytes = slice(
            offset,
            std::min<std::uint64_t>(maximum, data_.size() - std::min<std::uint64_t>(offset, data_.size())));
        auto end = std::find(bytes.begin(), bytes.end(), std::byte{});
        require(end != bytes.end(), "Unterminated binary string");
        return {reinterpret_cast<const char *>(bytes.data()), static_cast<size_t>(end - bytes.begin())};
    }

  private:
    std::span<const std::byte> data_;
};
Bytes read_bytes(const fs::path &path, std::uint64_t limit = 512ull << 20);
void write_bytes(const fs::path &path, std::span<const std::byte> data);
std::string sha256(const fs::path &path);
std::string sha256(std::span<const std::byte> data);
std::string utf8(const fs::path &path);
std::string narrow(std::wstring_view value);
std::wstring widen(std::string_view value);
void write_json(const fs::path &path, const Json &value);
Json read_json(const fs::path &path);
std::string timestamp();
bool is_safe_relative(std::string_view path);
void write_png_rgba(const fs::path &path, int width, int height, std::span<const std::byte> rgba,
                    bool bottom_up = false);
class Archive {
  public:
    explicit Archive(const fs::path &path);
    ~Archive();
    Archive(const Archive &) = delete;
    std::vector<std::string> names() const;
    bool contains(std::string_view name) const;
    Bytes read(std::string_view name, std::uint64_t limit = 256ull << 20) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace acp

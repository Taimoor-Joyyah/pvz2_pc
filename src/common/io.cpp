#include <acp/common.hpp>

#include <Windows.h>

#include <bcrypt.h>
#include <chrono>

namespace acp {
Bytes read_bytes(const fs::path &path, std::uint64_t limit) {
    auto length = fs::file_size(path);
    require(length <= limit && length <= SIZE_MAX, "Input exceeds configured size limit: " + utf8(path));
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot open " + utf8(path));
    Bytes bytes(static_cast<size_t>(length));
    if (length)
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
    require(bool(stream), "Cannot read " + utf8(path));
    return bytes;
}
void write_bytes(const fs::path &path, std::span<const std::byte> data) {
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(bool(stream), "Cannot create " + utf8(path));
    stream.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    stream.flush();
    require(bool(stream), "Cannot write " + utf8(path));
}
namespace {
class Hash {
  public:
    Hash() {
        require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                "SHA256 provider failed");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw Error("SHA256 initialization failed");
        }
    }
    ~Hash() {
        if (hash_)
            BCryptDestroyHash(hash_);
        if (algorithm_)
            BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void update(std::span<const std::byte> data) {
        require(data.size() <= ULONG_MAX, "Hash chunk too large");
        require(BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte *>(data.data())),
                               static_cast<ULONG>(data.size()), 0) >= 0,
                "SHA256 update failed");
    }
    std::string finish() {
        std::array<UCHAR, 32> result{};
        require(BCryptFinishHash(hash_, result.data(), static_cast<ULONG>(result.size()), 0) >= 0,
                "SHA256 finalization failed");
        std::string text;
        for (auto b : result)
            text += std::format("{:02x}", b);
        return text;
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};
} // namespace
std::string sha256(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot hash " + utf8(path));
    Hash hash;
    Bytes buffer(1 << 20);
    while (stream) {
        stream.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        hash.update(std::span(buffer).first(static_cast<size_t>(stream.gcount())));
    }
    require(stream.eof(), "Hash read failed");
    return hash.finish();
}
std::string sha256(std::span<const std::byte> data) {
    Hash hash;
    hash.update(data);
    return hash.finish();
}
std::string utf8(const fs::path &path) {
    auto s = path.u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
std::string narrow(std::wstring_view value) {
    if (value.empty())
        return {};
    auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                 static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    require(n > 0, "Invalid UTF-16 string");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring widen(std::string_view value) {
    if (value.empty())
        return {};
    auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                 static_cast<int>(value.size()), nullptr, 0);
    require(n > 0, "Invalid UTF-8 string");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        out.data(), n);
    return out;
}
void write_json(const fs::path &path, const Json &value) {
    auto text = value.dump(2) + "\n";
    auto temp = path;
    temp += L".tmp";
    write_bytes(temp, std::as_bytes(std::span(text)));
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw Error("Cannot publish JSON: " + utf8(path));
    }
}
Json read_json(const fs::path &path) {
    auto bytes = read_bytes(path, 32 << 20);
    return Json::parse(bytes.begin(), bytes.end());
}
std::string timestamp() {
    return std::format("{:%Y%m%dT%H%M%SZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}
bool is_safe_relative(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find(':') != path.npos ||
        path.find('\0') != path.npos)
        return false;
    std::string clean(path);
    std::replace(clean.begin(), clean.end(), '\\', '/');
    auto p = path_from_utf8(clean);
    if (p.is_absolute() || p.has_root_name())
        return false;
    for (const auto &component : p)
        if (component == "..")
            return false;
    return true;
}
} // namespace acp

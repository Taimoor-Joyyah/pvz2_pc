#include <acp/common.hpp>

#include <miniz.h>
#include <unordered_map>
namespace acp {
struct Archive::Impl {
    Bytes bytes;
    mz_zip_archive zip{};
    std::unordered_map<std::string, mz_uint> index;
    explicit Impl(const fs::path &path) : bytes(read_bytes(path, 2ull << 30)) {
        require(mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0) != 0,
                "Invalid ZIP/APK: " + utf8(path));
        try {
            auto count = mz_zip_reader_get_num_files(&zip);
            require(count < 200000, "Excessive ZIP entry count");
            for (mz_uint i = 0; i < count; ++i) {
                mz_zip_archive_file_stat stat{};
                require(mz_zip_reader_file_stat(&zip, i, &stat) != 0, "Invalid ZIP directory");
                require(is_safe_relative(stat.m_filename), "Unsafe ZIP entry path");
                require(index.emplace(stat.m_filename, i).second, "Duplicate ZIP entry");
            }
        } catch (...) {
            mz_zip_reader_end(&zip);
            throw;
        }
    }
    ~Impl() {
        mz_zip_reader_end(&zip);
    }
};
Archive::Archive(const fs::path &path) : impl_(std::make_unique<Impl>(path)) {}
Archive::~Archive() = default;
std::vector<std::string> Archive::names() const {
    std::vector<std::string> out;
    for (const auto &[name, _] : impl_->index)
        out.push_back(name);
    std::ranges::sort(out);
    return out;
}
bool Archive::contains(std::string_view name) const {
    return impl_->index.contains(std::string(name));
}
Bytes Archive::read(std::string_view name, std::uint64_t limit) const {
    auto it = impl_->index.find(std::string(name));
    require(it != impl_->index.end(), "Missing ZIP entry: " + std::string(name));
    mz_zip_archive_file_stat stat{};
    require(mz_zip_reader_file_stat(&impl_->zip, it->second, &stat) != 0, "ZIP entry metadata failed");
    require(stat.m_uncomp_size <= limit && !stat.m_is_directory,
            "ZIP entry exceeds size limit or is a directory");
    Bytes out(static_cast<size_t>(stat.m_uncomp_size));
    require(mz_zip_reader_extract_to_mem(&impl_->zip, it->second, out.data(), out.size(), 0) != 0,
            "ZIP decompression or CRC check failed");
    return out;
}
} // namespace acp

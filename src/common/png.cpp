#include <acp/common.hpp>

#include <miniz.h>
namespace acp {
namespace {
void integer(Bytes &output, std::uint32_t value) {
    auto big = std::byteswap(value);
    auto bytes = std::as_bytes(std::span(&big, 1));
    output.insert(output.end(), bytes.begin(), bytes.end());
}
void chunk(Bytes &output, std::string_view type, std::span<const std::byte> data) {
    require(type.size() == 4 && data.size() <= UINT32_MAX, "Invalid PNG chunk");
    integer(output, static_cast<std::uint32_t>(data.size()));
    auto begin = output.size();
    auto t = std::as_bytes(std::span(type));
    output.insert(output.end(), t.begin(), t.end());
    output.insert(output.end(), data.begin(), data.end());
    auto crc =
        mz_crc32(0, reinterpret_cast<const unsigned char *>(output.data() + begin), output.size() - begin);
    integer(output, static_cast<std::uint32_t>(crc));
}
} // namespace
void write_png_rgba(const fs::path &path, int width, int height, std::span<const std::byte> rgba,
                    bool bottom_up) {
    require(width > 0 && height > 0 && std::uint64_t(width) * height <= 16'777'216,
            "PNG dimensions exceed capture limit");
    size_t row = static_cast<size_t>(width) * 4;
    require(rgba.size() == row * height, "PNG pixel buffer size mismatch");
    Bytes scanlines((row + 1) * height);
    for (int y = 0; y < height; ++y) {
        auto source = bottom_up ? height - y - 1 : y;
        std::memcpy(scanlines.data() + size_t(y) * (row + 1) + 1, rgba.data() + size_t(source) * row, row);
    }
    mz_ulong length = mz_compressBound(static_cast<mz_ulong>(scanlines.size()));
    Bytes compressed(length);
    require(mz_compress2(reinterpret_cast<unsigned char *>(compressed.data()), &length,
                         reinterpret_cast<const unsigned char *>(scanlines.data()),
                         static_cast<mz_ulong>(scanlines.size()), MZ_BEST_SPEED) == MZ_OK,
            "PNG compression failed");
    compressed.resize(length);
    Bytes png;
    const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    auto magic = std::as_bytes(std::span(signature));
    png.insert(png.end(), magic.begin(), magic.end());
    Bytes ihdr;
    integer(ihdr, static_cast<unsigned>(width));
    integer(ihdr, static_cast<unsigned>(height));
    for (unsigned char value : {8, 6, 0, 0, 0})
        ihdr.push_back(std::byte{value});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", compressed);
    chunk(png, "IEND", {});
    write_bytes(path, png);
}
} // namespace acp

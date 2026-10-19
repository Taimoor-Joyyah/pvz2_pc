#include <acp/android.hpp>

#include <Windows.h>

#include <algorithm>
#include <bcrypt.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <io.h>
#include <iostream>
#include <miniz.h>
#include <random>
#include <semaphore>
#include <sstream>

namespace acp::android {
namespace {
int compare_strings(const guest::Memory &memory, Address left, Address right, size_t maximum,
                    bool insensitive, bool bounded) {
    size_t offset = 0;
    while (offset < maximum) {
        auto a = left + offset, b = right + offset;
        auto count =
            std::min({maximum - offset, guest::Memory::page_size - size_t(a % guest::Memory::page_size),
                      guest::Memory::page_size - size_t(b % guest::Memory::page_size)});
        auto x = reinterpret_cast<const char *>(memory.readable(a, count).data());
        auto y = reinterpret_cast<const char *>(memory.readable(b, count).data());
        if (!insensitive) {
            auto end = static_cast<const char *>(std::memchr(x, 0, count));
            auto result = std::strncmp(x, y, end ? static_cast<size_t>(end - x) + 1 : count);
            if (result || end)
                return result;
        } else {
            for (size_t i = 0; i < count; ++i) {
                auto first = std::tolower(static_cast<unsigned char>(x[i]));
                auto second = std::tolower(static_cast<unsigned char>(y[i]));
                if (first != second || !first)
                    return first - second;
            }
        }
        offset += count;
    }
    require(bounded, "Unterminated guest string");
    return 0;
}

int compare_wide(const guest::Memory &memory, Address left, Address right, size_t maximum,
                 bool terminated) {
    size_t offset = 0;
    while (offset < maximum) {
        auto a = left + offset * 4, b = right + offset * 4;
        auto count = std::min({maximum - offset,
                               (guest::Memory::page_size - size_t(a % guest::Memory::page_size)) / 4,
                               (guest::Memory::page_size - size_t(b % guest::Memory::page_size)) / 4});
        // A deliberately unaligned 32-bit unit can span two pages.
        if (!count) {
            auto x = memory.load<std::uint32_t>(a), y = memory.load<std::uint32_t>(b);
            if (x != y)
                return x < y ? -1 : 1;
            if (terminated && !x)
                return 0;
            ++offset;
            continue;
        }
        auto x = memory.readable(a, count * 4), y = memory.readable(b, count * 4);
        for (size_t i = 0; i < count; ++i) {
            std::uint32_t first{}, second{};
            std::memcpy(&first, x.data() + i * 4, 4);
            std::memcpy(&second, y.data() + i * 4, 4);
            if (first != second)
                return first < second ? -1 : 1;
            if (terminated && !first)
                return 0;
        }
        offset += count;
    }
    return 0;
}

struct Args {
    guest::Cpu &cpu;
    size_t integer{}, floating{};
    Address va{};
    size_t stack_offset{};
    std::uint64_t word() {
        if (!va) {
            if (integer < 8) return cpu.x(integer++);
            auto value = cpu.memory().load<std::uint64_t>(cpu.sp() + stack_offset);
            stack_offset += 8;
            return value;
        }
        auto &m = cpu.memory();
        auto offset = m.load<std::int32_t>(va + 24);
        if (offset < 0) {
            m.store(va + 24, offset + 8);
            return m.load<std::uint64_t>(m.load<Address>(va + 8) + static_cast<std::int64_t>(offset));
        }
        auto stack = m.load<Address>(va);
        m.store(va, stack + 8);
        return m.load<std::uint64_t>(stack);
    }
    double real() {
        if (!va) {
            if (floating < 8)
                return cpu.f64(floating++);
            auto value = cpu.memory().load<double>(cpu.sp() + stack_offset);
            stack_offset += 8;
            return value;
        }
        auto &m = cpu.memory();
        auto offset = m.load<std::int32_t>(va + 28);
        if (offset < 0) {
            m.store(va + 28, offset + 16);
            return m.load<double>(m.load<Address>(va + 16) + static_cast<std::int64_t>(offset));
        }
        auto stack = m.load<Address>(va);
        m.store(va, stack + 8);
        return m.load<double>(stack);
    }
    std::array<std::uint64_t, 2> quad() {
        if (!va) {
            if (floating < 8) return cpu.v128(floating++);
            auto stack = align_up(cpu.sp() + stack_offset, 16);
            stack_offset = static_cast<size_t>(stack + 16 - cpu.sp());
            return cpu.memory().load<std::array<std::uint64_t, 2>>(stack);
        }
        auto &m = cpu.memory();
        auto offset = m.load<std::int32_t>(va + 28);
        if (offset < 0) {
            m.store(va + 28, offset + 16);
            return m.load<std::array<std::uint64_t, 2>>(m.load<Address>(va + 16) + static_cast<std::int64_t>(offset));
        }
        auto stack = align_up(m.load<Address>(va), 16);
        m.store(va, stack + 16);
        return m.load<std::array<std::uint64_t, 2>>(stack);
    }
};
template <class T> std::string print_value(std::string_view fmt, T value) {
    auto n = std::snprintf(nullptr, 0, std::string(fmt).c_str(), value);
    require(n >= 0 && n < 1 << 20, "Invalid or excessive printf conversion");
    std::string text(static_cast<size_t>(n) + 1, 0);
    std::snprintf(text.data(), text.size(), std::string(fmt).c_str(), value);
    text.resize(n);
    return text;
}
std::string encode_codepoint(std::uint32_t value);
std::string wide_string(guest::Memory &memory, Address p) {
    std::string out;
    for (size_t n = 0; n < 1 << 20; ++n) {
        auto c = memory.load<std::uint32_t>(p + n * 4);
        if (!c)
            return out;
        out += encode_codepoint(c);
    }
    throw Error("Unterminated UTF32 string");
}
struct ThreadExit {
    Address result;
};
std::uint64_t millis() {
    return GetTickCount64();
}
std::pair<std::uint32_t, size_t> codepoint(guest::Memory &memory, Address address, size_t available) {
    if (!available)
        return {0, SIZE_MAX - 1};
    auto first = memory.load<std::uint8_t>(address);
    if (first < 128)
        return {first, first ? 1 : 0};
    size_t length = (first & 0xe0) == 0xc0   ? 2
                    : (first & 0xf0) == 0xe0 ? 3
                    : (first & 0xf8) == 0xf0 ? 4
                                             : 0;
    if (!length)
        return {0, SIZE_MAX};
    if (available < length)
        return {0, SIZE_MAX - 1};
    std::uint32_t value = first & ((1u << (7 - length)) - 1);
    for (size_t i = 1; i < length; ++i) {
        auto b = memory.load<std::uint8_t>(address + i);
        if ((b & 0xc0) != 0x80)
            return {0, SIZE_MAX};
        value = (value << 6) | (b & 63);
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) || (length == 2 && value < 128) ||
        (length == 3 && value < 0x800) || (length == 4 && value < 0x10000))
        return {0, SIZE_MAX};
    return {value, length};
}
std::string encode_codepoint(std::uint32_t value) {
    require(value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff), "Invalid UTF32 code point");
    std::string out;
    if (value < 128)
        out += static_cast<char>(value);
    else if (value < 0x800) {
        out += static_cast<char>(0xc0 | (value >> 6));
        out += static_cast<char>(0x80 | (value & 63));
    } else if (value < 0x10000) {
        out += static_cast<char>(0xe0 | (value >> 12));
        out += static_cast<char>(0x80 | ((value >> 6) & 63));
        out += static_cast<char>(0x80 | (value & 63));
    } else {
        out += static_cast<char>(0xf0 | (value >> 18));
        out += static_cast<char>(0x80 | ((value >> 12) & 63));
        out += static_cast<char>(0x80 | ((value >> 6) & 63));
        out += static_cast<char>(0x80 | (value & 63));
    }
    return out;
}
std::u32string utf32(std::string_view text) {
    auto converted = widen(text);
    std::u32string result;
    for (size_t i = 0; i < converted.size(); ++i) {
        std::uint32_t value = converted[i];
        if (value >= 0xd800 && value <= 0xdbff) {
            require(i + 1 < converted.size() && converted[i + 1] >= 0xdc00 && converted[i + 1] <= 0xdfff,
                    "Invalid UTF16 conversion");
            value = 0x10000 + ((value - 0xd800) << 10) + (converted[++i] - 0xdc00);
        }
        result.push_back(value);
    }
    return result;
}
std::string format_text(guest::Cpu &cpu, std::string_view format, size_t first, Address va, bool wide) {
    Args args{cpu, first, 0, va};
    std::string output;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            output += format[i];
            continue;
        }
        require(++i < format.size(), "Incomplete printf format");
        if (format[i] == '%') {
            output += '%';
            continue;
        }
        std::string spec = "%";
        while (i < format.size() && std::string_view("-+ #0").find(format[i]) != std::string_view::npos)
            spec += format[i++];
        auto dimension = [&]() -> int {
            if (i < format.size() && format[i] == '*') {
                ++i;
                auto width = static_cast<std::int32_t>(args.word());
                require(std::abs(std::int64_t(width)) <= 65536, "Excessive printf width");
                spec += std::to_string(width);
                return width;
            } else {
                size_t start = i;
                while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
                    ++i;
                auto number = format.substr(start, i - start);
                require(number.size() < 6, "Excessive printf width");
                int parsed{};
                if (!number.empty()) {
                    auto [end, error] =
                        std::from_chars(number.data(), number.data() + number.size(), parsed);
                    require(error == std::errc{} && end == number.data() + number.size() && parsed <= 65536,
                            "Excessive printf width");
                }
                spec += number;
                return parsed;
            }
        };
        auto width = dimension();
        std::optional<int> precision;
        if (i < format.size() && format[i] == '.') {
            auto before_precision = spec.size();
            spec += '.';
            ++i;
            precision = dimension();
            if (*precision < 0) {
                precision.reset();
                spec.resize(before_precision);
            }
        }
        std::string length;
        while (i < format.size() && std::string_view("hljztL").find(format[i]) != std::string_view::npos)
            length += format[i++];
        require(i < format.size(), "Incomplete printf conversion");
        auto c = format[i];
        switch (c) {
        case 'd':
        case 'i': {
            auto value = args.word();
            auto signed_value = length.empty() || length == "h" || length == "hh"
                                    ? std::int64_t(static_cast<std::int32_t>(value))
                                    : std::bit_cast<std::int64_t>(value);
            output += print_value(spec + "ll" + c, static_cast<long long>(signed_value));
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            auto value = args.word();
            if (length.empty() || length == "h" || length == "hh")
                value = static_cast<std::uint32_t>(value);
            output += print_value(spec + "ll" + c, static_cast<unsigned long long>(value));
            break;
        }
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'e':
        case 'E':
        case 'a':
        case 'A':
            output += length == "L" ? format_binary128(args.quad(), spec, c) : print_value(spec + c, args.real());
            break;
        case 's':
        case 'S': {
            auto p = args.word();
            auto text =
                p ? (length == "l" || c == 'S' ? wide_string(cpu.memory(), p) : cpu.memory().string(p))
                  : "(null)";
            if (wide) {
                auto points = utf32(text);
                if (precision && points.size() > static_cast<size_t>(*precision))
                    points.resize(*precision);
                auto padding = std::max<std::int64_t>(0, std::abs(std::int64_t(width)) -
                                                             static_cast<std::int64_t>(points.size()));
                auto left = width < 0 || spec.find('-') != spec.npos;
                if (!left)
                    output.append(static_cast<size_t>(padding), ' ');
                for (auto point : points)
                    output += encode_codepoint(point);
                if (left)
                    output.append(static_cast<size_t>(padding), ' ');
            } else
                output += print_value(spec + "s", text.c_str());
            break;
        }
        case 'c':
        case 'C': {
            auto value = static_cast<std::uint32_t>(args.word());
            if (wide || length == "l" || c == 'C') {
                auto text = encode_codepoint(value);
                auto padding = std::max<std::int64_t>(
                    0, std::abs(std::int64_t(width)) - (wide ? 1 : static_cast<std::int64_t>(text.size())));
                auto left = width < 0 || spec.find('-') != spec.npos;
                if (!left)
                    output.append(static_cast<size_t>(padding), ' ');
                output += text;
                if (left)
                    output.append(static_cast<size_t>(padding), ' ');
            } else
                output += print_value(spec + "c", static_cast<int>(value));
            break;
        }
        case 'p':
            output += std::format("0x{:x}", args.word());
            break;
        case 'n': {
            auto p = args.word();
            auto count = wide ? utf32(output).size() : output.size();
            if (length == "l" || length == "ll" || length == "z" || length == "j" || length == "t")
                cpu.memory().store(p, static_cast<std::int64_t>(count));
            else if (length == "h")
                cpu.memory().store(p, static_cast<std::int16_t>(count));
            else if (length == "hh")
                cpu.memory().store(p, static_cast<std::int8_t>(count));
            else
                cpu.memory().store(p, static_cast<std::int32_t>(count));
            break;
        }
        default:
            throw Error("Unsupported printf conversion");
        }
        require(output.size() <= 16 << 20, "Guest printf output exceeds limit");
    }
    return output;
}
} // namespace
std::string format_guest(guest::Cpu &cpu, Address pointer, size_t first, Address va) {
    return format_text(cpu, cpu.memory().string(pointer), first, va, false);
}

void install_bionic(Runtime &r) {
    auto bind = [&](std::string name, auto handler) { r.bind(std::move(name), std::move(handler)); };
    auto &m = r.memory;
    bind("malloc", [&](auto &c) {
        return m.allocate(c.x(0), guest::Read | guest::Write, 16, guest::Initialize::Uninitialized);
    });
    bind("calloc", [&](auto &c) {
        require(!c.x(1) || c.x(0) <= SIZE_MAX / c.x(1), "calloc overflow");
        return m.allocate(c.x(0) * c.x(1));
    });
    bind("free", [&](auto &c) {
        m.release(c.x(0));
        return 0;
    });
    bind("realloc", [&](auto &c) { return m.reallocate(c.x(0), c.x(1)); });
    bind("memalign", [&](auto &c) {
        return m.allocate(c.x(1), guest::Read | guest::Write, c.x(0), guest::Initialize::Uninitialized);
    });
    bind("posix_memalign", [&](auto &c) {
        if (c.x(1) < 8 || !std::has_single_bit(c.x(1)))
            return 22;
        m.store(c.x(0),
                m.allocate(c.x(2), guest::Read | guest::Write, c.x(1), guest::Initialize::Uninitialized));
        return 0;
    });
    bind("malloc_usable_size", [&](auto &c) { return m.allocation_size(c.x(0)); });
    for (const auto *name : {"memcpy", "memmove", "__memcpy_chk", "__memmove_chk"})
        bind(name, [&, fortified = std::string_view(name).starts_with("__")](auto &c) {
            auto size = c.x(2);
            if (fortified)
                require(size <= c.x(3), "Fortified memory copy overflow");
            auto src = m.readable(c.x(1), size);
            auto dst = m.writable(c.x(0), size);
            std::memmove(dst.data(), src.data(), size);
            return c.x(0);
        });
    for (const auto *name : {"memset", "__memset_chk"})
        bind(name, [&, fortified = std::string_view(name).starts_with("__")](auto &c) {
            auto size = c.x(2);
            if (fortified)
                require(size <= c.x(3), "Fortified memset overflow");
            auto dst = m.writable(c.x(0), size);
            std::memset(dst.data(), static_cast<int>(c.x(1)), size);
            return c.x(0);
        });
    bind("memcmp", [&](auto &c) {
        return std::memcmp(m.readable(c.x(0), c.x(2)).data(), m.readable(c.x(1), c.x(2)).data(), c.x(2));
    });
    for (const auto *name : {"memchr", "memrchr"})
        bind(name, [&](auto &c) -> Address {
            auto b = m.readable(c.x(0), c.x(2));
            if (c.current_import == "memrchr") {
                for (size_t i = b.size(); i > 0; --i)
                    if (b[i - 1] == std::byte(c.x(1) & 255))
                        return c.x(0) + i - 1;
            } else {
                auto it = std::ranges::find(b, std::byte(c.x(1) & 255));
                if (it != b.end())
                    return c.x(0) + (it - b.begin());
            }
            return 0;
        });
    for (const auto *name : {"strlen", "__strlen_chk", "strnlen"})
        bind(name, [&, unbounded = std::string_view(name) == "strlen",
                    fortified = std::string_view(name) == "__strlen_chk"](auto &c) {
            auto max = unbounded ? size_t(1 << 20) : static_cast<size_t>(c.x(1));
            auto n = m.string_length(c.x(0), max);
            if (fortified)
                require(n < max, "Fortified strlen overflow");
            return n;
        });
    for (const auto *name : {"strdup", "strndup"})
        bind(name, [&, bounded = std::string_view(name) == "strndup"](auto &c) {
            auto maximum = bounded ? static_cast<size_t>(c.x(1)) : size_t(1 << 20);
            auto size = m.string_length(c.x(0), maximum);
            require(bounded || size < maximum, "Unterminated guest string");
            auto out =
                m.allocate(size + 1, guest::Read | guest::Write, 16, guest::Initialize::Uninitialized);
            m.write(out, m.readable(c.x(0), size));
            m.store(out + size, char{});
            return out;
        });
    for (const auto *name : {"strcmp", "strncmp", "strcasecmp", "strncasecmp"})
        bind(name,
             [&, bounded = std::string_view(name) == "strncmp" || std::string_view(name) == "strncasecmp",
              insensitive = std::string_view(name).find("casecmp") != std::string_view::npos](auto &c) {
                 return compare_strings(m, c.x(0), c.x(1), bounded ? size_t(c.x(2)) : size_t(1 << 20),
                                        insensitive, bounded);
             });
    for (const auto *name :
         {"strcpy", "__strcpy_chk", "strncpy", "__strncpy_chk", "__strncpy_chk2", "strcat", "__strcat_chk",
          "strncat", "__strncat_chk", "strlcpy", "__strlcpy_chk"})
        bind(name, [&](auto &c) {
            auto name = c.current_import, text = m.string(c.x(1));
            auto original = text.size();
            bool bounded = name.find("strncpy") != name.npos || name.find("strncat") != name.npos;
            if (bounded)
                text.resize(std::min<size_t>(text.size(), c.x(2)));
            bool lcopy = name.find("strlcpy") != name.npos;
            auto capacity = lcopy ? c.x(2) : UINT64_MAX;
            if (lcopy) {
                if (capacity)
                    text.resize(std::min<size_t>(text.size(), capacity - 1));
                else
                    return static_cast<std::uint64_t>(original);
            }
            auto destination = c.x(0);
            if (name.find("strcat") != name.npos || name.find("strncat") != name.npos)
                destination += m.string(destination).size();
            if (name.starts_with("__")) {
                auto cap = bounded || lcopy ? c.x(3) : c.x(2);
                require(destination - c.x(0) + text.size() + (name.find("strncpy") == name.npos ? 1 : 0) <=
                            cap,
                        "Fortified string operation overflow");
            }
            m.write(destination, std::as_bytes(std::span(text)));
            if (name.find("strncpy") != name.npos) {
                if (text.size() < c.x(2)) {
                    auto out = m.writable(destination + text.size(), c.x(2) - text.size());
                    std::memset(out.data(), 0, out.size());
                }
            } else
                m.store(destination + text.size(), char{});
            return lcopy ? static_cast<std::uint64_t>(original) : c.x(0);
        });
    for (const auto *name : {"strchr", "strrchr", "__strchr_chk", "strstr", "strpbrk"})
        bind(name, [&](auto &c) -> Address {
            auto text = m.string(c.x(0));
            auto name = c.current_import;
            if (name == "__strchr_chk")
                require(text.size() < c.x(2), "Fortified strchr overflow");
            size_t pos = name == "strstr"    ? text.find(m.string(c.x(1)))
                         : name == "strpbrk" ? text.find_first_of(m.string(c.x(1)))
                         : name == "strrchr" ? text.rfind(static_cast<char>(c.x(1)))
                                             : text.find(static_cast<char>(c.x(1)));
            if (c.x(1) == 0 && name != "strstr" && name != "strpbrk")
                pos = text.size();
            return pos == std::string::npos ? 0 : c.x(0) + pos;
        });
    bind("strspn", [&](auto &c) {
        auto s = m.string(c.x(0));
        auto p = s.find_first_not_of(m.string(c.x(1)));
        return p == s.npos ? s.size() : p;
    });
    bind("strcspn", [&](auto &c) {
        auto s = m.string(c.x(0));
        auto p = s.find_first_of(m.string(c.x(1)));
        return p == s.npos ? s.size() : p;
    });
    bind("strtok_r", [&](auto &c) -> Address {
        auto p = c.x(0) ? c.x(0) : m.load<Address>(c.x(2));
        if (!p)
            return 0;
        auto delimiters = m.string(c.x(1)), s = m.string(p);
        auto begin = s.find_first_not_of(delimiters);
        if (begin == s.npos) {
            m.store(c.x(2), Address{});
            return 0;
        }
        auto end = s.find_first_of(delimiters, begin);
        if (end == s.npos)
            m.store(c.x(2), Address{});
        else {
            m.store(p + end, char{});
            m.store(c.x(2), p + end + 1);
        }
        return p + begin;
    });
    auto strtok_state = m.allocate(8);
    bind("strtok", [&, strtok_state](auto &c) {
        auto saved = c.x(2);
        c.x(2, strtok_state);
        auto target = r.linker.symbol("strtok_r");
        auto value = c.call(target, std::array{c.x(0), c.x(1), strtok_state});
        c.x(2, saved);
        return value;
    });
    bind("strsep", [&](auto &c) -> Address {
        auto p = m.load<Address>(c.x(0));
        if (!p)
            return 0;
        auto s = m.string(p), d = m.string(c.x(1));
        auto end = s.find_first_of(d);
        if (end == s.npos)
            m.store(c.x(0), Address{});
        else {
            m.store(p + end, char{});
            m.store(c.x(0), p + end + 1);
        }
        return p;
    });
    for (const auto *name :
         {"atoi", "atol", "atoll", "strtol", "strtoll", "strtoul", "strtoull", "atof", "strtod", "strtof"})
        bind(name, [&](auto &c) {
            auto text = m.string(c.x(0));
            char *end{};
            const auto &name = c.current_import;
            if (name == "atof" || name == "strtod" || name == "strtof") {
                auto value = std::strtod(text.c_str(), &end);
                if (name != "atof" && c.x(1))
                    m.store(c.x(1), c.x(0) + static_cast<size_t>(end - text.c_str()));
                if (name == "strtof")
                    c.f32(0, static_cast<float>(value));
                else
                    c.f64(0, value);
                return std::uint64_t{};
            }
            int base = name.starts_with("str") ? static_cast<int>(c.x(2)) : 10;
            auto value = name == "strtoul" || name == "strtoull"
                             ? std::strtoull(text.c_str(), &end, base)
                             : static_cast<std::uint64_t>(std::strtoll(text.c_str(), &end, base));
            if (name.starts_with("str") && c.x(1))
                m.store(c.x(1), c.x(0) + static_cast<size_t>(end - text.c_str()));
            return value;
        });
    for (const auto *name :
         {"wcslen", "wcscmp", "wmemcmp", "wmemchr", "wcsncpy", "wmemcpy", "wmemmove", "wmemset"})
        bind(name, [&](auto &c) -> std::uint64_t {
            const auto &name = c.current_import;
            auto size = c.x(2);
            if (name == "wcslen") {
                size = 0;
                while (m.load<std::uint32_t>(c.x(0) + size * 4)) {
                    require(size < 1 << 20, "Unterminated wide string");
                    ++size;
                }
                return size;
            }
            if (name == "wcscmp" || name == "wmemcmp") {
                return static_cast<std::uint64_t>(static_cast<std::int64_t>(compare_wide(
                    m, c.x(0), c.x(1), name == "wcscmp" ? size_t(1 << 20) : size, name == "wcscmp")));
            }
            if (name == "wmemchr") {
                for (size_t i = 0; i < size; ++i)
                    if (m.load<std::uint32_t>(c.x(0) + i * 4) == c.x(1))
                        return c.x(0) + i * 4;
                return 0;
            }
            if (name == "wmemset")
                for (size_t i = 0; i < size; ++i)
                    m.store(c.x(0) + i * 4, static_cast<std::uint32_t>(c.x(1)));
            else if (name == "wcsncpy") {
                bool zero = false;
                for (size_t i = 0; i < size; ++i) {
                    auto value = zero ? 0 : m.load<std::uint32_t>(c.x(1) + i * 4);
                    zero |= value == 0;
                    m.store(c.x(0) + i * 4, value);
                }
            } else {
                auto src = m.readable(c.x(1), size * 4);
                auto dst = m.writable(c.x(0), size * 4);
                std::memmove(dst.data(), src.data(), src.size());
            }
            return c.x(0);
        });
    for (const auto *name : {"tolower", "toupper", "isalpha", "isalnum", "isdigit", "isspace", "isupper",
                             "islower", "isxdigit", "isprint", "iswalnum", "towlower", "towupper"})
        bind(name, [](auto &c) {
            auto v = static_cast<int>(c.x(0));
            const auto &name = c.current_import;
            if (name == "tolower" || name == "towlower")
                return std::tolower(v & 255);
            if (name == "toupper" || name == "towupper")
                return std::toupper(v & 255);
            if (name == "isalpha")
                return std::isalpha(v & 255);
            if (name == "isalnum" || name == "iswalnum")
                return std::isalnum(v & 255);
            if (name == "isdigit")
                return std::isdigit(v & 255);
            if (name == "isspace")
                return std::isspace(v & 255);
            if (name == "isupper")
                return std::isupper(v & 255);
            if (name == "islower")
                return std::islower(v & 255);
            if (name == "isxdigit")
                return std::isxdigit(v & 255);
            return std::isprint(v & 255);
        });

    const std::vector<std::pair<std::string, std::function<double(double)>>> unary{
        {"sin", [](double x) {return std::sin(x); }},
        {"cos", [](double x) {return std::cos(x); }},
        {"tan", [](double x) {return std::tan(x); }},
        {"asin", [](double x) {return std::asin(x); }},
        {"acos", [](double x) {return std::acos(x); }},
        {"atan", [](double x) {return std::atan(x); }},
        {"sqrt", [](double x) {return std::sqrt(x); }},
        {"floor", [](double x) {return std::floor(x); }},
        {"ceil", [](double x) {return std::ceil(x); }},
        {"round", [](double x) {return std::round(x); }},
        {"trunc", [](double x) {return std::trunc(x); }},
        {"fabs", [](double x) {return std::fabs(x); }},
        {"log", [](double x) {return std::log(x); }},
        {"log2", [](double x) {return std::log2(x); }},
        {"log10", [](double x) {return std::log10(x); }},
        {"exp", [](double x) {return std::exp(x); }},
        {"exp2", [](double x) {return std::exp2(x); }}};
    for (const auto &[name, fn] : unary) {
        r.bind_void(name, [fn](auto &c) { c.f64(0, fn(c.f64(0))); });
        r.bind_void(name + "f", [fn](auto &c) { c.f32(0, static_cast<float>(fn(c.f32(0)))); });
    }
    const std::vector<std::pair<std::string, std::function<double(double, double)>>> binary{
        {"pow", [](double x, double y) { return std::pow(x, y); }},
        {"atan2", [](double x, double y) { return std::atan2(x, y); }},
        {"fmod", [](double x, double y) { return std::fmod(x, y); }},
        {"fmin", [](double x, double y) { return std::fmin(x, y); }},
        {"fmax", [](double x, double y) { return std::fmax(x, y); }}};
    for (const auto &[name, fn] : binary) {
        r.bind_void(name, [fn](auto &c) { c.f64(0, fn(c.f64(0), c.f64(1))); });
        r.bind_void(name + "f", [fn](auto &c) { c.f32(0, static_cast<float>(fn(c.f32(0), c.f32(1)))); });
    }
    for (const auto *name : {"sincos", "sincosf"})
        r.bind_void(name, [&](auto &c) {
            if (c.current_import == "sincosf") {
                m.store(c.x(0), std::sin(c.f32(0)));
                m.store(c.x(1), std::cos(c.f32(0)));
            } else {
                m.store(c.x(0), std::sin(c.f64(0)));
                m.store(c.x(1), std::cos(c.f64(0)));
            }
        });
    for (const auto *name : {"modf", "modff"})
        r.bind_void(name, [&](auto &c) {
            if (c.current_import == "modff") {
                float whole;
                auto fract = std::modf(c.f32(0), &whole);
                m.store(c.x(0), whole);
                c.f32(0, fract);
            } else {
                double whole;
                auto fract = std::modf(c.f64(0), &whole);
                m.store(c.x(0), whole);
                c.f64(0, fract);
            }
        });
    r.bind_void("difftime", [](auto &c) {
        c.f64(0, static_cast<double>(std::bit_cast<std::int64_t>(c.x(0))) -
                     static_cast<double>(std::bit_cast<std::int64_t>(c.x(1))));
    });
    bind("rand", [](auto &) { return std::rand(); });
    bind("srand", [](auto &c) {
        std::srand(static_cast<unsigned>(c.x(0)));
        return 0;
    });
    bind("__errno", [&](auto &c) { return r.errno_address(c); });
    bind("__system_property_get", [&](auto &c) {
        auto key = m.string(c.x(0));
        std::string value;
        if (key == "ro.build.version.sdk")
            value = "36";
        else if (key == "ro.product.cpu.abi")
            value = "arm64-v8a";
        else if (key == "ro.product.model")
            value = "Desktop";
        else if (key == "ro.product.manufacturer")
            value = "AndroidPcRuntime";
        else if (key == "ro.build.version.release")
            value = "16";
        require(value.size() < 92, "System property overflow");
        m.write(c.x(1), std::as_bytes(std::span(value)));
        m.store(c.x(1) + value.size(), char{});
        return value.size();
    });
    bind("getpid", [](auto &) { return GetCurrentProcessId(); });
    bind("getppid", [](auto &) { return 1; });
    bind("geteuid", [](auto &) { return 10000; });
    bind("getuid", [](auto &) { return 10000; });
    bind("getauxval", [](auto &c) -> std::uint64_t {
        if (c.x(0) == 6)
            return 4096;
        if (c.x(0) == 16)
            return 3;
        return 0;
    });
    bind("getpagesize", [](auto &) { return 4096; });
    bind("sysconf", [](auto &c) -> std::uint64_t {
        if (c.x(0) == 39)
            return 4096;
        if (c.x(0) == 97)
            return std::max(1u, std::thread::hardware_concurrency());
        return UINT64_MAX;
    });
    bind("time", [&](auto &c) {
        auto value = static_cast<std::int64_t>(std::time(nullptr));
        if (c.x(0))
            m.store(c.x(0), value);
        return value;
    });
    bind("gettimeofday", [&](auto &c) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
        if (c.x(0)) {
            m.store(c.x(0), us / 1'000'000);
            m.store(c.x(0) + 8, us % 1'000'000);
        }
        if (c.x(1)) {
            m.store(c.x(1), std::int64_t{});
        }
        return 0;
    });
    bind("clock_gettime", [&](auto &c) {
        auto ns = c.x(0) == 0 ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count()
                              : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
        m.store(c.x(1), ns / 1'000'000'000);
        m.store(c.x(1) + 8, ns % 1'000'000'000);
        return 0;
    });
    bind("usleep", [](auto &c) {
        std::this_thread::sleep_for(std::chrono::microseconds(c.x(0)));
        return 0;
    });
    bind("sleep", [](auto &c) {
        std::this_thread::sleep_for(std::chrono::seconds(c.x(0)));
        return 0;
    });
    bind("nanosleep", [&](auto &c) {
        auto sec = m.load<std::int64_t>(c.x(0)), ns = m.load<std::int64_t>(c.x(0) + 8);
        if (sec < 0 || ns < 0 || ns >= 1'000'000'000)
            return r.failure(c, 22);
        std::this_thread::sleep_for(std::chrono::seconds(sec) + std::chrono::nanoseconds(ns));
        return std::uint64_t{};
    });
    auto tm_buffer = m.allocate(64), time_string = m.allocate(128);
    auto zones = std::make_shared<std::map<std::string, Address>>();
    auto guest_tm = [&r, &m, zones](const host::CalendarTime &tm, Address p) {
        std::lock_guard lock(r.state_mutex);
        auto bytes = m.writable(p, 56);
        std::memset(bytes.data(), 0, bytes.size());
        for (size_t i = 0; i < 9; ++i)
            m.store(p + i * 4, tm.fields[i]);
        auto [zone, inserted] = zones->try_emplace(tm.zone);
        if (inserted)
            zone->second = m.string(tm.zone);
        m.store(p + 40, tm.utc_offset);
        m.store(p + 48, zone->second);
    };
    for (const auto *name : {"gmtime", "gmtime_r", "localtime", "localtime_r"})
        bind(name, [&, guest_tm, tm_buffer](auto &c) {
            auto time = m.load<std::int64_t>(c.x(0));
            auto tm = host::calendar_time(time, c.current_import.starts_with("gmtime"));
            if (!tm) {
                r.failure(c, tm.error());
                return Address{};
            }
            auto p = c.current_import.ends_with("_r") ? c.x(1) : tm_buffer;
            guest_tm(*tm, p);
            return p;
        });
    for (const auto *name : {"mktime", "timegm"})
        bind(name, [&, guest_tm](auto &c) {
            std::tm tm{};
            int *fields[] = {&tm.tm_sec,  &tm.tm_min,  &tm.tm_hour, &tm.tm_mday, &tm.tm_mon,
                             &tm.tm_year, &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst};
            for (size_t i = 0; i < 9; ++i)
                *fields[i] = m.load<std::int32_t>(c.x(0) + i * 4);
            auto utc = c.current_import == "timegm";
            auto result = utc ? _mkgmtime64(&tm) : _mktime64(&tm);
            if (result != -1) {
                auto normalized = host::calendar_time(result, utc);
                if (normalized)
                    guest_tm(*normalized, c.x(0));
            }
            return result;
        });
    bind("strftime", [&](auto &c) {
        std::tm tm{};
        int *fields[] = {&tm.tm_sec,  &tm.tm_min,  &tm.tm_hour, &tm.tm_mday, &tm.tm_mon,
                         &tm.tm_year, &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst};
        for (size_t i = 0; i < 9; ++i)
            *fields[i] = m.load<std::int32_t>(c.x(3) + i * 4);
        auto out = m.writable(c.x(0), c.x(1));
        return std::strftime(reinterpret_cast<char *>(out.data()), out.size(), m.string(c.x(2)).c_str(),
                             &tm);
    });
    bind("ctime", [&, time_string](auto &c) {
        auto t = static_cast<__time64_t>(m.load<std::int64_t>(c.x(0)));
        char buffer[64]{};
        _ctime64_s(buffer, sizeof(buffer), &t);
        m.write(time_string, std::as_bytes(std::span(buffer)));
        return time_string;
    });
    bind("setlocale", [&](auto &) { return m.string("C"); });
    bind("newlocale", [&](auto &c) -> Address {
        auto name = m.string(c.x(1));
        if (name != "C" && name != "POSIX" && name != "C.UTF-8" && name != "en_US.UTF-8") {
            r.failure(c, 2);
            return 0;
        }
        return m.allocate(64);
    });
    bind("freelocale", [&](auto &c) {
        if (c.x(0) && c.x(0) != UINT64_MAX)
            m.release(c.x(0));
        return 0;
    });
    bind("uselocale", [&](auto &c) {
        auto old = m.load<Address>(c.tls() + 48);
        if (!old)
            old = UINT64_MAX;
        if (c.x(0))
            m.store(c.tls() + 48, c.x(0));
        return old;
    });
    auto locale_data = m.allocate(96);
    for (size_t i = 0; i < 10; ++i)
        m.store(locale_data + i * 8, m.string(i == 0 ? "." : ""));
    for (size_t i = 80; i < 96; ++i)
        m.store(locale_data + i, std::uint8_t{127});
    bind("localeconv", [locale_data](auto &) { return locale_data; });
    bind("__ctype_get_mb_cur_max", [](auto &) { return 4; });
    bind("btowc", [](auto &c) { return c.x(0) < 128 ? c.x(0) : std::uint64_t{UINT32_MAX}; });
    bind("wctob", [](auto &c) { return c.x(0) < 128 ? c.x(0) : UINT64_MAX; });
    for (const auto *name : {"mbtowc", "mbrtowc", "mbrlen"})
        bind(name, [&](auto &c) -> std::uint64_t {
            bool length = c.current_import == "mbrlen";
            auto input = c.x(length ? 0 : 1), count = c.x(length ? 1 : 2);
            if (!input)
                return 0;
            auto [value, bytes] = codepoint(m, input, count);
            if (bytes >= SIZE_MAX - 1) {
                if (c.current_import == "mbtowc" || bytes == SIZE_MAX)
                    return r.failure(c, 84);
                return SIZE_MAX - 1;
            }
            if (!length && c.x(0))
                m.store(c.x(0), value);
            return bytes;
        });
    bind("wcrtomb", [&](auto &c) -> std::uint64_t {
        if (!c.x(0))
            return 1;
        auto text = encode_codepoint(static_cast<std::uint32_t>(c.x(1)));
        m.write(c.x(0), std::as_bytes(std::span(text)));
        return text.size();
    });
    for (const auto *name : {"mbsrtowcs", "mbsnrtowcs"})
        bind(name, [&](auto &c) -> std::uint64_t {
            auto input = m.load<Address>(c.x(1));
            bool bounded = c.current_import == "mbsnrtowcs";
            auto available = bounded ? c.x(2) : UINT64_MAX, capacity = c.x(bounded ? 3 : 2);
            size_t count = 0, used = 0;
            while (!c.x(0) || count < capacity) {
                auto [value, n] = codepoint(m, input + used, available - used);
                if (n == SIZE_MAX)
                    return r.failure(c, 84);
                if (n == SIZE_MAX - 1)
                    break;
                if (!n) {
                    if (c.x(0)) {
                        m.store(c.x(0) + count * 4, std::uint32_t{});
                        m.store(c.x(1), Address{});
                    }
                    return count;
                }
                if (c.x(0))
                    m.store(c.x(0) + count * 4, value);
                used += n;
                ++count;
                require(count < 1 << 24, "Excessive multibyte conversion");
                if (used == available)
                    break;
            }
            if (c.x(0))
                m.store(c.x(1), input + used);
            return count;
        });
    for (const auto *name : {"wcsrtombs", "wcsnrtombs"})
        bind(name, [&](auto &c) -> std::uint64_t {
            auto input = m.load<Address>(c.x(1));
            bool bounded = c.current_import == "wcsnrtombs";
            auto available = bounded ? c.x(2) : UINT64_MAX, capacity = c.x(bounded ? 3 : 2);
            size_t count = 0, index = 0;
            for (; index < available; ++index) {
                auto value = m.load<std::uint32_t>(input + index * 4);
                auto text = encode_codepoint(value);
                if (c.x(0) && text.size() > capacity - count)
                    break;
                if (!value) {
                    if (c.x(0)) {
                        m.store(c.x(0) + count, char{});
                        m.store(c.x(1), Address{});
                    }
                    return count;
                }
                if (c.x(0))
                    m.write(c.x(0) + count, std::as_bytes(std::span(text)));
                count += text.size();
                require(count < 1 << 24, "Excessive wide conversion");
            }
            if (c.x(0))
                m.store(c.x(1), input + index * 4);
            return count;
        });
    for (const auto *name : {"strcoll_l", "wcscoll_l"})
        bind(name, [&](auto &c) {
            auto a = c.current_import == "strcoll_l" ? m.string(c.x(0)) : wide_string(m, c.x(0));
            auto b = c.current_import == "strcoll_l" ? m.string(c.x(1)) : wide_string(m, c.x(1));
            return a.compare(b);
        });
    for (const auto *name : {"strxfrm_l", "wcsxfrm_l"})
        bind(name, [&](auto &c) {
            bool wide = c.current_import == "wcsxfrm_l";
            auto length = wide ? wide_string(m, c.x(1)).size() : m.string(c.x(1)).size();
            if (wide) {
                length = 0;
                while (m.load<std::uint32_t>(c.x(1) + length * 4))
                    ++length;
            }
            if (c.x(2) > length) {
                auto size = (length + 1) * (wide ? 4 : 1);
                m.write(c.x(0), m.readable(c.x(1), size));
            }
            return length;
        });
    for (const auto *name :
         {"iswalpha_l", "iswblank_l", "iswcntrl_l", "iswdigit_l", "iswlower_l", "iswprint_l", "iswpunct_l",
          "iswspace_l", "iswupper_l", "iswxdigit_l", "towlower_l", "towupper_l"})
        bind(name, [](auto &c) {
            auto ch = static_cast<wint_t>(c.x(0));
            const auto &name = c.current_import;
            if (name == "iswalpha_l")
                return int(std::iswalpha(ch));
            if (name == "iswblank_l")
                return int(std::iswblank(ch));
            if (name == "iswcntrl_l")
                return int(std::iswcntrl(ch));
            if (name == "iswdigit_l")
                return int(std::iswdigit(ch));
            if (name == "iswlower_l")
                return int(std::iswlower(ch));
            if (name == "iswprint_l")
                return int(std::iswprint(ch));
            if (name == "iswpunct_l")
                return int(std::iswpunct(ch));
            if (name == "iswspace_l")
                return int(std::iswspace(ch));
            if (name == "iswupper_l")
                return int(std::iswupper(ch));
            if (name == "iswxdigit_l")
                return int(std::iswxdigit(ch));
            return int(name == "towlower_l" ? std::towlower(ch) : std::towupper(ch));
        });
    auto timezone = m.allocate(8);
    r.linker.provide_data("timezone", timezone);
    for (const auto *name : {"getentropy", "arc4random_buf"})
        bind(name, [&](auto &c) {
            auto buffer = m.writable(c.x(0), c.x(1));
            if (c.current_import == "getentropy" && buffer.size() > 256)
                return r.failure(c, 5);
            require(buffer.size() <= ULONG_MAX, "Random buffer too large");
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buffer.data()),
                                static_cast<ULONG>(buffer.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                return r.failure(c, 5);
            return std::uint64_t{};
        });
    bind("getenv", [](auto &) -> Address { return 0; });
    bind("setenv", [](auto &) { return 0; });
    bind("unsetenv", [](auto &) { return 0; });
    for (const auto *name : {"abort", "__stack_chk_fail", "__assert2", "__assert", "__fortify_fatal"})
        bind(name,
             [](auto &c) -> std::uint64_t { throw Error("Guest fatal condition: " + c.current_import); });
    bind("exit",
         [](auto &c) -> std::uint64_t { throw Error("Guest exit(" + std::to_string(c.x(0)) + ")"); });
    bind("_exit", [](auto &c) -> std::uint64_t { throw ThreadExit{c.x(0)}; });
    bind("__cxa_atexit", [](auto &) { return 0; });
    bind("__cxa_finalize", [](auto &) { return 0; });
    bind("__register_atfork", [](auto &) { return 0; });
    for (const auto *name : {"fork", "execv", "system", "mknod", "mkfifo", "ptrace", "waitpid", "link",
                             "symlink", "readlink", "__readlink_chk", "pipe"})
        r.bind_suppressed(name, "unrelated-posix", "-1 / ENOSYS; no host process, device, pipe or link is created",
                          [&](auto &c) { return r.failure(c, 38); });
    for (const auto *name : {"getpwuid", "getpwnam", "getgrgid", "getgrnam"})
        bind(name, [](auto &) -> Address { return 0; });
    bind("getpwuid_r", [&](auto &c) {
        if (c.x(4))
            m.store(c.x(4), Address{});
        return 0;
    });

    // Host FILE objects are held behind guest-owned opaque handles.
    auto stdio = m.allocate(3 * 152);
    r.file_handles[stdio] = 0;
    r.file_handles[stdio + 152] = 1;
    r.file_handles[stdio + 304] = 2;
    r.linker.provide_data("__sF", stdio);
    for (auto [name, handle] : std::vector<std::pair<std::string, Address>>{
             {"stdin", stdio}, {"stdout", stdio + 152}, {"stderr", stdio + 304}}) {
        auto slot = m.allocate(8);
        m.store(slot, handle);
        r.linker.provide_data(name, slot);
    }
    bind("fopen", [&](auto &c) -> Address {
        auto mode = m.string(c.x(1));
        bool writing = mode.find_first_of("wa+") != mode.npos;
        auto filename = m.string(c.x(0));
        if (filename == "/dev/null") {
            auto descriptor = r.open_device(filename);
            if (descriptor < 0) {
                r.failure(c, 5);
                return 0;
            }
            return r.file_handle(descriptor);
        }
        auto path = r.resolve_path(filename, writing);
        if (!path) {
            r.file_failure(filename, path.error());
            r.failure(c, path.error());
            return 0;
        }
        try {
            auto id = r.open_file(*path, mode);
            if (id < 0) {
                r.file_failure(filename, 2);
                r.failure(c, 2);
                return 0;
            }
            return r.file_handle(id);
        } catch (const fs::filesystem_error &) {
            r.failure(c, 2);
            return 0;
        }
    });
    bind("fclose", [&](auto &c) {
        auto id = r.descriptor(c.x(0));
        auto result = r.close_file(id);
        if (result)
            r.failure(c, 9);
        return result;
    });
    bind("fileno", [&](auto &c) { return r.descriptor(c.x(0)); });
    for (const auto *name : {"fread", "fwrite", "__fread_chk", "__fwrite_chk"})
        bind(name, [&](auto &c) {
            auto count = c.x(1), size = c.x(2);
            require(!size || count <= SIZE_MAX / size, "Guest stdio size overflow");
            auto handle = c.x(3);
            if (c.current_import.starts_with("__"))
                require(count * size <= c.x(4), "Fortified stdio buffer overflow");
            auto fp = r.file(r.descriptor(handle));
            if (!fp) {
                r.failure(c, 9);
                return size_t{};
            }
            if (c.current_import == "fread" || c.current_import == "__fread_chk") {
                auto dst = m.writable(c.x(0), count * size);
                return count ? r.read_stream(r.descriptor(handle), dst) / count : 0;
            }
            auto src = m.readable(c.x(0), count * size);
            return std::fwrite(src.data(), count, size, fp);
        });
    bind("fflush", [&](auto &c) {
        auto fp = c.x(0) ? r.file(r.descriptor(c.x(0))) : nullptr;
        if (c.x(0) && !fp)
            return static_cast<int>(r.failure(c, 9));
        return std::fflush(fp);
    });
    bind("fseek", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(0)));
        return fp ? r.seek_stream(r.descriptor(c.x(0)), std::bit_cast<std::int64_t>(c.x(1)), static_cast<int>(c.x(2))) : -1;
    });
    bind("fseeko", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(0)));
        return fp ? r.seek_stream(r.descriptor(c.x(0)), std::bit_cast<std::int64_t>(c.x(1)), static_cast<int>(c.x(2))) : -1;
    });
    bind("ftell", [&](auto &c) -> std::int64_t {
        auto fp = r.file(r.descriptor(c.x(0)));
        return fp ? r.stream_position(r.descriptor(c.x(0))) : -1;
    });
    bind("fsetpos", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(0)));
        return fp ? r.seek_stream(r.descriptor(c.x(0)), m.load<std::int64_t>(c.x(1)), SEEK_SET) : -1;
    });
    bind("fgetpos", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(0)));
        if (!fp)
            return -1;
        m.store(c.x(1), r.stream_position(r.descriptor(c.x(0))));
        return 0;
    });
    for (const auto *name : {"ferror", "feof", "fgetc"})
        bind(name, [&](auto &c) {
            auto fp = r.file(r.descriptor(c.x(0)));
            if (!fp)
                return -1;
            if (c.current_import == "ferror")
                return std::ferror(fp);
            if (c.current_import == "feof")
                return int(r.stream_eof(r.descriptor(c.x(0))));
            std::byte byte;
            return r.read_stream(r.descriptor(c.x(0)), std::span(&byte, 1)) ? int(std::to_integer<unsigned char>(byte)) : EOF;
        });
    bind("fputc", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(1)));
        return fp ? std::fputc(static_cast<int>(c.x(0)), fp) : -1;
    });
    bind("putchar", [](auto &c) { return std::putchar(static_cast<int>(c.x(0))); });
    bind("fputs", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(1)));
        return fp ? std::fputs(m.string(c.x(0)).c_str(), fp) : -1;
    });
    bind("puts", [&](auto &c) { return std::puts(m.string(c.x(0)).c_str()); });
    bind("fgets", [&](auto &c) -> Address {
        auto fp = r.file(r.descriptor(c.x(2)));
        if (!fp || !c.x(1))
            return 0;
        auto out = m.writable(c.x(0), c.x(1));
        size_t count = 0;
        while (count + 1 < out.size()) {
            std::byte value;
            if (!r.read_stream(r.descriptor(c.x(2)), std::span(&value, 1))) break;
            out[count++] = value;
            if (value == std::byte{'\n'}) break;
        }
        out[count] = std::byte{};
        return count || out.size() == 1 ? c.x(0) : 0;
    });
    bind("ungetc", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(1)));
        if (!fp || static_cast<int>(c.x(0)) == EOF) return -1;
        std::byte value{static_cast<unsigned char>(c.x(0))};
        r.unread_stream(r.descriptor(c.x(1)), std::span(&value, 1));
        return int(std::to_integer<unsigned char>(value));
    });
    bind("setvbuf", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(0)));
        if (!fp)
            return -1;
        auto mode = c.x(2) == 0 ? _IOFBF : c.x(2) == 1 ? _IOLBF : _IONBF;
        return std::setvbuf(fp, nullptr, mode, c.x(3));
    });
    bind("fwide", [](auto &) { return 0; });
    auto syslog_identity = std::make_shared<std::string>();
    bind("openlog", [&, syslog_identity](auto &c) {
        std::lock_guard lock(r.state_mutex);
        *syslog_identity = c.x(0) ? m.string(c.x(0)) : "";
        return 0;
    });
    bind("closelog", [&, syslog_identity](auto &) {
        std::lock_guard lock(r.state_mutex);
        syslog_identity->clear();
        return 0;
    });
    for (const auto *name : {"syslog", "vsyslog"})
        bind(name, [&, syslog_identity](auto &c) {
            auto text = format_guest(c, c.x(1), 2, c.current_import == "vsyslog" ? c.x(2) : 0);
            std::lock_guard lock(r.state_mutex);
            r.message(std::format("[syslog/{}] {}: {}", c.x(0) & 7, *syslog_identity, text));
            return 0;
        });
    for (const auto *name : {"asprintf", "vasprintf"})
        bind(name, [&](auto &c) {
            auto output = format_guest(c, c.x(1), 2, c.current_import == "vasprintf" ? c.x(2) : 0);
            m.store(c.x(0), m.string(output));
            return static_cast<int>(output.size());
        });
    for (const auto *name : {"swprintf", "vswprintf"})
        bind(name, [&](auto &c) -> Address {
            auto capacity = c.x(1);
            if (!capacity)
                return r.failure(c, 22);
            auto text = format_text(c, wide_string(m, c.x(2)), 3,
                                    c.current_import == "vswprintf" ? c.x(3) : 0, true);
            auto result = utf32(text);
            auto count = std::min<std::uint64_t>(result.size(), capacity - 1);
            for (size_t i = 0; i < count; ++i)
                m.store(c.x(0) + i * 4, static_cast<std::uint32_t>(result[i]));
            m.store(c.x(0) + count * 4, std::uint32_t{});
            return result.size() >= capacity ? r.failure(c, 75) : result.size();
        });
    for (const auto *name : {"printf", "fprintf", "sprintf", "snprintf", "vsprintf", "vsnprintf",
                             "vfprintf", "__vsnprintf_chk", "__vsprintf_chk"})
        bind(name, [&](auto &c) {
            const auto &name = c.current_import;
            bool file = name == "fprintf" || name == "vfprintf", console = name == "printf",
                 limited = name.find("snprintf") != name.npos,
                 variadic = name.starts_with('v') || name.starts_with("__v");
            size_t fmt = console ? 0 : file ? 1 : limited ? 2 : 1;
            size_t arg = fmt + 1;
            if (name == "__vsnprintf_chk") {
                fmt = 4;
                arg = 5;
                require(c.x(1) <= c.x(3), "Fortified snprintf size mismatch");
            }
            if (name == "__vsprintf_chk") {
                fmt = 3;
                arg = 4;
            }
            auto output = format_guest(c, c.x(fmt), arg, variadic ? c.x(arg) : 0);
            if (console)
                std::fwrite(output.data(), 1, output.size(), stdout);
            else if (file) {
                auto fp = r.file(r.descriptor(c.x(0)));
                if (!fp)
                    return -1;
                std::fwrite(output.data(), 1, output.size(), fp);
            } else {
                auto capacity = limited ? c.x(1) : output.size() + 1;
                if (name == "__vsprintf_chk")
                    require(output.size() < c.x(2), "Fortified sprintf overflow");
                if (capacity) {
                    auto size = std::min<std::uint64_t>(capacity - 1, output.size());
                    m.write(c.x(0), std::as_bytes(std::span(output)).first(size));
                    m.store(c.x(0) + size, char{});
                }
            }
            return static_cast<int>(output.size());
        });
    for (const auto *name : {"__android_log_write", "__android_log_print", "__android_log_vprint"})
        bind(name, [&](auto &c) {
            auto text =
                c.current_import == "__android_log_write"
                    ? m.string(c.x(2))
                    : format_guest(c, c.x(2), 3, c.current_import == "__android_log_vprint" ? c.x(3) : 0);
            r.message("[android/" + m.string(c.x(1)) + "] " + text);
            return text.size();
        });

    bind("close", [&](auto &c) { return r.close_file(static_cast<int>(c.x(0))) ? r.failure(c, 9) : 0; });
    for (const auto *name : {"read", "__read_chk", "write", "__write_chk"})
        bind(name, [&](auto &c) {
            if (c.current_import.starts_with("__"))
                require(c.x(2) <= c.x(3), "Fortified descriptor IO overflow");
            auto device = r.device(static_cast<int>(c.x(0)));
            bool reading = c.current_import.find("read") != std::string::npos;
            if (!device.empty()) {
                if (!reading)
                    return c.x(2);
                if (device == "/dev/null")
                    return std::uint64_t{};
                auto bytes = m.writable(c.x(1), c.x(2));
                if (device == "/dev/zero")
                    std::memset(bytes.data(), 0, bytes.size());
                else {
                    require(bytes.size() <= ULONG_MAX, "Random read too large");
                    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                                        static_cast<ULONG>(bytes.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                        return r.failure(c, 5);
                }
                return c.x(2);
            }
            auto fp = r.file(static_cast<int>(c.x(0)));
            if (!fp)
                return r.failure(c, 9);
            if (reading) {
                auto b = m.writable(c.x(1), c.x(2));
                return static_cast<std::uint64_t>(r.read_stream(static_cast<int>(c.x(0)), b));
            }
            auto b = m.readable(c.x(1), c.x(2));
            return static_cast<std::uint64_t>(std::fwrite(b.data(), 1, b.size(), fp));
        });
    for (const auto *name : {"lseek", "lseek64"})
        bind(name, [&](auto &c) -> std::uint64_t {
            auto fp = r.file(static_cast<int>(c.x(0)));
            if (!fp)
                return r.failure(c, 9);
            if (r.seek_stream(static_cast<int>(c.x(0)), std::bit_cast<std::int64_t>(c.x(1)), static_cast<int>(c.x(2))))
                return r.failure(c, 22);
            return static_cast<std::uint64_t>(r.stream_position(static_cast<int>(c.x(0))));
        });
    bind("access", [&](auto &c) -> std::uint64_t {
        try {
            return fs::exists(r.path(m.string(c.x(0)))) ? 0 : r.failure(c, 2);
        } catch (const Error &) {
            return r.failure(c, 13);
        }
    });
    bind("mkdir", [&](auto &c) -> std::uint64_t {
        auto path = r.resolve_path(m.string(c.x(0)), true);
        if (!path)
            return r.failure(c, path.error());
        std::error_code e;
        auto created = fs::create_directory(*path, e);
        return created ? 0 : r.failure(c, e ? 2 : 17);
    });
    for (const auto *name : {"unlink", "remove", "rmdir"})
        bind(name, [&](auto &c) -> std::uint64_t {
            auto path = r.resolve_path(m.string(c.x(0)), true);
            if (!path)
                return r.failure(c, path.error());
            std::error_code e;
            return fs::remove(*path, e) ? 0 : r.failure(c, e ? 13 : 2);
        });
    bind("rename", [&](auto &c) -> std::uint64_t {
        auto from = r.resolve_path(m.string(c.x(0)), true), to = r.resolve_path(m.string(c.x(1)), true);
        if (!from || !to)
            return r.failure(c, !from ? from.error() : to.error());
        return MoveFileExW(from->c_str(), to->c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : r.failure(c, 13);
    });
    for (const auto *name : {"basename", "dirname"})
        bind(name, [&](auto &c) {
            auto text = m.string(c.x(0));
            auto pos = text.find_last_of('/');
            auto out = c.current_import == "basename" ? (pos == text.npos ? text : text.substr(pos + 1))
                                                      : (pos == text.npos ? "."
                                                         : pos == 0       ? "/"
                                                                          : text.substr(0, pos));
            return m.string(out);
        });

    bind("madvise", [](auto &) { return 0; });
    bind("mlock", [](auto &) { return 0; });

    auto get_mutex = [&](Address address) {
        std::lock_guard lock(r.state_mutex);
        auto &mutex = r.mutexes[address];
        if (!mutex)
            mutex = std::make_shared<std::recursive_timed_mutex>();
        return mutex;
    };
    bind("pthread_mutex_init", [&, get_mutex](auto &c) {
        m.writable(c.x(0), 40);
        get_mutex(c.x(0));
        return 0;
    });
    auto lock_mutex = [&r](const auto &mutex) {
        while (!mutex->try_lock_for(std::chrono::milliseconds(100))) {
            if (r.stopping)
                throw ThreadExit{0};
            r.check_threads();
        }
    };
    bind("pthread_mutex_lock", [get_mutex, lock_mutex](auto &c) {
        lock_mutex(get_mutex(c.x(0)));
        return 0;
    });
    bind("pthread_mutex_trylock", [get_mutex](auto &c) { return get_mutex(c.x(0))->try_lock() ? 0 : 16; });
    bind("pthread_mutex_unlock", [get_mutex](auto &c) {
        get_mutex(c.x(0))->unlock();
        return 0;
    });
    bind("pthread_mutex_destroy", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        r.mutexes.erase(c.x(0));
        return 0;
    });
    for (const auto *name :
         {"pthread_mutexattr_init", "pthread_mutexattr_destroy", "pthread_mutexattr_settype",
          "pthread_mutexattr_setpshared", "pthread_attr_init", "pthread_attr_destroy",
          "pthread_attr_setstacksize", "pthread_attr_setdetachstate", "pthread_condattr_init",
          "pthread_condattr_destroy", "pthread_condattr_setclock", "pthread_setschedparam"})
        bind(name, [](auto &) { return 0; });
    auto get_condition = [&](Address address) {
        std::lock_guard lock(r.state_mutex);
        auto &condition = r.conditions[address];
        if (!condition)
            condition = std::make_shared<std::condition_variable_any>();
        return condition;
    };
    bind("pthread_cond_init", [get_condition](auto &c) {
        get_condition(c.x(0));
        return 0;
    });
    bind("pthread_cond_destroy", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        r.conditions.erase(c.x(0));
        return 0;
    });
    bind("pthread_cond_signal", [get_condition](auto &c) {
        get_condition(c.x(0))->notify_one();
        return 0;
    });
    bind("pthread_cond_broadcast", [get_condition](auto &c) {
        get_condition(c.x(0))->notify_all();
        return 0;
    });
    for (const auto *name : {"pthread_cond_wait", "pthread_cond_timedwait"})
        bind(name, [&, get_mutex, get_condition](auto &c) {
            if (r.stopping)
                throw ThreadExit{0};
            auto mutex = get_mutex(c.x(1));
            auto condition = get_condition(c.x(0));
            std::unique_lock lock(*mutex, std::adopt_lock);
            bool timeout = false;
            if (c.current_import == "pthread_cond_timedwait") {
                auto sec = m.load<std::int64_t>(c.x(2)), ns = m.load<std::int64_t>(c.x(2) + 8);
                auto duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::seconds(sec) + std::chrono::nanoseconds(ns));
                auto target = std::chrono::system_clock::time_point(duration);
                while (condition->wait_until(lock, std::min(target, std::chrono::system_clock::now() +
                                                                        std::chrono::milliseconds(100))) ==
                       std::cv_status::timeout) {
                    if (r.stopping)
                        throw ThreadExit{0};
                    r.check_threads();
                    if (std::chrono::system_clock::now() >= target) {
                        timeout = true;
                        break;
                    }
                }
            } else
                condition->wait_for(lock, std::chrono::milliseconds(100));
            r.check_threads();
            lock.release();
            return timeout ? 110 : 0;
        });
    bind("pthread_once", [&, get_mutex](auto &c) {
        auto mutex = get_mutex(c.x(0));
        std::lock_guard lock(*mutex);
        if (m.load<std::uint32_t>(c.x(0)) != 2) {
            c.call(c.x(1));
            m.store(c.x(0), std::uint32_t{2});
        }
        return 0;
    });
    bind("pthread_self", [](auto &c) { return c.tls(); });
    bind("pthread_equal", [](auto &c) { return c.x(0) == c.x(1); });
    bind("pthread_key_create", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        auto key = static_cast<unsigned>(r.key_destructors.size() + 1);
        r.key_destructors[key] = c.x(1);
        m.store(c.x(0), key);
        return 0;
    });
    bind("pthread_key_delete", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        r.thread_keys.erase(static_cast<unsigned>(c.x(0)));
        return 0;
    });
    bind("pthread_getspecific", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        return r.thread_keys[static_cast<unsigned>(c.x(0))][c.tls()];
    });
    bind("pthread_setspecific", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        r.thread_keys[static_cast<unsigned>(c.x(0))][c.tls()] = c.x(1);
        return 0;
    });
    bind("pthread_create", [&](auto &c) {
        std::lock_guard creation_lock(r.state_mutex);
        if (r.stopping)
            return 11;
        auto id = r.next_thread_id++;
        if (id >= 256)
            return 11;
        auto item = std::make_unique<Runtime::GuestThread>();
        item->cpu = std::make_unique<guest::Cpu>(m, r.traps, id);
        auto handle = item->cpu->tls();
        item->cpu->syscall = r.cpu.syscall;
        item->cpu->checkpoint = r.cpu.checkpoint;
        auto *ptr = item.get();
        auto entry = c.x(2), arg = c.x(3);
        m.store(c.x(0), handle);
        {
            std::lock_guard lock(r.state_mutex);
            r.threads.emplace(handle, std::move(item));
        }
        ptr->host = std::thread([ptr, entry, arg, &r] {
            try {
                ptr->result = ptr->cpu->call(entry, std::array{arg}, UINT64_MAX,
                                             std::chrono::milliseconds::max());
            } catch (const ThreadExit &e) {
                ptr->result = e.result;
            } catch (const std::exception &e) {
                if (!r.stopping) {
                    r.fail_thread(*ptr, e.what());
                    r.message("Guest worker failed: " + ptr->error);
                }
            }
            ptr->completed = true;
        });
        return 0;
    });
    bind("pthread_join", [&](auto &c) {
        Runtime::GuestThread *thread;
        {
            std::lock_guard lock(r.state_mutex);
            auto it = r.threads.find(c.x(0));
            if (it == r.threads.end())
                return 3;
            thread = it->second.get();
        }
        if (!thread->host.joinable())
            return 22;
        while (!thread->completed.load()) {
            if (r.stopping)
                throw ThreadExit{0};
            r.check_threads();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        thread->host.join();
        if (c.x(1))
            m.store(c.x(1), thread->result);
        if (!thread->error.empty())
            throw Error(thread->error);
        return 0;
    });
    bind("pthread_detach", [&](auto &c) {
        std::lock_guard lock(r.state_mutex);
        auto it = r.threads.find(c.x(0));
        if (it == r.threads.end())
            return 3;
        it->second->detached = true;
        return 0;
    });
    bind("pthread_exit", [](auto &c) -> std::uint64_t { throw ThreadExit{c.x(0)}; });
    bind("sched_get_priority_min", [](auto &) { return 0; });
    bind("sched_get_priority_max", [](auto &) { return 99; });
    bind("sched_yield", [](auto &) {
        std::this_thread::yield();
        return 0;
    });
    for (const auto *name : {"pthread_rwlock_init", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
                             "pthread_rwlock_unlock", "pthread_rwlock_destroy"})
        bind(name, [&, get_mutex, lock_mutex](auto &c) {
            auto name = c.current_import;
            if (name.ends_with("init")) {
                m.writable(c.x(0), 56);
                get_mutex(c.x(0));
            } else if (name.ends_with("unlock"))
                get_mutex(c.x(0))->unlock();
            else if (name.ends_with("destroy")) {
                std::lock_guard lock(r.state_mutex);
                r.mutexes.erase(c.x(0));
            } else
                lock_mutex(get_mutex(c.x(0)));
            return 0;
        });
    auto semaphores =
        std::make_shared<std::map<Address, std::shared_ptr<std::counting_semaphore<INT_MAX>>>>();
    bind("sem_init", [&, semaphores](auto &c) {
        std::lock_guard lock(r.state_mutex);
        if (c.x(1))
            return r.failure(c, 38);
        if (c.x(2) > INT_MAX)
            return r.failure(c, 22);
        m.writable(c.x(0), 16);
        (*semaphores)[c.x(0)] =
            std::make_shared<std::counting_semaphore<INT_MAX>>(static_cast<std::ptrdiff_t>(c.x(2)));
        return std::uint64_t{};
    });
    for (const auto *name : {"sem_post", "sem_wait", "sem_destroy"})
        bind(name, [&, semaphores](auto &c) {
            std::shared_ptr<std::counting_semaphore<INT_MAX>> semaphore;
            {
                std::lock_guard lock(r.state_mutex);
                auto it = semaphores->find(c.x(0));
                if (it == semaphores->end())
                    return r.failure(c, 22);
                semaphore = it->second;
                if (c.current_import == "sem_destroy") {
                    semaphores->erase(it);
                    return std::uint64_t{};
                }
            }
            if (c.current_import == "sem_post")
                semaphore->release();
            else
                while (!semaphore->try_acquire_for(std::chrono::milliseconds(100))) {
                    if (r.stopping)
                        throw ThreadExit{0};
                    r.check_threads();
                }
            return std::uint64_t{};
        });
    bind("qsort", [&](auto &c) {
        auto base = c.x(0), count = c.x(1), size = c.x(2), compare = c.x(3);
        require(!size || count <= (64 << 20) / size, "qsort input too large");
        std::vector<size_t> order(count);
        for (size_t i = 0; i < count; ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return static_cast<std::int32_t>(
                       c.call(compare, std::array{base + a * size, base + b * size})) < 0;
        });
        Bytes copy(count * size);
        for (size_t i = 0; i < count; ++i)
            m.read(base + order[i] * size, std::span(copy).subspan(i * size, size));
        m.write(base, copy);
        return 0;
    });
    bind("__emutls_get_address", [&](auto &c) {
        auto control = c.x(0), key = (control << 32) ^ c.tls();
        std::lock_guard lock(r.state_mutex);
        auto &value = r.emutls[key];
        if (!value) {
            auto size = m.load<std::uint64_t>(control), alignment = m.load<std::uint64_t>(control + 8),
                 initial = m.load<Address>(control + 24);
            value = m.allocate(size, guest::Read | guest::Write, std::max<std::uint64_t>(16, alignment));
            if (initial)
                m.write(value, m.readable(initial, size));
        }
        return value;
    });
    for (const auto *name : {"signal", "sigaction"})
        r.bind_suppressed(name, "unrelated-posix", "SIG_ERR or -1 / ENOSYS; host signal handlers remain untouched",
                          [&](auto &c) { return r.failure(c, 38); });
    r.bind_suppressed("raise", "unrelated-posix", "-1 / ENOSYS; no host signal is raised",
                      [&](auto &c) { return r.failure(c, 38); });
    for (const auto *name :
         {"socket",      "connect",     "send",        "recv",          "sendto", "recvfrom",
          "sendmmsg",    "recvmmsg",    "bind",        "listen",        "accept", "getsockopt",
          "setsockopt",  "getsockname", "getpeername", "shutdown", "accept4"})
        r.bind_suppressed(name, "online-network", "-1 / ENETUNREACH; no socket or network request",
                          [&](auto &c) { return r.failure(c, 101); });
    r.bind_suppressed("getnameinfo", "online-network", "Android EAI_NONAME (8); no DNS lookup",
                      [](auto &) { return 8; });
    r.bind_suppressed("if_nametoindex", "online-network", "0 / ENODEV; no Android network interface",
                      [&](auto &c) { r.failure(c, 19); return 0; });
    r.bind_suppressed("getaddrinfo", "online-network", "Android EAI_NONAME (8), null result; no DNS lookup", [&](auto &c) {
        if (c.x(3))
            m.store(c.x(3), Address{});
        return 8;
    });
    bind("freeaddrinfo", [](auto &) { return 0; });
    r.bind_suppressed("gethostbyname", "online-network", "null; no DNS lookup",
                      [](auto &) -> Address { return 0; });
    bind("gai_strerror",
         [&](auto &) { return m.string("Network services unavailable in offline runtime"); });
    bind("strerror", [&](auto &c) { return m.string("Android errno " + std::to_string(c.x(0))); });
    bind("strerror_r", [&](auto &c) {
        auto text = "Android errno " + std::to_string(c.x(0));
        if (c.x(2) <= text.size())
            return 34;
        m.write(c.x(1), std::as_bytes(std::span(text)));
        m.store(c.x(1) + text.size(), char{});
        return 0;
    });
    auto dl_error = m.string("Dynamic library or symbol not found");
    bind("dlsym", [&](auto &c) { return r.linker.symbol(m.string(c.x(1))); });
    bind("dlopen", [&](auto &c) -> Address {
        if (!c.x(0))
            return 1;
        auto name = m.string(c.x(0));
        for (const auto &image : r.linker.images())
            if (utf8(image->path.filename()) == name)
                return image->base;
        for (const auto *system :
             {"libc.so", "libm.so", "libdl.so", "liblog.so", "libGLESv2.so", "libGLESv1_CM.so", "libEGL.so",
              "libOpenSLES.so", "libandroid.so", "libz.so"})
            if (name == system)
                return 1;
        return 0;
    });
    bind("dlclose", [](auto &) { return 0; });
    bind("dlerror", [dl_error](auto &) { return dl_error; });
    bind("dladdr", [&](auto &c) {
        for (const auto &image : r.linker.images())
            if (c.x(0) >= image->base && c.x(0) - image->base < image->elf.image_size()) {
                m.store(c.x(1), m.string(utf8(image->path.filename())));
                m.store(c.x(1) + 8, image->base);
                m.store(c.x(1) + 16, Address{});
                m.store(c.x(1) + 24, Address{});
                return 1;
            }
        return 0;
    });
    for (auto [alias, target, count] :
         std::vector<std::tuple<std::string, std::string, size_t>>{{"strftime_l", "strftime", 4},
                                                                   {"strtoll_l", "strtoll", 3},
                                                                   {"strtoull_l", "strtoull", 3},
                                                                   {"ftello", "ftell", 1},
                                                                   {"getc", "fgetc", 1}})
        bind(alias, [&r, target, count](auto &c) {
            std::vector<std::uint64_t> args;
            for (size_t i = 0; i < count; ++i)
                args.push_back(c.argument(i));
            return c.call(r.linker.symbol(target), args);
        });
    struct Directory {
        std::vector<std::pair<std::string, bool>> names;
        size_t index{};
        Address entry{};
    };
    auto directories = std::make_shared<std::map<Address, Directory>>();
    bind("opendir", [&, directories](auto &c) -> Address {
        fs::path path;
        try {
            path = r.path(m.string(c.x(0)));
        } catch (const Error &) {
            r.failure(c, 13);
            return 0;
        }
        std::error_code error;
        if (!fs::is_directory(path, error)) {
            r.failure(c, 2);
            return 0;
        }
        Directory dir;
        dir.entry = m.allocate(280);
        dir.names = {{".", true}, {"..", true}};
        for (const auto &item : fs::directory_iterator(path))
            dir.names.emplace_back(utf8(item.path().filename()), item.is_directory());
        auto handle = m.allocate(8);
        directories->emplace(handle, std::move(dir));
        return handle;
    });
    bind("readdir", [&, directories](auto &c) -> Address {
        auto it = directories->find(c.x(0));
        if (it == directories->end()) {
            r.failure(c, 9);
            return 0;
        }
        auto &dir = it->second;
        if (dir.index >= dir.names.size())
            return 0;
        auto [name, is_dir] = dir.names[dir.index++];
        require(name.size() < 256, "Directory entry name exceeds Android NAME_MAX");
        auto bytes = m.writable(dir.entry, 280);
        std::fill(bytes.begin(), bytes.end(), std::byte{});
        m.store(dir.entry, static_cast<std::uint64_t>(dir.index));
        m.store(dir.entry + 8, static_cast<std::int64_t>(dir.index));
        m.store(dir.entry + 16, std::uint16_t{280});
        m.store(dir.entry + 18, std::uint8_t(is_dir ? 4 : 8));
        m.write(dir.entry + 19, std::as_bytes(std::span(name)));
        return dir.entry;
    });
    bind("closedir", [&, directories](auto &c) {
        auto it = directories->find(c.x(0));
        if (it == directories->end())
            return r.failure(c, 9);
        m.release(it->second.entry);
        m.release(it->first);
        directories->erase(it);
        return std::uint64_t{};
    });
    bind("fdopendir", [&](auto &c) -> Address {
        r.failure(c, 20);
        return 0;
    });
    bind("isatty", [&](auto &c) {
        r.failure(c, 25);
        return 0;
    });
    bind("setbuf", [&](auto &c) {
        auto fp = r.file(r.descriptor(c.x(0)));
        if (fp)
            std::setvbuf(fp, nullptr, c.x(1) ? _IOFBF : _IONBF, BUFSIZ);
        return 0;
    });
    bind("android_set_abort_message", [&](auto &c) {
        r.message("Android abort message: " + m.string(c.x(0)));
        return 0;
    });
    bind("pathconf", [](auto &c) -> std::uint64_t {
        if (c.x(1) == 3)
            return 255;
        if (c.x(1) == 4)
            return 4096;
        return UINT64_MAX;
    });
    bind("statvfs", [&](auto &c) -> std::uint64_t {
        auto path = r.path(m.string(c.x(0)));
        std::error_code error;
        auto space = fs::space(path, error);
        if (error)
            return r.failure(c, 2);
        auto b = m.writable(c.x(1), 112);
        std::fill(b.begin(), b.end(), std::byte{});
        m.store(c.x(1), std::uint64_t{4096});
        m.store(c.x(1) + 8, std::uint64_t{4096});
        m.store(c.x(1) + 16, space.capacity / 4096);
        m.store(c.x(1) + 24, space.free / 4096);
        m.store(c.x(1) + 32, space.available / 4096);
        m.store(c.x(1) + 80, std::uint64_t{255});
        return 0;
    });
    bind("__FD_SET_chk", [&](auto &c) {
        require(c.x(0) < c.x(2) * 8, "FD_SET overflow");
        auto p = c.x(1) + (c.x(0) / 64) * 8;
        m.store(p, m.load<std::uint64_t>(p) | (1ull << (c.x(0) % 64)));
        return 0;
    });
    bind("__FD_ISSET_chk", [&](auto &c) {
        require(c.x(0) < c.x(2) * 8, "FD_ISSET overflow");
        return (m.load<std::uint64_t>(c.x(1) + (c.x(0) / 64) * 8) >> (c.x(0) % 64)) & 1;
    });
    bind("dl_iterate_phdr", [&](auto &c) {
        auto callback = c.x(0), context = c.x(1);
        for (const auto &image : r.linker.images()) {
            auto info = m.allocate(64);
            BinaryView elf(image->elf.bytes());
            m.store(info, image->base);
            m.store(info + 8, m.string(utf8(image->path.filename())));
            m.store(info + 16, image->base + elf.get<std::uint64_t>(32));
            m.store(info + 24, elf.get<std::uint16_t>(56));
            auto result = c.call(callback, std::array{info, std::uint64_t{64}, context});
            m.release(m.load<Address>(info + 8));
            m.release(info);
            if (result)
                return result;
        }
        return std::uint64_t{};
    });
    for (const auto *name : {"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp"})
        bind(name, [&](auto &c) {
            auto saved = m.writable(c.x(0), 176);
            std::array<std::uint64_t, 13> registers{};
            for (size_t i = 19; i <= 30; ++i)
                registers[i - 19] = c.x(i);
            registers[12] = c.sp();
            std::memcpy(saved.data(), registers.data(), sizeof(registers));
            for (size_t i = 8; i < 16; ++i) {
                auto value = std::bit_cast<std::uint64_t>(c.f64(i));
                std::memcpy(saved.data() + 112 + (i - 8) * 8, &value, 8);
            }
            return 0;
        });
    for (const auto *name : {"longjmp", "_longjmp", "siglongjmp"})
        r.bind_void(name, [&](auto &c) {
            auto p = c.x(0), value = c.x(1);
            auto saved = m.readable(p, 176);
            std::array<std::uint64_t, 22> registers{};
            std::memcpy(registers.data(), saved.data(), 176);
            for (size_t i = 19; i <= 30; ++i)
                c.x(i, registers[i - 19]);
            c.sp(registers[12]);
            for (size_t i = 8; i < 16; ++i)
                c.f64(i, std::bit_cast<double>(registers[14 + i - 8]));
            c.x(0, value ? value : 1);
            c.pc(c.x(30));
        });

    struct ZlibStreams {
        std::map<Address, std::unique_ptr<mz_stream>> streams;
        std::map<Address, std::pair<Bytes, size_t>> gzip;
    };
    auto zlib = std::make_shared<ZlibStreams>();
    auto sync_in = [&](Address a, mz_stream &z) {
        z.avail_in = m.load<std::uint32_t>(a + 8);
        z.avail_out = m.load<std::uint32_t>(a + 32);
        auto in = m.load<Address>(a), out = m.load<Address>(a + 24);
        z.next_in =
            in ? reinterpret_cast<const unsigned char *>(m.readable(in, z.avail_in).data()) : nullptr;
        z.next_out = out ? reinterpret_cast<unsigned char *>(m.writable(out, z.avail_out).data()) : nullptr;
    };
    bind("zlibVersion", [&](auto &) { return m.string("1.2.13"); });
    bind("crc32", [&](auto &c) {
        auto b = c.x(1) ? m.readable(c.x(1), c.x(2)) : std::span<const std::byte>{};
        return mz_crc32(static_cast<mz_ulong>(c.x(0)), reinterpret_cast<const unsigned char *>(b.data()),
                        b.size());
    });
    bind("adler32", [&](auto &c) {
        auto b = c.x(1) ? m.readable(c.x(1), c.x(2)) : std::span<const std::byte>{};
        return mz_adler32(static_cast<mz_ulong>(c.x(0)), reinterpret_cast<const unsigned char *>(b.data()),
                          b.size());
    });
    for (const auto *name : {"inflateInit_", "inflateInit2_", "deflateInit_", "deflateInit2_"})
        bind(name, [&, zlib](auto &c) {
            auto name = c.current_import;
            auto stream = std::make_unique<mz_stream>();
            auto address = c.x(0);
            if (m.load<Address>(address + 64) || m.load<Address>(address + 72))
                return -2;
            int result;
            if (name == "inflateInit_")
                result = mz_inflateInit(stream.get());
            else if (name == "inflateInit2_")
                result = mz_inflateInit2(stream.get(), static_cast<int>(c.x(1)));
            else if (name == "deflateInit_")
                result = mz_deflateInit(stream.get(), static_cast<int>(c.x(1)));
            else
                result = mz_deflateInit2(stream.get(), static_cast<int>(c.x(1)), static_cast<int>(c.x(2)),
                                         static_cast<int>(c.x(3)), static_cast<int>(c.x(4)),
                                         static_cast<int>(c.x(5)));
            if (result == MZ_OK) {
                zlib->streams[address] = std::move(stream);
                m.store(address + 16, std::uint64_t{});
                m.store(address + 40, std::uint64_t{});
                m.store(address + 56, address);
            }
            return result;
        });
    for (const auto *name : {"inflate", "deflate"})
        bind(name, [&, zlib, sync_in](auto &c) {
            auto address = c.x(0);
            auto it = zlib->streams.find(address);
            if (it == zlib->streams.end())
                return -2;
            auto &z = *it->second;
            sync_in(address, z);
            auto input = z.avail_in, output = z.avail_out;
            auto result = c.current_import == "inflate" ? mz_inflate(&z, static_cast<int>(c.x(1)))
                                                        : mz_deflate(&z, static_cast<int>(c.x(1)));
            auto used_in = input - z.avail_in, used_out = output - z.avail_out;
            m.store(address, m.load<Address>(address) + used_in);
            m.store(address + 8, z.avail_in);
            m.store(address + 16, m.load<std::uint64_t>(address + 16) + used_in);
            m.store(address + 24, m.load<Address>(address + 24) + used_out);
            m.store(address + 32, z.avail_out);
            m.store(address + 40, m.load<std::uint64_t>(address + 40) + used_out);
            m.store(address + 96, static_cast<std::uint64_t>(z.adler));
            return result;
        });
    for (const auto *name : {"inflateEnd", "deflateEnd", "inflateReset", "deflateReset"})
        bind(name, [&, zlib](auto &c) {
            auto it = zlib->streams.find(c.x(0));
            if (it == zlib->streams.end())
                return -2;
            int result = c.current_import == "inflateEnd"     ? mz_inflateEnd(it->second.get())
                         : c.current_import == "deflateEnd"   ? mz_deflateEnd(it->second.get())
                         : c.current_import == "inflateReset" ? mz_inflateReset(it->second.get())
                                                              : mz_deflateReset(it->second.get());
            if (c.current_import.ends_with("End")) {
                zlib->streams.erase(it);
                m.store(c.x(0) + 56, Address{});
            } else {
                m.store(c.x(0) + 16, std::uint64_t{});
                m.store(c.x(0) + 40, std::uint64_t{});
            }
            return result;
        });
    bind("gzopen", [&, zlib](auto &c) -> Address {
        auto mode = m.string(c.x(1));
        if (mode.find('w') != mode.npos) {
            r.failure(c, 38);
            return 0;
        }
        try {
            auto data = read_bytes(r.path(m.string(c.x(0))), 256 << 20);
            Bytes output;
            if (data.size() >= 18 && data[0] == std::byte{0x1f} && data[1] == std::byte{0x8b}) {
                BinaryView v(data);
                require(v.get<std::uint8_t>(2) == 8, "Unsupported gzip method");
                auto flags = v.get<std::uint8_t>(3);
                size_t offset = 10;
                if (flags & 4) {
                    auto n = v.get<std::uint16_t>(offset);
                    offset += 2 + n;
                }
                for (unsigned flag : {8u, 16u})
                    if (flags & flag) {
                        auto value = v.cstring(offset);
                        offset += value.size() + 1;
                    }
                if (flags & 2)
                    offset += 2;
                require(offset <= data.size() - 8, "Invalid gzip header");
                auto size = v.get<std::uint32_t>(data.size() - 4);
                require(size <= 256 << 20, "Gzip output too large");
                output.resize(size);
                mz_stream z{};
                require(mz_inflateInit2(&z, -15) == MZ_OK, "Gzip inflate init failed");
                z.next_in = reinterpret_cast<const unsigned char *>(data.data() + offset);
                z.avail_in = static_cast<unsigned>(data.size() - offset - 8);
                z.next_out = reinterpret_cast<unsigned char *>(output.data());
                z.avail_out = size;
                auto result = mz_inflate(&z, MZ_FINISH);
                mz_inflateEnd(&z);
                require(result == MZ_STREAM_END && z.total_out == size, "Gzip inflate failed");
                require(mz_crc32(0, reinterpret_cast<const unsigned char *>(output.data()),
                                 output.size()) == v.get<std::uint32_t>(data.size() - 8),
                        "Gzip CRC mismatch");
            } else
                output = std::move(data);
            auto handle = m.allocate(8);
            zlib->gzip.emplace(handle, std::make_pair(std::move(output), size_t{}));
            return handle;
        } catch (const fs::filesystem_error &) {
            r.failure(c, 2);
            return 0;
        }
    });
    bind("gzread", [&, zlib](auto &c) {
        auto it = zlib->gzip.find(c.x(0));
        if (it == zlib->gzip.end())
            return -1;
        auto &[data, offset] = it->second;
        auto size = std::min<size_t>(c.x(2), data.size() - offset);
        m.write(c.x(1), std::span(data).subspan(offset, size));
        offset += size;
        return static_cast<int>(size);
    });
    bind("gzclose", [&, zlib](auto &c) {
        auto count = zlib->gzip.erase(c.x(0));
        if (count)
            m.release(c.x(0));
        return count ? 0 : -1;
    });
    r.cpu.syscall = [&](auto &c) {
        {
            std::lock_guard lock(r.state_mutex);
            ++r.syscall_counts[c.x(8)];
        }
        if (c.x(8) == 113) {
            auto clock = c.x(0), out = c.x(1);
            auto result = c.call(r.linker.symbol("clock_gettime"), std::array{clock, out});
            c.x(0, result == UINT64_MAX ? -static_cast<std::int64_t>(m.load<int>(r.errno_address(c)))
                                        : result);
            return;
        }
        if (c.x(8) == 172) {
            c.x(0, GetCurrentProcessId());
            return;
        }
        if (c.x(8) == 178) {
            c.x(0, c.tls());
            return;
        }
        c.x(0, static_cast<std::uint64_t>(-38ll));
    };
    bind("syscall", [&](auto &c) {
        auto number = c.x(0);
        for (size_t i = 0; i < 6; ++i)
            c.x(i, c.argument(i + 1));
        c.x(8, number);
        r.cpu.syscall(c);
        if (c.x(0) >= static_cast<std::uint64_t>(-4095ll))
            return r.failure(c, static_cast<int>(-static_cast<std::int64_t>(c.x(0))));
        return c.x(0);
    });
}
} // namespace acp::android

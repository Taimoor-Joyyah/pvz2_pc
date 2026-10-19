#include <acp/android.hpp>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace acp::android {
namespace {
using boost::multiprecision::cpp_int;
bool space(char32_t c) { return c <= 0xffff && std::iswspace(static_cast<wint_t>(c)); }
std::u32string wide(guest::Memory &m, Address p) {
    std::u32string out;
    for (size_t i = 0; i < 1 << 20; ++i) {
        auto c = m.load<std::uint32_t>(p + i * 4);
        if (!c) return out;
        out.push_back(c);
    }
    throw Error("Unterminated Android wide string");
}
std::u32string bytes_as_characters(std::string_view value) {
    std::u32string out;
    for (unsigned char c : value) out.push_back(c);
    return out;
}
std::string ascii(std::u32string_view value) {
    std::string out;
    for (auto c : value) out.push_back(c < 128 ? static_cast<char>(c) : space(c) ? ' ' : '\x7f');
    return out;
}
std::optional<std::string> encode(char32_t c) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return {};
    if (c < 128) return std::string(1, static_cast<char>(c));
    std::string out;
    if (c < 0x800) out += static_cast<char>(0xc0 | (c >> 6));
    else {
        if (c < 0x10000) out += static_cast<char>(0xe0 | (c >> 12));
        else { out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63)); }
        out += static_cast<char>(0x80 | ((c >> 6) & 63));
    }
    out += static_cast<char>(0x80 | (c & 63));
    return out;
}
struct Quad {
    std::array<std::uint64_t, 2> bits{};
    size_t used{};
    bool range{};
};
int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 16;
}
cpp_int power(unsigned base, unsigned exponent) {
    cpp_int result = 1, term = base;
    while (exponent) {
        if (exponent & 1) result *= term;
        exponent >>= 1;
        if (exponent) term *= term;
    }
    return result;
}
// AArch64 long double is IEEE binary128, whereas MSVC long double is binary64.
// Convert an exact integer ratio with one final round-to-nearest/ties-to-even step.
Quad parse_quad(std::string_view text) {
    Quad result;
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    bool negative = i < text.size() && text[i] == '-';
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    auto sign = negative ? 1ull << 63 : 0;
    auto starts = [&](std::string_view value) {
        if (text.size() - i < value.size()) return false;
        for (size_t n = 0; n < value.size(); ++n)
            if (std::tolower(static_cast<unsigned char>(text[i + n])) != value[n]) return false;
        return true;
    };
    if (starts("inf")) {
        i += 3;
        if (starts("inity")) i += 5;
        result.used = i; result.bits[1] = sign | 0x7fff000000000000ull; return result;
    }
    if (starts("nan")) {
        i += 3;
        if (i < text.size() && text[i] == '(') {
            auto end = i + 1;
            while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
            if (end < text.size() && text[end] == ')') i = end + 1;
        }
        result.used = i; result.bits[1] = sign | 0x7fff800000000000ull; return result;
    }
    bool hex = i + 2 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
               (digit(text[i + 2]) < 16 || (text[i + 2] == '.' && i + 3 < text.size() && digit(text[i + 3]) < 16));
    if (hex) i += 2;
    unsigned radix = hex ? 16 : 10;
    std::string digits;
    size_t fractional = 0;
    bool point = false;
    while (i < text.size()) {
        if (text[i] == '.' && !point) { point = true; ++i; continue; }
        if (static_cast<unsigned>(digit(text[i])) >= radix) break;
        digits += text[i++];
        if (point) ++fractional;
    }
    if (digits.empty()) return result;
    std::int64_t exponent = 0;
    if (i < text.size() && std::tolower(static_cast<unsigned char>(text[i])) == (hex ? 'p' : 'e')) {
        auto end = i + 1;
        bool minus = end < text.size() && text[end] == '-';
        if (end < text.size() && (text[end] == '+' || text[end] == '-')) ++end;
        auto first = end;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
            exponent = std::min<std::int64_t>(10'000'000, exponent * 10 + text[end++] - '0');
        }
        if (end != first) { i = end; if (minus) exponent = -exponent; }
        else exponent = 0;
    }
    result.used = i;
    result.bits[1] = sign;
    auto first = digits.find_first_not_of('0');
    if (first == digits.npos) return result;
    digits.erase(0, first);
    exponent -= static_cast<std::int64_t>(fractional) * (hex ? 4 : 1);
    auto magnitude = exponent + static_cast<std::int64_t>(digits.size()) * (hex ? 4 : 1);
    if (magnitude > (hex ? 16400 : 4934)) {
        result.range = true; result.bits[1] |= 0x7fff000000000000ull; return result;
    }
    if (magnitude < (hex ? -16500 : -4967)) { result.range = true; return result; }
    // 25k decimal digits exceed the exact decimal expansion of every binary128 midpoint.
    // Keep a sticky tail to distinguish exact ties from values infinitesimally above a tie.
    auto limit = hex ? size_t{5000} : size_t{25000};
    bool sticky = false;
    if (digits.size() > limit) {
        sticky = digits.find_first_not_of('0', limit) != digits.npos;
        exponent += static_cast<std::int64_t>(digits.size() - limit) * (hex ? 4 : 1);
        digits.resize(limit);
    }
    cpp_int numerator = 0, denominator = 1;
    for (char c : digits) { numerator *= radix; numerator += digit(c); }
    if (hex) {
        if (exponent >= 0) numerator <<= static_cast<unsigned>(exponent);
        else denominator <<= static_cast<unsigned>(-exponent);
    } else if (exponent >= 0) numerator *= power(10, static_cast<unsigned>(exponent));
    else denominator = power(10, static_cast<unsigned>(-exponent));
    int binary_exponent = static_cast<int>(boost::multiprecision::msb(numerator)) -
                          static_cast<int>(boost::multiprecision::msb(denominator));
    if (binary_exponent >= 0 ? numerator < (denominator << binary_exponent)
                             : (numerator << -binary_exponent) < denominator) --binary_exponent;
    if (binary_exponent > 16383) { result.range = true; result.bits[1] |= 0x7fff000000000000ull; return result; }
    int shift = binary_exponent < -16382 ? 16494 : 112 - binary_exponent;
    if (shift >= 0) numerator <<= shift;
    else denominator <<= -shift;
    cpp_int significand = numerator / denominator, remainder = numerator % denominator;
    cpp_int twice = remainder << 1;
    if (twice > denominator || (twice == denominator && (sticky || bool(significand & 1)))) ++significand;
    unsigned encoded_exponent = binary_exponent < -16382 ? 0 : binary_exponent + 16383;
    if (significand >= (cpp_int(1) << 113)) { significand >>= 1; ++encoded_exponent; }
    if (!encoded_exponent && significand >= (cpp_int(1) << 112)) encoded_exponent = 1;
    if (encoded_exponent >= 0x7fff) { result.range = true; result.bits[1] |= 0x7fff000000000000ull; return result; }
    if (binary_exponent < -16382) result.range = true;
    cpp_int fraction = significand & ((cpp_int(1) << 112) - 1);
    result.bits[0] = (fraction & ((cpp_int(1) << 64) - 1)).convert_to<std::uint64_t>();
    result.bits[1] = sign | (std::uint64_t(encoded_exponent) << 48) | (fraction >> 64).convert_to<std::uint64_t>();
    return result;
}
struct ScanArgs {
    guest::Cpu &cpu;
    size_t next = 2;
    Address va{};
    Address get() {
        if (!va) return cpu.argument(next++);
        auto &m = cpu.memory();
        auto offset = m.load<std::int32_t>(va + 24);
        if (offset < 0) {
            m.store(va + 24, offset + 8);
            return m.load<Address>(m.load<Address>(va + 8) + static_cast<std::int64_t>(offset));
        }
        auto stack = m.load<Address>(va);
        m.store(va, stack + 8);
        return m.load<Address>(stack);
    }
};
struct Input {
    Runtime &r;
    std::u32string text;
    int fd = -1;
    size_t offset{}, consumed{};
    std::deque<char32_t> pending;
    bool end{};
    ~Input() {
        if (fd < 0 || pending.empty()) return;
        Bytes bytes;
        for (auto value : pending) bytes.push_back(static_cast<std::byte>(value));
        r.unread_stream(fd, bytes);
    }
    int peek(size_t n = 0) {
        while (pending.size() <= n && !end) {
            if (fd < 0) {
                if (offset == text.size()) { end = true; break; }
                pending.push_back(text[offset++]);
            } else {
                std::byte value;
                if (!r.read_stream(fd, std::span(&value, 1))) { end = true; break; }
                pending.push_back(std::to_integer<unsigned char>(value));
            }
        }
        return pending.size() > n ? static_cast<int>(pending[n]) : -1;
    }
    int get() {
        auto value = peek();
        if (value >= 0) { pending.pop_front(); ++consumed; }
        return value;
    }
};
std::optional<char32_t> character(Input &input, bool utf8) {
    int first = input.peek();
    if (first < 0) return {};
    if (!utf8 || first < 128) { input.get(); return static_cast<char32_t>(first); }
    size_t size = (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
    if (!size) return {};
    char32_t value = first & ((1 << (7 - size)) - 1);
    for (size_t i = 1; i < size; ++i) {
        auto next = input.peek(i);
        if (next < 0 || (next & 0xc0) != 0x80) return {};
        value = (value << 6) | (next & 63);
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
        (size == 2 && value < 0x80) || (size == 3 && value < 0x800) || (size == 4 && value < 0x10000)) return {};
    for (size_t i = 0; i < size; ++i) input.get();
    return value;
}
void store_integer(guest::Memory &m, Address p, std::string_view length, std::uint64_t value) {
    if (length == "hh") m.store(p, static_cast<std::uint8_t>(value));
    else if (length == "h") m.store(p, static_cast<std::uint16_t>(value));
    else if (length == "l" || length == "ll" || length == "j" || length == "z" || length == "t") m.store(p, value);
    else m.store(p, static_cast<std::uint32_t>(value));
}
int scan(Runtime &r, guest::Cpu &cpu, Input &input, std::u32string_view format, ScanArgs args, bool wide_input) {
    int assigned = 0;
    bool converted = false;
    auto fail = [&](bool input_failure) { return input_failure && !converted ? -1 : assigned; };
    auto &m = r.memory;
    for (size_t f = 0; f < format.size();) {
        auto code = format[f++];
        if (space(code)) {
            while (input.peek() >= 0 && space(input.peek())) input.get();
            continue;
        }
        if (code != '%') {
            if (input.peek() != static_cast<int>(code)) return fail(input.peek() < 0);
            input.get(); continue;
        }
        if (f < format.size() && format[f] == '%') {
            ++f;
            if (input.peek() != '%') return fail(input.peek() < 0);
            input.get(); continue;
        }
        bool suppress = f < format.size() && format[f] == '*';
        if (suppress) ++f;
        size_t width = 0;
        while (f < format.size() && format[f] >= '0' && format[f] <= '9')
            width = std::min<size_t>(1 << 20, width * 10 + format[f++] - '0');
        std::string length;
        if (f < format.size() && std::u32string_view(U"hljztL").find(format[f]) != std::u32string_view::npos) {
            length += static_cast<char>(format[f++]);
            if (f < format.size() && (length == "h" || length == "l") && format[f] == static_cast<unsigned char>(length[0])) length += static_cast<char>(format[f++]);
        }
        if (f == format.size()) { r.failure(cpu, 22); return assigned; }
        code = format[f++];
        if (code == 'n') {
            if (!suppress) store_integer(m, args.get(), length, input.consumed);
            converted = true;
            continue;
        }
        if (code != 'c' && code != '[')
            while (input.peek() >= 0 && space(input.peek())) input.get();
        if (input.peek() < 0) return fail(true);
        if (code == 'c' || code == 's' || code == '[') {
            std::vector<std::pair<char32_t, char32_t>> ranges;
            bool inverted = false;
            if (code == '[') {
                if (f < format.size() && format[f] == '^') { inverted = true; ++f; }
                if (f < format.size() && format[f] == ']') { ranges.emplace_back(']', ']'); ++f; }
                while (f < format.size() && format[f] != ']') {
                    auto begin = format[f++], end = begin;
                    if (f + 1 < format.size() && format[f] == '-' && format[f + 1] != ']') { ++f; end = format[f++]; }
                    ranges.emplace_back(begin, end);
                }
                if (f == format.size()) { r.failure(cpu, 22); return assigned; }
                ++f;
            }
            if (!width) width = code == 'c' ? 1 : 1 << 20;
            std::u32string characters;
            bool wide_output = length == "l";
            while (characters.size() < width && input.peek() >= 0) {
                auto next = static_cast<char32_t>(input.peek());
                if (code == 's' && space(next)) break;
                if (code == '[') {
                    bool inside = std::ranges::any_of(ranges, [next](auto range) { return next >= range.first && next <= range.second; });
                    if (inside == inverted) break;
                }
                auto value = character(input, !wide_input && wide_output);
                if (!value) { r.failure(cpu, 84); return fail(true); }
                characters.push_back(*value);
            }
            if (characters.empty()) return fail(input.peek() < 0);
            if (!suppress) {
                auto destination = args.get();
                if (wide_output) {
                    for (size_t n = 0; n < characters.size(); ++n) m.store(destination + n * 4, std::uint32_t(characters[n]));
                    if (code != 'c') m.store(destination + characters.size() * 4, std::uint32_t{});
                } else {
                    std::string bytes;
                    for (auto value : characters) {
                        if (wide_input) {
                            auto encoded = encode(value);
                            if (!encoded) { r.failure(cpu, 84); return fail(true); }
                            bytes += *encoded;
                        } else bytes += static_cast<char>(value);
                    }
                    m.write(destination, std::as_bytes(std::span(bytes)));
                    if (code != 'c') m.store(destination + bytes.size(), char{});
                }
                ++assigned;
            }
            converted = true;
            continue;
        }
        bool floating = std::u32string_view(U"aAeEfFgG").find(code) != std::u32string_view::npos;
        bool integer = std::u32string_view(U"diouxXp").find(code) != std::u32string_view::npos;
        if (!floating && !integer) { r.failure(cpu, 22); return assigned; }
        if (!width) width = 1 << 20;
        std::string token;
        for (size_t n = 0; n < width; ++n) {
            auto ch = input.peek(n);
            if (ch < 0 || ch > 127 || !(std::isalnum(ch) || ch == '+' || ch == '-' || ch == '.' ||
                                       (floating && (ch == '(' || ch == ')' || ch == '_')))) break;
            token += static_cast<char>(ch);
        }
        char *end = nullptr;
        size_t used = 0;
        std::uint64_t number = 0;
        double real = 0;
        float single = 0;
        Quad quad;
        errno = 0;
        if (floating) {
            if (length == "L") { quad = parse_quad(token); used = quad.used; if (quad.range) errno = ERANGE; }
            else if (length == "l") { real = std::strtod(token.c_str(), &end); used = end - token.c_str(); }
            else { single = std::strtof(token.c_str(), &end); used = end - token.c_str(); }
            // scanf consumes an incomplete exponent as an input item, then reports a match failure.
            if (used && used < token.size() && (token[used] == 'e' || token[used] == 'E' || token[used] == 'p' || token[used] == 'P')) {
                ++used;
                if (used < token.size() && (token[used] == '+' || token[used] == '-')) ++used;
                for (size_t n = 0; n < used; ++n) input.get();
                return fail(false);
            }
        } else {
            int base = code == 'i' ? 0 : code == 'o' ? 8 : code == 'x' || code == 'X' || code == 'p' ? 16 : 10;
            number = code == 'd' || code == 'i' ? static_cast<std::uint64_t>(std::strtoll(token.c_str(), &end, base))
                                              : std::strtoull(token.c_str(), &end, base);
            used = end - token.c_str();
        }
        if (errno == ERANGE) r.failure(cpu, 34);
        if (!used) return fail(false);
        for (size_t n = 0; n < used; ++n) input.get();
        if (!suppress) {
            auto p = args.get();
            if (!floating) store_integer(m, p, code == 'p' ? "l" : length, number);
            else if (length == "L") m.store(p, quad.bits);
            else if (length == "l") m.store(p, real);
            else m.store(p, single);
            ++assigned;
        }
        converted = true;
    }
    return assigned;
}
} // namespace

std::string format_binary128(std::array<std::uint64_t, 2> bits, std::string_view spec, char conversion) {
    bool negative = (bits[1] >> 63) != 0, upper = conversion >= 'A' && conversion <= 'Z';
    char kind = static_cast<char>(std::tolower(static_cast<unsigned char>(conversion)));
    auto exponent = static_cast<unsigned>((bits[1] >> 48) & 0x7fff);
    cpp_int fraction = cpp_int(bits[1] & 0x0000ffffffffffffull) << 64;
    fraction |= bits[0];
    int precision = 6, width = 0;
    bool explicit_precision = false;
    for (size_t i = 1; i < spec.size();) {
        if (spec[i] == '.') {
            explicit_precision = true; precision = 0; ++i;
            while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) precision = precision * 10 + spec[i++] - '0';
        } else if (spec[i] >= '1' && spec[i] <= '9') {
            while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) width = width * 10 + spec[i++] - '0';
        } else ++i;
    }
    std::string text;
    if (exponent == 0x7fff) text = fraction == 0 ? "inf" : "nan";
    else if (kind == 'a') {
        auto digits = explicit_precision ? precision : 28;
        cpp_int significand = fraction;
        if (exponent) significand |= cpp_int(1) << 112;
        int binary_exponent = exponent ? static_cast<int>(exponent) - 16383 : fraction == 0 ? 0 : -16382;
        if (digits < 28) {
            int shift = 112 - digits * 4;
            cpp_int rounded = significand >> shift;
            cpp_int remainder = significand & ((cpp_int(1) << shift) - 1), half = cpp_int(1) << (shift - 1);
            if (remainder > half || (remainder == half && bool(rounded & 1))) ++rounded;
            significand = rounded << shift;
            if (significand >= (cpp_int(1) << 113)) { significand >>= 1; ++binary_exponent; }
        }
        auto leading = (significand >> 112).convert_to<unsigned>();
        cpp_int tail = significand & ((cpp_int(1) << 112) - 1);
        std::string hex;
        for (int i = 27; i >= 0; --i) hex += "0123456789abcdef"[((tail >> (i * 4)) & 15).convert_to<unsigned>()];
        if (digits < 28) hex.resize(digits);
        else if (digits > 28) hex.append(digits - 28, '0');
        if (!explicit_precision) while (!hex.empty() && hex.back() == '0') hex.pop_back();
        text = "0x" + std::to_string(leading);
        if (!hex.empty() || spec.find('#') != spec.npos) text += '.';
        text += hex + 'p' + (binary_exponent >= 0 ? "+" : "") + std::to_string(binary_exponent);
    } else {
        using Float = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<
            113, boost::multiprecision::digit_base_2, void, std::int32_t, -32768, 32768>>;
        cpp_int significand = fraction;
        if (exponent) significand |= cpp_int(1) << 112;
        Float value(significand);
        value = boost::multiprecision::ldexp(value, (exponent ? static_cast<int>(exponent) - 16383 : -16382) - 112);
        std::ostringstream output;
        if (kind == 'f') output << std::fixed;
        if (kind == 'e') output << std::scientific;
        if (spec.find('#') != spec.npos) output << std::showpoint;
        output << std::setprecision(kind == 'g' && !precision ? 1 : precision) << value;
        text = output.str();
    }
    if (upper) std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    std::string sign = negative ? "-" : spec.find('+') != spec.npos ? "+" : spec.find(' ') != spec.npos ? " " : "";
    text = sign + text;
    if (width > static_cast<int>(text.size())) {
        auto padding = static_cast<size_t>(width) - text.size();
        if (spec.find('-') != spec.npos) text.append(padding, ' ');
        else if (spec.find('0') != spec.npos && exponent != 0x7fff) text.insert(sign.size() + (kind == 'a' ? 2 : 0), padding, '0');
        else text.insert(0, padding, ' ');
    }
    return text;
}

void install_bionic_scan(Runtime &r) {
    auto &m = r.memory;
    for (const auto *name : {"strtold", "strtold_l", "wcstold"})
        r.bind(name, [&r, &m](auto &c) -> Address {
            bool wide_input = c.current_import == "wcstold";
            auto text = wide_input ? ascii(wide(m, c.x(0))) : m.string(c.x(0));
            auto value = parse_quad(text);
            if (c.x(1)) m.store(c.x(1), c.x(0) + value.used * (wide_input ? 4 : 1));
            if (value.range) r.failure(c, 34);
            c.v128(0, value.bits);
            return 0;
        });
    for (const auto *name : {"wcstod", "wcstof", "wcstol", "wcstoll", "wcstoul", "wcstoull"})
        r.bind(name, [&r, &m](auto &c) -> Address {
            auto text = ascii(wide(m, c.x(0)));
            char *end = nullptr;
            Address value = 0;
            const auto &name = c.current_import;
            bool floating = name == "wcstod" || name == "wcstof";
            int base = static_cast<int>(c.x(2));
            if (!floating && base != 0 && (base < 2 || base > 36)) {
                if (c.x(1)) m.store(c.x(1), c.x(0));
                r.failure(c, 22); return 0;
            }
            errno = 0;
            if (name == "wcstod") c.f64(0, std::strtod(text.c_str(), &end));
            else if (name == "wcstof") c.f32(0, std::strtof(text.c_str(), &end));
            else if (name == "wcstol" || name == "wcstoll") value = std::strtoll(text.c_str(), &end, base);
            else value = std::strtoull(text.c_str(), &end, base);
            if (errno == ERANGE) r.failure(c, 34);
            if (c.x(1)) m.store(c.x(1), c.x(0) + (end - text.c_str()) * 4);
            return value;
        });
    for (const auto *name : {"sscanf", "vsscanf", "swscanf", "fscanf"})
        r.bind(name, [&r, &m](auto &c) -> Address {
            bool wide_input = c.current_import == "swscanf";
            bool stream = c.current_import == "fscanf";
            std::lock_guard lock(r.state_mutex);
            int fd = stream ? r.descriptor(c.x(0)) : -1;
            if (stream && !r.file(fd)) return r.failure(c, 9);
            Input input{r, stream ? std::u32string{} : wide_input ? wide(m, c.x(0)) : bytes_as_characters(m.string(c.x(0))), fd};
            auto format = wide_input ? wide(m, c.x(1)) : bytes_as_characters(m.string(c.x(1)));
            ScanArgs args{c, 2, c.current_import == "vsscanf" ? c.x(2) : 0};
            return static_cast<std::int64_t>(scan(r, c, input, format, args, wide_input));
        });
    r.bind("getwc", [&r](auto &c) -> Address {
        std::lock_guard lock(r.state_mutex);
        int fd = r.descriptor(c.x(0));
        if (!r.file(fd)) { r.failure(c, 9); return UINT32_MAX; }
        Input input{r, {}, fd};
        auto value = character(input, true);
        if (!value) {
            if (input.peek() >= 0) r.failure(c, 84);
            return UINT32_MAX;
        }
        return *value;
    });
    for (const auto *name : {"fputwc", "ungetwc"})
        r.bind(name, [&r](auto &c) -> Address {
            std::lock_guard lock(r.state_mutex);
            int fd = r.descriptor(c.x(1));
            auto fp = r.file(fd);
            if (!fp) { r.failure(c, 9); return UINT32_MAX; }
            if (static_cast<std::uint32_t>(c.x(0)) == UINT32_MAX) return UINT32_MAX;
            auto bytes = encode(static_cast<char32_t>(c.x(0)));
            if (!bytes) { r.failure(c, 84); return UINT32_MAX; }
            if (c.current_import == "ungetwc") r.unread_stream(fd, std::as_bytes(std::span(*bytes)));
            else if (std::fwrite(bytes->data(), 1, bytes->size(), fp) != bytes->size()) { r.failure(c, 5); return UINT32_MAX; }
            return static_cast<std::uint32_t>(c.x(0));
        });
}
} // namespace acp::android

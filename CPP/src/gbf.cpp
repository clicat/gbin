
#include "gbin/gbf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

#include <zlib.h>

namespace gbin {

GbfError::GbfError(ErrorKind k, const std::string& msg)
    : std::runtime_error(msg), kind_(k) {}

ErrorKind GbfError::kind() const noexcept { return kind_; }

// ------------------------------
// Small JSON (internal)
// ------------------------------

namespace internal {

struct JsonNumber {
    std::string raw;
    double value = 0.0;
    bool is_int = false;
};

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    std::variant<std::nullptr_t, bool, JsonNumber, std::string, Array, Object> v;

    bool is_object() const { return std::holds_alternative<Object>(v); }
    bool is_array() const { return std::holds_alternative<Array>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_number() const { return std::holds_alternative<JsonNumber>(v); }
    const Object& as_object() const { return std::get<Object>(v); }
    const Array& as_array() const { return std::get<Array>(v); }
    const std::string& as_string() const { return std::get<std::string>(v); }
    bool as_bool() const { return std::get<bool>(v); }
    const JsonNumber& as_number() const { return std::get<JsonNumber>(v); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    Json parse() {
        skip_ws();
        Json out = parse_value();
        skip_ws();
        if (pos_ != s_.size()) {
            throw GbfError(ErrorKind::HeaderJsonParse, "trailing data in JSON");
        }
        return out;
    }

private:
    std::string_view s_;
    std::size_t pos_{0};

    void skip_ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++pos_;
                continue;
            }
            break;
        }
    }

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    char get() {
        if (pos_ >= s_.size()) {
            throw GbfError(ErrorKind::HeaderJsonParse, "unexpected end of JSON");
        }
        return s_[pos_++];
    }

    static void append_utf8(std::string& out, unsigned codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    unsigned parse_hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = get();
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(10 + (c - 'a'));
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(10 + (c - 'A'));
            else throw GbfError(ErrorKind::HeaderJsonParse, "invalid \\u escape");
        }
        return v;
    }

    Json parse_string() {
        // assumes opening quote already consumed
        std::string out;
        while (true) {
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                char e = get();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned u = parse_hex4();
                        // minimal surrogate handling
                        if (u >= 0xD800 && u <= 0xDBFF) {
                            // high surrogate; expect \uXXXX
                            if (get() != '\\' || get() != 'u') {
                                throw GbfError(ErrorKind::HeaderJsonParse, "invalid surrogate pair");
                            }
                            unsigned u2 = parse_hex4();
                            if (u2 < 0xDC00 || u2 > 0xDFFF) {
                                throw GbfError(ErrorKind::HeaderJsonParse, "invalid surrogate pair");
                            }
                            unsigned codepoint = 0x10000 + (((u - 0xD800) << 10) | (u2 - 0xDC00));
                            append_utf8(out, codepoint);
                        } else {
                            append_utf8(out, u);
                        }
                        break;
                    }
                    default:
                        throw GbfError(ErrorKind::HeaderJsonParse, "invalid escape in JSON string");
                }
            } else {
                out.push_back(c);
            }
        }
        return Json{out};
    }

    Json parse_number() {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        bool has_dot = false;
        bool has_exp = false;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c >= '0' && c <= '9') { ++pos_; continue; }
            if (c == '.') { has_dot = true; ++pos_; continue; }
            if (c == 'e' || c == 'E') {
                has_exp = true; ++pos_;
                if (peek() == '+' || peek() == '-') ++pos_;
                continue;
            }
            break;
        }
        std::string raw(s_.substr(start, pos_ - start));
        if (raw.empty() || raw == "-" ) {
            throw GbfError(ErrorKind::HeaderJsonParse, "invalid number in JSON");
        }
        double v = 0.0;
        try {
            v = std::stod(raw);
        } catch (...) {
            throw GbfError(ErrorKind::HeaderJsonParse, "invalid number in JSON");
        }
        JsonNumber n;
        n.raw = std::move(raw);
        n.value = v;
        n.is_int = !(has_dot || has_exp);
        return Json{n};
    }

    Json parse_array() {
        // assumes '[' consumed
        Json::Array arr;
        skip_ws();
        if (peek() == ']') {
            get();
            return Json{arr};
        }
        while (true) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            char c = get();
            if (c == ']') break;
            if (c != ',') throw GbfError(ErrorKind::HeaderJsonParse, "expected ',' in array");
        }
        return Json{arr};
    }

    Json parse_object() {
        // assumes '{' consumed
        Json::Object obj;
        skip_ws();
        if (peek() == '}') {
            get();
            return Json{obj};
        }
        while (true) {
            skip_ws();
            if (get() != '"') throw GbfError(ErrorKind::HeaderJsonParse, "expected string key");
            Json keyj = parse_string();
            std::string key = std::get<std::string>(keyj.v);
            skip_ws();
            if (get() != ':') throw GbfError(ErrorKind::HeaderJsonParse, "expected ':' in object");
            skip_ws();
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            char c = get();
            if (c == '}') break;
            if (c != ',') throw GbfError(ErrorKind::HeaderJsonParse, "expected ',' in object");
        }
        return Json{obj};
    }

    Json parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"') { get(); return parse_string(); }
        if (c == '{') { get(); return parse_object(); }
        if (c == '[') { get(); return parse_array(); }
        if (c == 't') { expect("true"); return Json{true}; }
        if (c == 'f') { expect("false"); return Json{false}; }
        if (c == 'n') { expect("null"); return Json{nullptr}; }
        // number
        return parse_number();
    }

    void expect(const char* lit) {
        std::size_t n = std::strlen(lit);
        if (pos_ + n > s_.size() || s_.substr(pos_, n) != lit) {
            throw GbfError(ErrorKind::HeaderJsonParse, std::string("expected '") + lit + "'");
        }
        pos_ += n;
    }
};

static void json_escape_string(std::ostream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(c) << std::dec << std::setw(0);
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

static void json_serialize(std::ostream& os, const Json& j);

static void json_serialize_object(std::ostream& os, const Json::Object& obj) {
    os << '{';
    bool first = true;
    for (const auto& kv : obj) {
        if (!first) os << ',';
        first = false;
        json_escape_string(os, kv.first);
        os << ':';
        json_serialize(os, kv.second);
    }
    os << '}';
}

static void json_serialize_array(std::ostream& os, const Json::Array& arr) {
    os << '[';
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (i) os << ',';
        json_serialize(os, arr[i]);
    }
    os << ']';
}

static void json_serialize(std::ostream& os, const Json& j) {
    if (std::holds_alternative<std::nullptr_t>(j.v)) {
        os << "null";
    } else if (std::holds_alternative<bool>(j.v)) {
        os << (std::get<bool>(j.v) ? "true" : "false");
    } else if (std::holds_alternative<JsonNumber>(j.v)) {
        const auto& n = std::get<JsonNumber>(j.v);
        // Preserve the original raw representation if available; for writer we will construct numbers as integers,
        // so raw will already be stable.
        os << n.raw;
    } else if (std::holds_alternative<std::string>(j.v)) {
        json_escape_string(os, std::get<std::string>(j.v));
    } else if (std::holds_alternative<Json::Array>(j.v)) {
        json_serialize_array(os, std::get<Json::Array>(j.v));
    } else {
        json_serialize_object(os, std::get<Json::Object>(j.v));
    }
}

static std::string json_dump_compact(const Json& j) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    json_serialize(oss, j);
    return oss.str();
}

// Helpers to build numbers with stable raw string.
static Json json_u64(std::uint64_t v) {
    JsonNumber n;
    n.is_int = true;
    n.value = static_cast<double>(v);
    n.raw = std::to_string(v);
    return Json{n};
}

static Json json_i64(std::int64_t v) {
    JsonNumber n;
    n.is_int = true;
    n.value = static_cast<double>(v);
    n.raw = std::to_string(v);
    return Json{n};
}

static Json json_bool(bool b) { return Json{b}; }
static Json json_str(const std::string& s) { return Json{s}; }
static Json json_null() { return Json{nullptr}; }

} // namespace internal

// ------------------------------
// Small helpers
// ------------------------------

static constexpr std::uint32_t kMaxHeaderLen   = 64u * 1024u * 1024u; // 64MB
static constexpr std::uint64_t kMaxFieldUsize  = 16ull * 1024ull * 1024ull * 1024ull; // 16 GiB
static constexpr std::uint64_t kMaxFieldCsize  = 16ull * 1024ull * 1024ull * 1024ull; // 16 GiB

static bool checked_mul_size(std::size_t a, std::size_t b, std::size_t& out) {
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a > (std::numeric_limits<std::size_t>::max)() / b) return false;
    out = a * b;
    return true;
}

static bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a > (std::numeric_limits<std::uint64_t>::max)() - b) return false;
    out = a + b;
    return true;
}

static std::uint32_t read_u32_le(std::istream& is) {
    std::array<unsigned char, 4> b{};
    is.read(reinterpret_cast<char*>(b.data()), 4);
    if (!is) throw GbfError(ErrorKind::Truncated, "unexpected EOF while reading u32");
    return (static_cast<std::uint32_t>(b[0])      ) |
           (static_cast<std::uint32_t>(b[1]) <<  8) |
           (static_cast<std::uint32_t>(b[2]) << 16) |
           (static_cast<std::uint32_t>(b[3]) << 24);
}

static void write_u32_le(std::ostream& os, std::uint32_t v) {
    unsigned char b[4] = {
        static_cast<unsigned char>(v & 0xFFu),
        static_cast<unsigned char>((v >> 8) & 0xFFu),
        static_cast<unsigned char>((v >> 16) & 0xFFu),
        static_cast<unsigned char>((v >> 24) & 0xFFu),
    };
    os.write(reinterpret_cast<const char*>(b), 4);
}

static std::uint64_t u64_from_json(const internal::Json& j) {
    using internal::JsonNumber;
    if (std::holds_alternative<JsonNumber>(j.v)) {
        const auto& n = std::get<JsonNumber>(j.v);
        if (n.is_int) {
            // raw may be > 2^53 but in practice header uses matlab doubles; still parse as unsigned long long.
            try {
                std::size_t idx = 0;
                unsigned long long v = std::stoull(n.raw, &idx, 10);
                if (idx == n.raw.size()) return static_cast<std::uint64_t>(v);
            } catch (...) {
                // fall through
            }
        }
        // float or exponent
        if (!std::isfinite(n.value) || n.value < 0.0) return 0;
        long double ld = static_cast<long double>(n.value);
        long double rounded = std::llround(ld);
        if (rounded < 0) return 0;
        return static_cast<std::uint64_t>(rounded);
    }
    if (std::holds_alternative<std::string>(j.v)) {
        const auto& s = std::get<std::string>(j.v);
        try {
            std::size_t idx = 0;
            unsigned long long v = std::stoull(s, &idx, 10);
            if (idx == s.size()) return static_cast<std::uint64_t>(v);
        } catch (...) {}
        // hex?
        try {
            std::size_t idx = 0;
            unsigned long long v = std::stoull(s, &idx, 16);
            if (idx == s.size()) return static_cast<std::uint64_t>(v);
        } catch (...) {}
    }
    return 0;
}

static std::uint32_t u32_from_json(const internal::Json& j) {
    std::uint64_t v = u64_from_json(j);
    return static_cast<std::uint32_t>(v & 0xFFFFFFFFu);
}

static std::string str_from_json(const internal::Json& j) {
    if (std::holds_alternative<std::string>(j.v)) return std::get<std::string>(j.v);
    return {};
}

static bool bool_from_json(const internal::Json& j, bool def=false) {
    if (std::holds_alternative<bool>(j.v)) return std::get<bool>(j.v);
    return def;
}

static const internal::Json* obj_get(const internal::Json::Object& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &it->second;
}

static std::vector<std::uint64_t> shape_u64_from_json(const internal::Json& j) {
    std::vector<std::uint64_t> out;
    if (!std::holds_alternative<internal::Json::Array>(j.v)) return out;
    for (const auto& el : std::get<internal::Json::Array>(j.v)) {
        out.push_back(u64_from_json(el));
    }
    return out;
}

static std::vector<std::size_t> shape_usize_from_u64(const std::vector<std::uint64_t>& s) {
    std::vector<std::size_t> out;
    out.reserve(s.size());
    for (auto v : s) out.push_back(static_cast<std::size_t>(v));
    return out;
}

std::size_t numel(const std::vector<std::size_t>& shape) {
    if (shape.empty()) return 0;
    std::size_t n = 1;
    for (auto d : shape) {
        if (d == 0) throw GbfError(ErrorKind::InvalidData, "shape contains zero dimension");
        std::size_t tmp = 0;
        if (!checked_mul_size(n, d, tmp)) throw GbfError(ErrorKind::InvalidData, "shape size overflow");
        n = tmp;
    }
    return n;
}

std::size_t numel_u64(const std::vector<std::uint64_t>& shape) {
    if (shape.empty()) return 0;
    std::size_t n = 1;
    for (auto d : shape) {
        if (d == 0) throw GbfError(ErrorKind::InvalidData, "shape contains zero dimension");
        if (d > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw GbfError(ErrorKind::InvalidData, "shape dimension overflow");
        }
        std::size_t tmp = 0;
        if (!checked_mul_size(n, static_cast<std::size_t>(d), tmp)) throw GbfError(ErrorKind::InvalidData, "shape size overflow");
        n = tmp;
    }
    return n;
}

static std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto dot = s.find('.', start);
        if (dot == std::string::npos) dot = s.size();
        parts.push_back(s.substr(start, dot - start));
        start = dot + 1;
        if (dot == s.size()) break;
    }
    // remove empty segments
    for (const auto& p : parts) {
        if (p.empty()) {
            throw GbfError(ErrorKind::InvalidData, "invalid path: empty segment");
        }
    }
    return parts;}

static std::string join_path(const std::vector<std::string>& parts, std::size_t upto) {
    std::string out;
    for (std::size_t i = 0; i < upto; ++i) {
        if (i) out.push_back('.');
        out += parts[i];
    }
    return out;
}

static std::string upper_hex8(std::uint32_t v) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

static std::uint32_t parse_hex_u32(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) t.push_back(c);
    }
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        t = t.substr(2);
    }
    if (t.empty()) return 0;
    std::uint32_t v = 0;
    std::istringstream iss(t);
    iss >> std::hex >> v;
    return v;
}

// Replace the value of "header_crc32_hex" in-place with all '0's (keeping length).
static void zero_out_header_crc_value(std::string& json) {
    const std::string key = "\"header_crc32_hex\"";
    auto kpos = json.find(key);
    if (kpos == std::string::npos) return;

    auto colon = json.find(':', kpos + key.size());
    if (colon == std::string::npos) return;

    auto q1 = json.find('"', colon);
    if (q1 == std::string::npos) return;
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return;

    for (std::size_t i = q1 + 1; i < q2; ++i) {
        json[i] = '0';
    }
}

static std::uint32_t crc32_bytes(const std::uint8_t* data, std::size_t len) {
    uLong crc = ::crc32(0L, Z_NULL, 0);
    crc = ::crc32(crc, reinterpret_cast<const Bytef*>(data), static_cast<uInt>(len));
    return static_cast<std::uint32_t>(crc);
}

static std::uint32_t crc32_str_zeroed_header(const std::string& header_json_raw) {
    std::string tmp = header_json_raw;
    zero_out_header_crc_value(tmp);
    return crc32_bytes(reinterpret_cast<const std::uint8_t*>(tmp.data()), tmp.size());
}

static std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t>& in, int level) {
    if (in.empty()) return {};
    uLongf bound = ::compressBound(static_cast<uLong>(in.size()));
    std::vector<std::uint8_t> out(bound);

    uLongf out_len = bound;
    int rc = ::compress2(reinterpret_cast<Bytef*>(out.data()), &out_len,
                         reinterpret_cast<const Bytef*>(in.data()),
                         static_cast<uLong>(in.size()),
                         level);
    if (rc != Z_OK) {
        throw GbfError(ErrorKind::ZlibError, "zlib compress2 failed");
    }
    out.resize(static_cast<std::size_t>(out_len));
    return out;
}

static std::vector<std::uint8_t> zlib_decompress(const std::vector<std::uint8_t>& in, std::size_t usize) {
    if (usize == 0) return {};
    if (usize > static_cast<std::size_t>(kMaxFieldUsize)) {
        throw GbfError(ErrorKind::InvalidData, "field usize exceeds configured limit");
    }
    std::vector<std::uint8_t> out(usize);
    uLongf out_len = static_cast<uLongf>(usize);
    int rc = ::uncompress(reinterpret_cast<Bytef*>(out.data()), &out_len,
                          reinterpret_cast<const Bytef*>(in.data()),
                          static_cast<uLong>(in.size()));
    if (rc != Z_OK || static_cast<std::size_t>(out_len) != usize) {
        throw GbfError(ErrorKind::ZlibError, "zlib uncompress failed");
    }
    return out;
}

// ------------------------------
// Numeric class helpers
// ------------------------------

std::string to_string(NumericClass c) {
    switch (c) {
        case NumericClass::Double: return "double";
        case NumericClass::Single: return "single";
        case NumericClass::Int8: return "int8";
        case NumericClass::UInt8: return "uint8";
        case NumericClass::Int16: return "int16";
        case NumericClass::UInt16: return "uint16";
        case NumericClass::Int32: return "int32";
        case NumericClass::UInt32: return "uint32";
        case NumericClass::Int64: return "int64";
        case NumericClass::UInt64: return "uint64";
        default: return "unknown";
    }
}

NumericClass numeric_class_from_string(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "double") return NumericClass::Double;
    if (t == "single") return NumericClass::Single;
    if (t == "int8") return NumericClass::Int8;
    if (t == "uint8") return NumericClass::UInt8;
    if (t == "int16") return NumericClass::Int16;
    if (t == "uint16") return NumericClass::UInt16;
    if (t == "int32") return NumericClass::Int32;
    if (t == "uint32") return NumericClass::UInt32;
    if (t == "int64") return NumericClass::Int64;
    if (t == "uint64") return NumericClass::UInt64;
    return NumericClass::Unknown;
}

static std::size_t bytes_per_elem(NumericClass c) {
    switch (c) {
        case NumericClass::Double: return 8;
        case NumericClass::Single: return 4;
        case NumericClass::Int8: return 1;
        case NumericClass::UInt8: return 1;
        case NumericClass::Int16: return 2;
        case NumericClass::UInt16: return 2;
        case NumericClass::Int32: return 4;
        case NumericClass::UInt32: return 4;
        case NumericClass::Int64: return 8;
        case NumericClass::UInt64: return 8;
        default: return 1;
    }
}

// ------------------------------
// GbfValue helpers
// ------------------------------

GbfValue GbfValue::make_struct() {
    GbfValue v;
    v.v = Struct{};
    return v;
}

GbfValue GbfValue::make_struct(const Struct& m) {
    GbfValue v;
    v.v = m;
    return v;
}

GbfValue GbfValue::make_numeric(const NumericArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_logical(const LogicalArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_string(const StringArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_char(const CharArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_datetime(const DateTimeArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_duration(const DurationArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_calendarduration(const CalendarDurationArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_categorical(const CategoricalArray& a) {
    GbfValue v;
    v.v = a;
    return v;
}

GbfValue GbfValue::make_opaque(const OpaqueValue& a) {
    GbfValue v;
    v.v = a;
    return v;
}

bool GbfValue::is_struct() const noexcept {
    return std::holds_alternative<Struct>(v);
}

const GbfValue::Struct& GbfValue::as_struct() const {
    if (!std::holds_alternative<Struct>(v)) throw GbfError(ErrorKind::InvalidData, "value is not a struct");
    return std::get<Struct>(v);
}

GbfValue::Struct& GbfValue::as_struct() {
    if (!std::holds_alternative<Struct>(v)) throw GbfError(ErrorKind::InvalidData, "value is not a struct");
    return std::get<Struct>(v);
}

// ------------------------------
// Header parse/build
// ------------------------------

static Header parse_header(const std::string& raw_json) {
    internal::JsonParser p(raw_json);
    internal::Json root = p.parse();
    if (!root.is_object()) throw GbfError(ErrorKind::HeaderJsonParse, "header JSON is not an object");

    const auto& obj = root.as_object();
    Header h;

    if (auto* v = obj_get(obj, "format")) h.format = str_from_json(*v);
    if (auto* v = obj_get(obj, "magic")) h.magic = str_from_json(*v);
    if (auto* v = obj_get(obj, "version")) h.version = static_cast<int>(u64_from_json(*v));
    if (auto* v = obj_get(obj, "endianness")) h.endianness = str_from_json(*v);
    if (auto* v = obj_get(obj, "order")) h.order = str_from_json(*v);
    if (auto* v = obj_get(obj, "root")) h.root = str_from_json(*v);
    if (auto* v = obj_get(obj, "created_utc")) h.created_utc = str_from_json(*v);
    if (auto* v = obj_get(obj, "matlab_version")) h.matlab_version = str_from_json(*v);

    if (auto* v = obj_get(obj, "payload_start")) h.payload_start = u64_from_json(*v);
    if (auto* v = obj_get(obj, "file_size")) h.file_size = u64_from_json(*v);
    if (auto* v = obj_get(obj, "header_crc32_hex")) h.header_crc32_hex = str_from_json(*v);

    if (auto* fv = obj_get(obj, "fields")) {
        if (std::holds_alternative<internal::Json::Array>(fv->v)) {
            for (const auto& fj : std::get<internal::Json::Array>(fv->v)) {
                if (!fj.is_object()) continue;
                const auto& fo = fj.as_object();
                FieldMeta f;
                if (auto* x = obj_get(fo, "name")) f.name = str_from_json(*x);
                if (auto* x = obj_get(fo, "kind")) f.kind = str_from_json(*x);
                if (auto* x = obj_get(fo, "class")) f.class_name = str_from_json(*x);
                if (auto* x = obj_get(fo, "shape")) f.shape = shape_u64_from_json(*x);
                if (auto* x = obj_get(fo, "complex")) f.complex = bool_from_json(*x, false);
                if (auto* x = obj_get(fo, "encoding")) f.encoding = str_from_json(*x);
                if (auto* x = obj_get(fo, "compression")) f.compression = str_from_json(*x);
                if (auto* x = obj_get(fo, "offset")) f.offset = u64_from_json(*x);
                if (auto* x = obj_get(fo, "csize")) f.csize = u64_from_json(*x);
                if (auto* x = obj_get(fo, "usize")) f.usize = u64_from_json(*x);
                if (auto* x = obj_get(fo, "crc32")) f.crc32 = u32_from_json(*x);
                h.fields.push_back(std::move(f));
            }
        }
    }

    return h;
}

static internal::Json header_to_json(const Header& h, bool crc_zeroed) {
    using internal::Json;
    using internal::json_bool;
    using internal::json_str;
    using internal::json_u64;

    internal::Json::Object obj;

    obj.emplace("format", json_str(h.format));
    obj.emplace("magic", json_str(h.magic));
    obj.emplace("version", internal::json_u64(static_cast<std::uint64_t>(h.version)));
    obj.emplace("endianness", json_str(h.endianness));
    obj.emplace("order", json_str(h.order));
    obj.emplace("root", json_str(h.root));
    if (!h.created_utc.empty()) obj.emplace("created_utc", json_str(h.created_utc));
    if (!h.matlab_version.empty()) obj.emplace("matlab_version", json_str(h.matlab_version));

    // fields
    internal::Json::Array fields;
    fields.reserve(h.fields.size());
    for (const auto& f : h.fields) {
        internal::Json::Object fo;
        fo.emplace("name", json_str(f.name));
        fo.emplace("kind", json_str(f.kind));
        fo.emplace("class", json_str(f.class_name));

        internal::Json::Array shape;
        for (auto d : f.shape) shape.push_back(json_u64(d));
        fo.emplace("shape", Json{shape});

        fo.emplace("complex", json_bool(f.complex));
        fo.emplace("encoding", json_str(f.encoding));
        fo.emplace("compression", json_str(f.compression));
        fo.emplace("offset", json_u64(f.offset));
        fo.emplace("csize", json_u64(f.csize));
        fo.emplace("usize", json_u64(f.usize));
        fo.emplace("crc32", json_u64(f.crc32));

        fields.push_back(Json{fo});
    }
    obj.emplace("fields", Json{fields});

    obj.emplace("payload_start", json_u64(h.payload_start));
    obj.emplace("file_size", json_u64(h.file_size));
    obj.emplace("header_crc32_hex", json_str(crc_zeroed ? "00000000" : h.header_crc32_hex));

    return Json{obj};
}

// ------------------------------
// Value <-> bytes encoding
// ------------------------------

static std::vector<std::uint8_t> encode_u32_le(std::uint32_t v) {
    return {
        static_cast<std::uint8_t>(v & 0xFFu),
        static_cast<std::uint8_t>((v >> 8) & 0xFFu),
        static_cast<std::uint8_t>((v >> 16) & 0xFFu),
        static_cast<std::uint8_t>((v >> 24) & 0xFFu),
    };
}

static void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    auto b = encode_u32_le(v);
    out.insert(out.end(), b.begin(), b.end());
}

static void append_i32_le(std::vector<std::uint8_t>& out, std::int32_t v) {
    append_u32_le(out, static_cast<std::uint32_t>(v));
}

static void append_i64_le(std::vector<std::uint8_t>& out, std::int64_t v) {
    std::uint64_t u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((u >> (8*i)) & 0xFFu));
}

static void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

static std::uint32_t read_u32_le_from(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0])      ) |
           (static_cast<std::uint32_t>(p[1]) <<  8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

static std::int32_t read_i32_le_from(const std::uint8_t* p) {
    return static_cast<std::int32_t>(read_u32_le_from(p));
}

static std::int64_t read_i64_le_from(const std::uint8_t* p) {
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= (static_cast<std::uint64_t>(p[i]) << (8*i));
    return static_cast<std::int64_t>(u);
}

static std::uint16_t read_u16_le_from(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

static std::vector<std::uint8_t> encode_value_bytes(const GbfValue& v, FieldMeta& meta) {
    std::vector<std::uint8_t> out;

    // struct: only empty scalar struct is encoded as empty.
    if (std::holds_alternative<GbfValue::Struct>(v.v)) {
        meta.kind = "struct";
        meta.class_name = "struct";
        meta.encoding = "empty-scalar-struct";
        meta.complex = false;
        meta.usize = 0;
        return {};
    }

    if (std::holds_alternative<NumericArray>(v.v)) {
        const auto& a = std::get<NumericArray>(v.v);
        meta.kind = "numeric";
        meta.class_name = to_string(a.class_id);
        meta.encoding = "";
        meta.complex = a.complex;
        // shape in header is u64
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        const std::size_t elem = bytes_per_elem(a.class_id);
        std::size_t expected_real = 0;
        if (!checked_mul_size(numel(a.shape), elem, expected_real)) {
            throw GbfError(ErrorKind::InvalidData, "numeric payload size overflow");
        }
        if (a.real_le.size() != expected_real) {
            throw GbfError(ErrorKind::InvalidData, "numeric real_le size does not match shape/class");
        }
        if (!a.complex) {
            if (a.imag_le.has_value() && !a.imag_le->empty()) {
                throw GbfError(ErrorKind::InvalidData, "non-complex numeric must not have imag_le");
            }
        } else {
            if (!a.imag_le) throw GbfError(ErrorKind::InvalidData, "complex numeric requires imag_le");
            if (a.imag_le->size() != expected_real) {
                throw GbfError(ErrorKind::InvalidData, "numeric imag_le size does not match shape/class");
            }
        }

        out = a.real_le;
        if (a.complex) {
            if (!a.imag_le) throw GbfError(ErrorKind::InvalidData, "complex numeric requires imag_le");
            const auto& im = *a.imag_le;
            out.insert(out.end(), im.begin(), im.end());
        }
        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<LogicalArray>(v.v)) {
        const auto& a = std::get<LogicalArray>(v.v);
        meta.kind = "logical";
        meta.class_name = "logical";
        meta.encoding = "";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));
        out = a.data;
        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<StringArray>(v.v)) {
        const auto& a = std::get<StringArray>(v.v);
        meta.kind = "string";
        meta.class_name = "string";
        meta.encoding = "utf-8";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        // Per-element: [u8 missing][u32 len][bytes]
        if (a.data.size() != numel(a.shape)) {
            throw GbfError(ErrorKind::InvalidData, "string array data length does not match shape");
        }
        for (const auto& el : a.data) {
            if (!el.has_value()) {
                out.push_back(1);
                append_u32_le(out, 0);
            } else {
                out.push_back(0);
                const auto& s = *el;
                append_u32_le(out, static_cast<std::uint32_t>(s.size()));
                out.insert(out.end(), s.begin(), s.end());
            }
        }
        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<CharArray>(v.v)) {
        const auto& a = std::get<CharArray>(v.v);
        meta.kind = "char";
        meta.class_name = "char";
        meta.encoding = "utf-16-codeunits";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        if (a.utf16.size() != numel(a.shape)) {
            throw GbfError(ErrorKind::InvalidData, "char array data length does not match shape");
        }
        out.reserve(a.utf16.size() * 2);
        for (std::uint16_t cu : a.utf16) append_u16_le(out, cu);
        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<DateTimeArray>(v.v)) {
        const auto& a = std::get<DateTimeArray>(v.v);
        meta.kind = "datetime";
        meta.class_name = "datetime";
        meta.encoding = a.timezone.empty()
            ? "dt:naive-unixms+nat-mask+tz+locale+format"
            : "dt:tz-unixms+nat-mask+tz+locale+format";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        std::size_t n = numel(a.shape);
        if (a.nat_mask.size() != n || a.unix_ms.size() != n) {
            throw GbfError(ErrorKind::InvalidData, "datetime arrays must match shape");
        }

        // Layout inferred from MATLAB files:
        // [u8 n_strings=3] [u32 tz_len][tz bytes] [u32 locale_len][locale bytes] [u32 fmt_len][fmt bytes]
        // [nat_mask bytes (n)] [i64 unix_ms values (n)]
        out.push_back(3);
        append_u32_le(out, static_cast<std::uint32_t>(a.timezone.size()));
        out.insert(out.end(), a.timezone.begin(), a.timezone.end());
        append_u32_le(out, static_cast<std::uint32_t>(a.locale.size()));
        out.insert(out.end(), a.locale.begin(), a.locale.end());
        append_u32_le(out, static_cast<std::uint32_t>(a.format.size()));
        out.insert(out.end(), a.format.begin(), a.format.end());
        out.insert(out.end(), a.nat_mask.begin(), a.nat_mask.end());
        for (auto ms : a.unix_ms) append_i64_le(out, ms);

        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<DurationArray>(v.v)) {
        const auto& a = std::get<DurationArray>(v.v);
        meta.kind = "duration";
        meta.class_name = "duration";
        meta.encoding = "ms-i64+nan-mask";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        std::size_t n = numel(a.shape);
        if (a.nan_mask.size() != n || a.ms.size() != n) {
            throw GbfError(ErrorKind::InvalidData, "duration arrays must match shape");
        }
        out.insert(out.end(), a.nan_mask.begin(), a.nan_mask.end());
        for (auto ms : a.ms) append_i64_le(out, ms);

        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<CalendarDurationArray>(v.v)) {
        const auto& a = std::get<CalendarDurationArray>(v.v);
        meta.kind = "calendarduration";
        meta.class_name = "calendarDuration";
        meta.encoding = "mask+months-i32+days-i32+time-ms-i64";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        std::size_t n = numel(a.shape);
        if (a.mask.size() != n || a.months.size() != n || a.days.size() != n || a.time_ms.size() != n) {
            throw GbfError(ErrorKind::InvalidData, "calendarduration arrays must match shape");
        }
        out.insert(out.end(), a.mask.begin(), a.mask.end());
        for (auto x : a.months) append_i32_le(out, x);
        for (auto x : a.days) append_i32_le(out, x);
        for (auto x : a.time_ms) append_i64_le(out, x);

        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<CategoricalArray>(v.v)) {
        const auto& a = std::get<CategoricalArray>(v.v);
        meta.kind = "categorical";
        meta.class_name = "categorical";
        meta.encoding = "cats-utf8+codes-u32";
        meta.complex = false;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));

        std::size_t n = numel(a.shape);
        if (a.codes.size() != n) {
            throw GbfError(ErrorKind::InvalidData, "categorical codes must match shape");
        }

        append_u32_le(out, static_cast<std::uint32_t>(a.categories.size()));
        for (const auto& s : a.categories) {
            append_u32_le(out, static_cast<std::uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        }
        for (auto code : a.codes) append_u32_le(out, code);

        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    if (std::holds_alternative<OpaqueValue>(v.v)) {
        const auto& a = std::get<OpaqueValue>(v.v);
        meta.kind = a.kind;
        meta.class_name = a.class_name;
        meta.encoding = a.encoding;
        meta.complex = a.complex;
        meta.shape.clear();
        for (auto d : a.shape) meta.shape.push_back(static_cast<std::uint64_t>(d));
        out = a.bytes;
        meta.usize = static_cast<std::uint64_t>(out.size());
        return out;
    }

    throw GbfError(ErrorKind::Unsupported, "unsupported value variant");
}

static GbfValue decode_value_bytes(const FieldMeta& meta, const std::vector<std::uint8_t>& bytes) {
    const std::string kind = meta.kind;
    const std::string cls = meta.class_name;
    std::vector<std::size_t> shape = shape_usize_from_u64(meta.shape);
    std::size_t n = numel(shape);

    // Empty payloads: construct empty containers per kind.
    if (bytes.empty()) {
        if (kind == "struct") {
            return GbfValue::make_struct();
        }
        if (kind == "numeric") {
            NumericArray a;
            a.class_id = numeric_class_from_string(cls);
            a.shape = shape;
            a.complex = meta.complex;
            return GbfValue::make_numeric(a);
        }
        if (kind == "logical") {
            LogicalArray a;
            a.shape = shape;
            return GbfValue::make_logical(a);
        }
        if (kind == "string") {
            StringArray a;
            a.shape = shape;
            a.data.assign(n, std::nullopt);
            return GbfValue::make_string(a);
        }
        if (kind == "char") {
            CharArray a;
            a.shape = shape;
            return GbfValue::make_char(a);
        }
        // Others: create opaque.
        OpaqueValue o;
        o.kind = kind;
        o.class_name = cls;
        o.shape = shape;
        o.complex = meta.complex;
        o.encoding = meta.encoding;
        o.bytes = bytes;
        return GbfValue::make_opaque(o);
    }

    if (kind == "struct") {
        // empty scalar struct marker
        return GbfValue::make_struct();
    }

    if (kind == "numeric") {
        NumericArray a;
        a.class_id = numeric_class_from_string(cls);
        a.shape = shape;
        a.complex = meta.complex;

        const std::size_t elem = bytes_per_elem(a.class_id);
        std::size_t expected_real = 0;
        if (!checked_mul_size(n, elem, expected_real)) {
            throw GbfError(ErrorKind::InvalidData, "numeric expected size overflow");
        }

        if (!a.complex) {
            if (bytes.size() != expected_real) {
                throw GbfError(ErrorKind::InvalidData, "numeric payload size does not match shape/class");
            }
            a.real_le = bytes;
        } else {
            std::size_t expected_total = 0;
            if (!checked_mul_size(expected_real, 2, expected_total)) {
                throw GbfError(ErrorKind::InvalidData, "numeric expected size overflow");
            }
            if (bytes.size() != expected_total) {
                throw GbfError(ErrorKind::InvalidData, "complex numeric payload size does not match shape/class");
            }
            a.real_le.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(expected_real));
            std::vector<std::uint8_t> im(bytes.begin() + static_cast<std::ptrdiff_t>(expected_real), bytes.end());
            a.imag_le = std::move(im);
        }
        return GbfValue::make_numeric(a);
    }

    if (kind == "logical") {
        LogicalArray a;
        a.shape = shape;
        a.data = bytes;
        return GbfValue::make_logical(a);
    }

    if (kind == "string") {
        StringArray a;
        a.shape = shape;
        a.data.clear();
        a.data.reserve(n);

        std::size_t pos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (pos + 1 + 4 > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated string payload");
            std::uint8_t missing = bytes[pos];
            std::uint32_t len = read_u32_le_from(&bytes[pos + 1]);
            pos += 1 + 4;
            if (pos + len > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated string payload");
            if (missing != 0) {
                // ignore len bytes if len>0? (should be 0)
                pos += len;
                a.data.push_back(std::nullopt);
            } else {
                std::string s(reinterpret_cast<const char*>(&bytes[pos]), reinterpret_cast<const char*>(&bytes[pos + len]));
                pos += len;
                a.data.push_back(std::move(s));
            }
        }
        // ignore trailing bytes (tolerant)
        return GbfValue::make_string(a);
    }

    if (kind == "char") {
        CharArray a;
        a.shape = shape;
        std::size_t expected = n * 2;
        if (bytes.size() < expected) throw GbfError(ErrorKind::Truncated, "truncated char payload");
        a.utf16.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            a.utf16.push_back(read_u16_le_from(&bytes[i * 2]));
        }
        return GbfValue::make_char(a);
    }

    if (kind == "datetime") {
        DateTimeArray a;
        a.shape = shape;
        std::size_t pos = 0;
        if (pos + 1 > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated datetime payload");
        std::uint8_t nstr = bytes[pos++];
        // Expect 3 strings, but be tolerant.
        std::vector<std::string> strs;
        for (std::size_t si = 0; si < nstr; ++si) {
            if (pos + 4 > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated datetime payload");
            std::uint32_t len = read_u32_le_from(&bytes[pos]);
            pos += 4;
            if (pos + len > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated datetime payload");
            strs.emplace_back(reinterpret_cast<const char*>(&bytes[pos]), reinterpret_cast<const char*>(&bytes[pos + len]));
            pos += len;
        }
        // Map: [tz, locale, format] when available.
        if (strs.size() > 0) a.timezone = strs[0];
        if (strs.size() > 1) a.locale = strs[1];
        if (strs.size() > 2) a.format = strs[2];

        if (pos + n > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated datetime payload");
        a.nat_mask.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;

        if (pos + 8 * n > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated datetime payload");
        a.unix_ms.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            a.unix_ms[i] = read_i64_le_from(&bytes[pos + i * 8]);
        }
        return GbfValue::make_datetime(a);
    }

    if (kind == "duration") {
        DurationArray a;
        a.shape = shape;
        std::size_t expected = n + 8 * n;
        if (bytes.size() < expected) throw GbfError(ErrorKind::Truncated, "truncated duration payload");
        a.nan_mask.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        a.ms.resize(n);
        std::size_t pos = n;
        for (std::size_t i = 0; i < n; ++i) {
            a.ms[i] = read_i64_le_from(&bytes[pos + i * 8]);
        }
        return GbfValue::make_duration(a);
    }

    if (kind == "calendarduration") {
        CalendarDurationArray a;
        a.shape = shape;
        std::size_t expected = n + 4 * n + 4 * n + 8 * n;
        if (bytes.size() < expected) throw GbfError(ErrorKind::Truncated, "truncated calendarduration payload");
        std::size_t pos = 0;
        a.mask.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        pos += n;
        a.months.resize(n);
        for (std::size_t i = 0; i < n; ++i) a.months[i] = read_i32_le_from(&bytes[pos + i * 4]);
        pos += 4 * n;
        a.days.resize(n);
        for (std::size_t i = 0; i < n; ++i) a.days[i] = read_i32_le_from(&bytes[pos + i * 4]);
        pos += 4 * n;
        a.time_ms.resize(n);
        for (std::size_t i = 0; i < n; ++i) a.time_ms[i] = read_i64_le_from(&bytes[pos + i * 8]);
        return GbfValue::make_calendarduration(a);
    }

    if (kind == "categorical") {
        CategoricalArray a;
        a.shape = shape;
        std::size_t pos = 0;
        if (pos + 4 > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated categorical payload");
        std::uint32_t ncat = read_u32_le_from(&bytes[pos]);
        pos += 4;
        a.categories.clear();
        a.categories.reserve(ncat);
        for (std::uint32_t i = 0; i < ncat; ++i) {
            if (pos + 4 > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated categorical payload");
            std::uint32_t len = read_u32_le_from(&bytes[pos]);
            pos += 4;
            if (pos + len > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated categorical payload");
            a.categories.emplace_back(reinterpret_cast<const char*>(&bytes[pos]), reinterpret_cast<const char*>(&bytes[pos + len]));
            pos += len;
        }
        if (pos + 4 * n > bytes.size()) throw GbfError(ErrorKind::Truncated, "truncated categorical payload");
        a.codes.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            a.codes[i] = read_u32_le_from(&bytes[pos + i * 4]);
        }
        return GbfValue::make_categorical(a);
    }

    // Unknown kind => opaque
    OpaqueValue o;
    o.kind = kind;
    o.class_name = cls;
    o.shape = shape;
    o.complex = meta.complex;
    o.encoding = meta.encoding;
    o.bytes = bytes;
    return GbfValue::make_opaque(o);
}

static void insert_path(GbfValue& root, const std::string& path, const GbfValue& leaf) {
    auto parts = split_path(path);
    if (parts.empty()) return;

    GbfValue* cur = &root;
    if (!cur->is_struct()) {
        *cur = GbfValue::make_struct();
    }

    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        auto& m = cur->as_struct();
        auto it = m.find(parts[i]);
        if (it == m.end()) {
            it = m.emplace(parts[i], GbfValue::make_struct()).first;
            } else if (!it->second.is_struct()) {
                throw GbfError(ErrorKind::InvalidData, "path hits non-struct at '" + join_path(parts, i + 1) + "'");
            }
        cur = &it->second;
    }

    cur->as_struct()[parts.back()] = leaf;
}

static std::vector<const FieldMeta*> fields_with_prefix(const std::vector<FieldMeta>& fields, const std::string& prefix) {
    std::vector<const FieldMeta*> out;
    for (const auto& f : fields) {
        if (prefix.empty() || prefix == "<root>") {
            out.push_back(&f);
        } else if (f.name == prefix || (f.name.size() > prefix.size() && f.name.compare(0, prefix.size(), prefix) == 0 && f.name[prefix.size()] == '.')) {
            out.push_back(&f);
        }
    }
    return out;
}

// ------------------------------
// API implementations
// ------------------------------

std::tuple<Header, std::uint32_t, std::string> read_header_only(
    const std::filesystem::path& file,
    const ReadOptions& opts
) {
    std::ifstream is(file, std::ios::binary);
    if (!is) {
        throw GbfError(ErrorKind::Io, "failed to open file: " + file.string());
    }

    std::array<char, 8> magic{};
    is.read(magic.data(), magic.size());
    if (!is) throw GbfError(ErrorKind::Truncated, "unexpected EOF reading magic");
    std::string magic_s(magic.data(), magic.size());
    // trim trailing NULs
    while (!magic_s.empty() && magic_s.back() == '\0') magic_s.pop_back();

    if (magic_s != "GREDBIN") {
        throw GbfError(ErrorKind::BadMagic, "bad magic: '" + magic_s + "'");
    }

    std::uint32_t header_len = read_u32_le(is);
    if (header_len == 0 || header_len > kMaxHeaderLen) {
        throw GbfError(ErrorKind::InvalidData, "unreasonable header length");
    }
    std::string raw_json;
    raw_json.resize(header_len);
    is.read(&raw_json[0], header_len);
    if (!is) throw GbfError(ErrorKind::Truncated, "unexpected EOF reading header JSON");

    Header hdr = parse_header(raw_json);

    // Compute payload_start from framing if missing.
    std::uint64_t computed_payload_start = 8ull + 4ull + static_cast<std::uint64_t>(header_len);
    if (hdr.payload_start == 0) hdr.payload_start = computed_payload_start;

    // File size
    std::uint64_t actual_size = 0;
    try {
        actual_size = static_cast<std::uint64_t>(std::filesystem::file_size(file));
    } catch (...) {
        actual_size = 0;
    }
    if (actual_size != 0) {
        std::uint64_t min_size = 0;
        if (!checked_add_u64(8ull + 4ull, static_cast<std::uint64_t>(header_len), min_size)) {
            throw GbfError(ErrorKind::InvalidData, "size overflow computing payload start");
        }
        if (actual_size < min_size) {
            throw GbfError(ErrorKind::Truncated, "file too small for header length");
        }
    }
    if (hdr.file_size == 0 && actual_size != 0) hdr.file_size = actual_size;

    // Validate header CRC
    if (opts.validate) {
        std::uint32_t expected = parse_hex_u32(hdr.header_crc32_hex);
        std::uint32_t got = crc32_str_zeroed_header(raw_json);
        if (expected != 0 && expected != got) {
            std::ostringstream oss;
            oss << "header CRC mismatch: expected " << upper_hex8(expected) << ", got " << upper_hex8(got);
            throw GbfError(ErrorKind::HeaderCrcMismatch, oss.str());
        }
    }

    return {hdr, header_len, raw_json};
}

static std::vector<std::uint8_t> read_field_payload(
    std::ifstream& is,
    const Header& hdr,
    const FieldMeta& f,
    const ReadOptions& opts
) {
    if (f.csize == 0 || f.usize == 0) {
        return {};
    }
    if (f.usize > kMaxFieldUsize || f.csize > kMaxFieldCsize) {
        throw GbfError(ErrorKind::InvalidData, "field size exceeds configured limit");
    }

    std::uint64_t pos = 0;
    if (!checked_add_u64(hdr.payload_start, f.offset, pos)) {
        throw GbfError(ErrorKind::InvalidData, "payload offset overflow");
    }
    if (hdr.file_size != 0) {
        std::uint64_t end = 0;
        if (!checked_add_u64(pos, f.csize, end) || end > hdr.file_size) {
            throw GbfError(ErrorKind::Truncated, "field payload exceeds file bounds");
        }
    }
    is.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    if (!is) throw GbfError(ErrorKind::Io, "seek failed while reading payload");

    std::vector<std::uint8_t> chunk(static_cast<std::size_t>(f.csize));
    is.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    if (!is) throw GbfError(ErrorKind::Truncated, "unexpected EOF reading field payload");

    std::vector<std::uint8_t> raw;
    if (f.compression == "zlib") {
        raw = zlib_decompress(chunk, static_cast<std::size_t>(f.usize));
    } else {
        raw = std::move(chunk);
        if (raw.size() != static_cast<std::size_t>(f.usize)) {
            // tolerate; do not throw here unless validate
            if (opts.validate) {
                throw GbfError(ErrorKind::Truncated, "field usize mismatch");
            }
        }
    }

    if (opts.validate && f.crc32 != 0 && !raw.empty()) {
        std::uint32_t got = crc32_bytes(raw.data(), raw.size());
        if (got != f.crc32) {
            std::ostringstream oss;
            oss << "field CRC mismatch for '" << f.name << "': expected " << upper_hex8(f.crc32) << ", got " << upper_hex8(got);
            throw GbfError(ErrorKind::FieldCrcMismatch, oss.str());
        }
    }

    return raw;
}

GbfValue read_file(const std::filesystem::path& file, const ReadOptions& opts) {
    auto [hdr, header_len, raw_json] = read_header_only(file, opts);

    std::ifstream is(file, std::ios::binary);
    if (!is) throw GbfError(ErrorKind::Io, "failed to open file: " + file.string());

    GbfValue root = GbfValue::make_struct();

    for (const auto& f : hdr.fields) {
        std::vector<std::uint8_t> payload = read_field_payload(is, hdr, f, opts);
        GbfValue leaf = decode_value_bytes(f, payload);
        insert_path(root, f.name, leaf);
    }

    return root;
}

GbfValue read_var(const std::filesystem::path& file, const std::string& var, const ReadOptions& opts) {
    auto [hdr, header_len, raw_json] = read_header_only(file, opts);

    // Root special case
    if (var.empty() || var == "<root>") {
        return read_file(file, opts);
    }

    // Find exact leaf
    const FieldMeta* exact = nullptr;
    for (const auto& f : hdr.fields) {
        if (f.name == var) { exact = &f; break; }
    }

    std::ifstream is(file, std::ios::binary);
    if (!is) throw GbfError(ErrorKind::Io, "failed to open file: " + file.string());

    if (exact) {
        std::vector<std::uint8_t> payload = read_field_payload(is, hdr, *exact, opts);
        return decode_value_bytes(*exact, payload);
    }

    // Otherwise treat var as prefix and build struct
    auto selected = fields_with_prefix(hdr.fields, var);
    if (selected.empty()) {
        throw GbfError(ErrorKind::NotFound, "variable not found: " + var);
    }

    GbfValue out = GbfValue::make_struct();
    for (const auto* fp : selected) {
        std::vector<std::uint8_t> payload = read_field_payload(is, hdr, *fp, opts);
        GbfValue leaf = decode_value_bytes(*fp, payload);

        // Trim prefix from name
        std::string rel = fp->name;
        if (rel == var) {
            // exact should have matched, but just in case
            out = leaf;
            continue;
        }
        if (!var.empty() && rel.size() > var.size() && rel.compare(0, var.size(), var) == 0 && rel[var.size()] == '.') {
            rel = rel.substr(var.size() + 1);
        }
        insert_path(out, rel, leaf);
    }
    return out;
}

static void flatten(const GbfValue& v, const std::string& prefix, std::vector<std::pair<std::string, GbfValue>>& leaves) {
    if (std::holds_alternative<GbfValue::Struct>(v.v)) {
        const auto& m = std::get<GbfValue::Struct>(v.v);
        if (m.empty()) {
            // Empty scalar struct: materialize leaf only when it is explicitly a value (i.e., prefix itself is a named field).
            if (!prefix.empty()) {
                leaves.emplace_back(prefix, v);
            }
            return;
        }
        for (const auto& kv : m) {
            std::string child = prefix.empty() ? kv.first : (prefix + "." + kv.first);
            flatten(kv.second, child, leaves);
        }
        return;
    }
    leaves.emplace_back(prefix, v);
}

void write_file(const std::filesystem::path& file, const GbfValue& root, const WriteOptions& opts) {
    // Flatten root into leaves. Root is typically a struct; for non-struct root, store at "<root>".
    std::vector<std::pair<std::string, GbfValue>> leaves;
    if (std::holds_alternative<GbfValue::Struct>(root.v)) {
        flatten(root, "", leaves);
    } else {
        leaves.emplace_back(std::string("<root>"), root);
    }

    // Build fields and payload
    Header hdr;
    hdr.format = "GBF";
    hdr.magic = "GREDBIN";
    hdr.version = 1;
    hdr.endianness = "little";
    hdr.order = "column-major";
    hdr.root = "struct";
    hdr.created_utc = ""; // caller can set via opaque value if desired

    std::vector<std::uint8_t> payload;
    payload.reserve(1024);

    std::uint64_t payload_off = 0;
    hdr.fields.clear();
    hdr.fields.reserve(leaves.size());

    for (const auto& kv : leaves) {
        FieldMeta meta;
        meta.name = kv.first;
        meta.compression = "none";
        meta.offset = 0;
        meta.csize = 0;
        meta.usize = 0;
        meta.crc32 = 0;

        std::vector<std::uint8_t> raw = encode_value_bytes(kv.second, meta);
        meta.usize = static_cast<std::uint64_t>(raw.size());

        if (opts.include_crc32 && !raw.empty()) {
            meta.crc32 = crc32_bytes(raw.data(), raw.size());
        }

        std::vector<std::uint8_t> stored = raw;
        if (!raw.empty()) {
            if (opts.compression == CompressionMode::Always || opts.compression == CompressionMode::Auto) {
                std::vector<std::uint8_t> comp = zlib_compress(raw, opts.zlib_level);
                if (opts.compression == CompressionMode::Always || (comp.size() < raw.size())) {
                    stored = std::move(comp);
                    meta.compression = "zlib";
                }
            }
        }

        meta.csize = static_cast<std::uint64_t>(stored.size());
        if (meta.csize == 0) {
            meta.offset = 0;
        } else {
            meta.offset = payload_off;
            payload_off += meta.csize;
            payload.insert(payload.end(), stored.begin(), stored.end());
        }

        hdr.fields.push_back(std::move(meta));
    }

    // Iteratively finalize header JSON because payload_start and file_size depend on header_len.
    hdr.payload_start = 0;
    hdr.file_size = 0;
    hdr.header_crc32_hex = "00000000";

    std::string header_json;
    std::uint32_t header_len = 0;

    for (int iter = 0; iter < 6; ++iter) {
        internal::Json j0 = header_to_json(hdr, /*crc_zeroed=*/true);
        header_json = internal::json_dump_compact(j0);
        header_len = static_cast<std::uint32_t>(header_json.size());

        std::uint64_t new_payload_start = 8ull + 4ull + static_cast<std::uint64_t>(header_len);
        std::uint64_t new_file_size = new_payload_start + static_cast<std::uint64_t>(payload.size());

        if (hdr.payload_start == new_payload_start && hdr.file_size == new_file_size) {
            break;
        }
        hdr.payload_start = new_payload_start;
        hdr.file_size = new_file_size;
    }

    // Compute header CRC on zeroed header JSON bytes and write into the actual field.
    std::uint32_t crc = crc32_str_zeroed_header(header_json);
    hdr.header_crc32_hex = upper_hex8(crc);

    // Now serialize with crc value (but CRC is still computed on zeroed header string, per spec).
    internal::Json j_final = header_to_json(hdr, /*crc_zeroed=*/false);
    std::string header_json_final = internal::json_dump_compact(j_final);
    if (header_json_final.size() != header_len) {
        // CRC string is fixed-width, but payload_start/file_size might have changed due to digits. Re-run if needed.
        hdr.header_crc32_hex = "00000000";
        // One more fixpoint.
        for (int iter = 0; iter < 6; ++iter) {
            internal::Json j0 = header_to_json(hdr, /*crc_zeroed=*/true);
            header_json = internal::json_dump_compact(j0);
            header_len = static_cast<std::uint32_t>(header_json.size());

            std::uint64_t new_payload_start = 8ull + 4ull + static_cast<std::uint64_t>(header_len);
            std::uint64_t new_file_size = new_payload_start + static_cast<std::uint64_t>(payload.size());

            if (hdr.payload_start == new_payload_start && hdr.file_size == new_file_size) {
                break;
            }
            hdr.payload_start = new_payload_start;
            hdr.file_size = new_file_size;
        }
        crc = crc32_str_zeroed_header(header_json);
        hdr.header_crc32_hex = upper_hex8(crc);
        j_final = header_to_json(hdr, /*crc_zeroed=*/false);
        header_json_final = internal::json_dump_compact(j_final);
        header_len = static_cast<std::uint32_t>(header_json_final.size());
    }

    // Write file
    std::ofstream os(file, std::ios::binary | std::ios::trunc);
    if (!os) throw GbfError(ErrorKind::Io, "failed to open for write: " + file.string());

    // 8-byte magic
    std::array<char, 8> magic{};
    std::memset(magic.data(), 0, magic.size());
    std::memcpy(magic.data(), "GREDBIN", 7); // include trailing NUL or keep 8? "GREDBIN" is 7 letters; but examples show 7? Actually GREDBIN is 7. We'll pad with NUL.
    // If you want exactly 7 chars + NUL, this is correct.
    os.write(magic.data(), magic.size());
    write_u32_le(os, header_len);
    os.write(header_json_final.data(), static_cast<std::streamsize>(header_json_final.size()));
    if (!payload.empty()) {
        os.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    if (!os) throw GbfError(ErrorKind::Io, "failed writing GBF file");
}

} // namespace gbin


#include "gbin/gbf.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <locale>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>

// FTXUI TUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#if !defined(_WIN32)
#include <unistd.h>
#endif


namespace {

struct Ansi {
    bool enabled{true};

    std::string reset() const { return enabled ? "\x1b[0m" : ""; }
    std::string dim() const { return enabled ? "\x1b[2m" : ""; }
    std::string bold() const { return enabled ? "\x1b[1m" : ""; }
    std::string red() const { return enabled ? "\x1b[31m" : ""; }
    std::string green() const { return enabled ? "\x1b[32m" : ""; }
    std::string yellow() const { return enabled ? "\x1b[33m" : ""; }
    std::string blue() const { return enabled ? "\x1b[34m" : ""; }
    std::string magenta() const { return enabled ? "\x1b[35m" : ""; }
    std::string cyan() const { return enabled ? "\x1b[36m" : ""; }
    std::string gray() const { return enabled ? "\x1b[90m" : ""; }
};

static bool is_tty() {
#if defined(_WIN32)
    return false;
#else
    return ::isatty(fileno(stdout));
#endif
}

static std::string fmt_shape_u64(const std::vector<std::uint64_t>& shape) {
    if (shape.empty()) return "[?]";
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i) oss << " x ";
        oss << shape[i];
    }
    oss << ']';
    return oss.str();
}

static std::string fmt_shape(const std::vector<std::size_t>& shape) {
    if (shape.empty()) return "[?]";
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i) oss << " x ";
        oss << shape[i];
    }
    oss << ']';
    return oss.str();
}

static std::string hex8(std::uint32_t v) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

static std::string utf16_to_utf8_lossy(const std::vector<std::uint16_t>& cu) {
    std::string out;
    out.reserve(cu.size());
    std::size_t i = 0;
    while (i < cu.size()) {
        std::uint32_t cp = cu[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF && i < cu.size()) {
            std::uint32_t lo = cu[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                ++i;
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
            }
        }
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

static void usage() {
    std::cerr <<
        "gbin (C++) - GBF/GREDBIN inspector\n"
        "\n"
        "Usage:\n"
        "  gbin header <FILE> [--raw] [--validate] [--no-color]\n"
        "  gbin tree  <FILE> [--prefix <P>] [--max-depth N] [--details] [--validate] [--no-color]\n"
        "  gbin show  <FILE> [<VAR>] [--max-elems N] [--rows N] [--cols N] [--stats] [--validate] [--no-color]\n";
}

struct Args {
    std::string cmd;
    std::string file;
    std::string var;
    bool raw{false};
    bool validate{false};
    bool details{false};
    bool stats{false};
    bool no_color{false};
    std::string prefix;
    std::size_t max_depth{static_cast<std::size_t>(-1)};
    std::size_t max_elems{20};
    std::size_t rows{6};
    std::size_t cols{6};
};

static bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 3) return false;
    a.cmd = argv[1];
    a.file = argv[2];

    int i = 3;
    // positional var for show
    if (a.cmd == "show" && i < argc && std::string(argv[i]).rfind("--", 0) != 0) {
        a.var = argv[i++];
    }

    while (i < argc) {
        std::string opt = argv[i++];
        if (opt == "--raw") a.raw = true;
        else if (opt == "--validate") a.validate = true;
        else if (opt == "--details") a.details = true;
        else if (opt == "--stats") a.stats = true;
        else if (opt == "--no-color") a.no_color = true;
        else if (opt == "--prefix" && i < argc) a.prefix = argv[i++];
        else if (opt == "--max-depth" && i < argc) a.max_depth = static_cast<std::size_t>(std::stoull(argv[i++]));
        else if (opt == "--max-elems" && i < argc) a.max_elems = static_cast<std::size_t>(std::stoull(argv[i++]));
        else if (opt == "--rows" && i < argc) a.rows = static_cast<std::size_t>(std::stoull(argv[i++]));
        else if (opt == "--cols" && i < argc) a.cols = static_cast<std::size_t>(std::stoull(argv[i++]));
        else {
            std::cerr << "Unknown option: " << opt << "\n";
            return false;
        }
    }

    if (a.cmd != "header" && a.cmd != "tree" && a.cmd != "show") {
        std::cerr << "Unknown command: " << a.cmd << "\n";
        return false;
    }
    return true;
}

// ----------------- Tree printer -----------------

struct TreeNode {
    std::map<std::string, TreeNode> children;
    const gbin::FieldMeta* leaf{nullptr};
};

static void tree_insert(TreeNode& root, const std::string& path, const gbin::FieldMeta* leaf) {
    TreeNode* cur = &root;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto dot = path.find('.', start);
        if (dot == std::string::npos) dot = path.size();
        std::string part = path.substr(start, dot - start);
        if (!part.empty()) {
            cur = &cur->children[part];
        }
        if (dot == path.size()) break;
        start = dot + 1;
    }
    cur->leaf = leaf;
}

static const TreeNode* tree_find(const TreeNode& root, const std::string& prefix) {
    const TreeNode* cur = &root;
    std::size_t start = 0;
    while (start <= prefix.size()) {
        auto dot = prefix.find('.', start);
        if (dot == std::string::npos) dot = prefix.size();
        std::string part = prefix.substr(start, dot - start);
        if (!part.empty()) {
            auto it = cur->children.find(part);
            if (it == cur->children.end()) return nullptr;
            cur = &it->second;
        }
        if (dot == prefix.size()) break;
        start = dot + 1;
    }
    return cur;
}

static void print_tree(
    const TreeNode& node,
    const Ansi& ansi,
    std::size_t indent,
    std::size_t depth,
    std::size_t max_depth,
    bool details
) {
    if (depth > max_depth) return;
    for (const auto& kv : node.children) {
        const std::string& name = kv.first;
        const TreeNode& child = kv.second;

        std::string pad(indent, ' ');
        bool is_dir = !child.children.empty();
        bool is_leaf = (child.leaf != nullptr);

        if (is_leaf) {
            const auto& f = *child.leaf;
            std::string shape = fmt_shape_u64(f.shape);

            // Leaf label: show dimension + class.
            std::cout << pad
                      << ansi.cyan() << name << ansi.reset()
                      << " " << ansi.gray() << shape << ansi.reset()
                      << " " << ansi.yellow() << f.class_name << ansi.reset();

            if (details) {
                std::cout << " " << ansi.dim()
                          << "kind=" << f.kind
                          << " complex=" << (f.complex ? "true" : "false")
                          << " comp=" << f.compression
                          << " off=" << f.offset
                          << " csize=" << f.csize
                          << " usize=" << f.usize
                          << " crc32=" << hex8(f.crc32)
                          << ansi.reset();
                if (!f.encoding.empty()) {
                    std::cout << " " << ansi.dim() << "enc=" << f.encoding << ansi.reset();
                }
            }
            std::cout << "\n";
        }

        if (is_dir) {
            std::cout << pad
                      << ansi.magenta() << name << "/" << ansi.reset()
                      << "\n";
            print_tree(child, ansi, indent + 2, depth + 1, max_depth, details);
        }
    }
}


// ----------------- Interactive UI tree (FTXUI) -----------------

struct UiNode {
    std::string name;
    std::string full_path; // dot-separated; empty for root
    std::map<std::string, UiNode> children;
    const gbin::FieldMeta* leaf{nullptr};
};

struct UiRow {
    const UiNode* node{nullptr};
    int depth{0};
    bool is_dir{false};
    bool is_leaf{false};
};

static void ui_insert(UiNode& root, const std::string& path, const gbin::FieldMeta* leaf) {
    UiNode* cur = &root;
    std::size_t start = 0;
    std::string prefix;

    while (start <= path.size()) {
        auto dot = path.find('.', start);
        if (dot == std::string::npos) dot = path.size();
        std::string part = path.substr(start, dot - start);
        if (!part.empty()) {
            if (!prefix.empty()) prefix += ".";
            prefix += part;
            UiNode& next = cur->children[part];
            if (next.name.empty()) next.name = part;
            if (next.full_path.empty()) next.full_path = prefix;
            cur = &next;
        }
        if (dot == path.size()) break;
        start = dot + 1;
    }
    cur->leaf = leaf;
}

static const UiNode* ui_find(const UiNode& root, const std::string& prefix) {
    const UiNode* cur = &root;
    std::size_t start = 0;
    while (start <= prefix.size()) {
        auto dot = prefix.find('.', start);
        if (dot == std::string::npos) dot = prefix.size();
        std::string part = prefix.substr(start, dot - start);
        if (!part.empty()) {
            auto it = cur->children.find(part);
            if (it == cur->children.end()) return nullptr;
            cur = &it->second;
        }
        if (dot == prefix.size()) break;
        start = dot + 1;
    }
    return cur;
}

static void flatten_rows(const UiNode& node,
                         const std::set<std::string>& expanded,
                         int depth,
                         std::vector<UiRow>& out) {
    // Root itself is not rendered; render its children.
    for (const auto& kv : node.children) {
        const UiNode& child = kv.second;
        bool is_dir = !child.children.empty();
        bool is_leaf = (child.leaf != nullptr);
        out.push_back(UiRow{&child, depth, is_dir, is_leaf});

        if (is_dir) {
            // Expand rule: if full_path is expanded OR it is the implicit root (empty)
            if (expanded.find(child.full_path) != expanded.end()) {
                flatten_rows(child, expanded, depth + 1, out);
            }
        }
    }
}

// Forward declarations (used by the FTXUI preview bridge)
static void print_value_preview(const gbin::GbfValue& v, std::size_t max_elems, std::size_t rows, std::size_t cols);
static void print_numeric_preview(const gbin::NumericArray& a, std::size_t max_elems, std::size_t rows, std::size_t cols);

static std::string preview_to_string(const gbin::GbfValue& v, std::size_t max_elems, std::size_t rows, std::size_t cols) {
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    // Reuse the existing preview printer to keep behavior consistent with non-TUI show.
    print_value_preview(v, max_elems, rows, cols);
    std::cout.rdbuf(old);
    return oss.str();
}

// --- Preview rendering helpers for FTXUI ---
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(128);
    for (char ch : s) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static ftxui::Element render_preview_colored(const std::string& preview) {
    using namespace ftxui;

    auto lines = split_lines(preview);
    if (lines.empty()) {
        return text("(no preview)") | color(Color::GrayDark);
    }

    std::vector<Element> els;
    els.reserve(lines.size());

    for (const auto& line : lines) {
        if (line.empty()) {
            els.push_back(text(""));
            continue;
        }

        // Section headers like "numeric:" / "preview:" / "struct:" etc.
        if (!line.empty() && line.back() == ':' && (line.size() < 40) && (line.rfind("  ", 0) != 0)) {
            els.push_back(text(line) | bold | color(Color::Magenta));
            continue;
        }

        // Indented key/value like "  class=double" or "  shape=[1 x 4]"
        if (line.rfind("  ", 0) == 0) {
            std::string rest = line.substr(2);
            auto eq = rest.find('=');
            if (eq != std::string::npos) {
                std::string k = rest.substr(0, eq);
                std::string v = rest.substr(eq + 1);
                els.push_back(hbox({
                    text("  "),
                    text(k) | bold | color(Color::Yellow),
                    text("=") | color(Color::GrayDark),
                    text(v) | color(Color::GrayLight) | flex,
                }));
                continue;
            }

            // Indented string literal lines like:  "hello"
            if (!rest.empty() && rest.front() == '"') {
                els.push_back(hbox({
                    text("  "),
                    text(rest) | color(Color::Green),
                }));
                continue;
            }

            els.push_back(text(line) | color(Color::White));
            continue;
        }

        // One-liners like: "datetime: shape=[...] numel=..."
        auto colon = line.find(':');
        if (colon != std::string::npos && colon < 32) {
            std::string head = line.substr(0, colon + 1);
            std::string tail = line.substr(colon + 1);
            els.push_back(hbox({
                text(head) | bold | color(Color::Magenta),
                text(tail) | color(Color::White) | flex,
            }));
            continue;
        }

        els.push_back(text(line) | color(Color::White));
    }

    return vbox(std::move(els));
}

static const UiRow* safe_row_at(const std::vector<UiRow>& rows, int idx) {
    if (rows.empty()) return nullptr;
    if (idx < 0) return nullptr;
    if ((std::size_t)idx >= rows.size()) return nullptr;
    return &rows[(std::size_t)idx];
}

// ----------------- End UI tree helpers -----------------


// ----------------- Value preview -----------------

template <typename T>
static std::string scalar_to_string(const std::uint8_t* p, std::size_t nbytes) {
    // fallback hex
    std::ostringstream oss;
    oss << "0x";
    for (std::size_t i = 0; i < nbytes; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)p[i];
    }
    return oss.str();
}

static void print_numeric_preview(const gbin::NumericArray& a, std::size_t max_elems, std::size_t rows, std::size_t cols) {
    std::size_t n = gbin::numel(a.shape);
    std::size_t elem = 1;
    switch (a.class_id) {
        case gbin::NumericClass::Double: elem = 8; break;
        case gbin::NumericClass::Single: elem = 4; break;
        case gbin::NumericClass::Int8: elem = 1; break;
        case gbin::NumericClass::UInt8: elem = 1; break;
        case gbin::NumericClass::Int16: elem = 2; break;
        case gbin::NumericClass::UInt16: elem = 2; break;
        case gbin::NumericClass::Int32: elem = 4; break;
        case gbin::NumericClass::UInt32: elem = 4; break;
        case gbin::NumericClass::Int64: elem = 8; break;
        case gbin::NumericClass::UInt64: elem = 8; break;
        default: elem = 1;
    }

    std::cout << "numeric:\n";
    std::cout << "  class=" << gbin::to_string(a.class_id) << "\n";
    std::cout << "  complex=" << (a.complex ? "true" : "false") << "\n";
    std::cout << "  shape=" << fmt_shape(a.shape) << "\n";
    std::cout << "  numel=" << n << "\n";
    std::cout << "  bytes(real)=" << a.real_le.size() << "\n";

    auto decode = [&](std::size_t idx) -> std::string {
        std::size_t off = idx * elem;
        if (off + elem > a.real_le.size()) return "?";
        const std::uint8_t* p = &a.real_le[off];

        std::ostringstream oss;
        oss.imbue(std::locale::classic());

        switch (a.class_id) {
            case gbin::NumericClass::Double: {
                double v;
                std::memcpy(&v, p, 8);
                oss << v;
                return oss.str();
            }
            case gbin::NumericClass::Single: {
                float v;
                std::memcpy(&v, p, 4);
                oss << v;
                return oss.str();
            }
            case gbin::NumericClass::Int32: {
                std::int32_t v;
                std::memcpy(&v, p, 4);
                oss << v;
                return oss.str();
            }
            case gbin::NumericClass::UInt64: {
                std::uint64_t v;
                std::memcpy(&v, p, 8);
                oss << v;
                return oss.str();
            }
            default:
                return scalar_to_string<int>(p, elem);
        }
    };

    if (a.shape.size() == 2 && !a.complex) {
        std::size_t r_total = a.shape[0];
        std::size_t c_total = a.shape[1];
        std::size_t r_show = std::min(rows, r_total);
        std::size_t c_show = std::min(cols, c_total);

        std::cout << "preview:\n";
        std::cout << "  top-left " << r_show << "x" << c_show << ":\n";
        for (std::size_t r = 0; r < r_show; ++r) {
            std::cout << "  ";
            for (std::size_t c = 0; c < c_show; ++c) {
                std::size_t idx = r + c * r_total; // column-major
                std::cout << decode(idx);
                if (c + 1 < c_show) std::cout << "  ";
            }
            std::cout << "\n";
        }
    } else {
        std::size_t show = std::min(max_elems, n);
        std::cout << "preview:\n";
        std::cout << "  first " << show << ":\n";
        std::cout << "  ";
        for (std::size_t i = 0; i < show; ++i) {
            std::cout << decode(i) << " ";
        }
        std::cout << "\n";
    }
}

static void print_value_preview(const gbin::GbfValue& v, std::size_t max_elems, std::size_t rows, std::size_t cols) {
    if (std::holds_alternative<gbin::GbfValue::Struct>(v.v)) {
        const auto& m = std::get<gbin::GbfValue::Struct>(v.v);
        std::cout << "struct:\n";
        std::cout << "  fields=" << m.size() << "\n";
        std::cout << "preview:\n";
        std::vector<std::string> keys;
        keys.reserve(m.size());
        for (const auto& kv : m) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) std::cout << "  " << k << "\n";
        return;
    }

    if (std::holds_alternative<gbin::NumericArray>(v.v)) {
        print_numeric_preview(std::get<gbin::NumericArray>(v.v), max_elems, rows, cols);
        return;
    }

    if (std::holds_alternative<gbin::LogicalArray>(v.v)) {
        const auto& a = std::get<gbin::LogicalArray>(v.v);
        std::size_t n = gbin::numel(a.shape);
        std::cout << "logical:\n";
        std::cout << "  shape=" << fmt_shape(a.shape) << "\n";
        std::cout << "  numel=" << n << "\n";
        std::size_t show = std::min(max_elems, a.data.size());
        std::cout << "preview (first " << show << "): ";
        for (std::size_t i = 0; i < show; ++i) std::cout << (a.data[i] ? "true" : "false") << " ";
        std::cout << "\n";
        return;
    }

    if (std::holds_alternative<gbin::StringArray>(v.v)) {
        const auto& a = std::get<gbin::StringArray>(v.v);
        std::size_t n = gbin::numel(a.shape);
        std::cout << "string:\n";
        std::cout << "  shape=" << fmt_shape(a.shape) << "\n";
        std::cout << "  numel=" << n << "\n";
        std::cout << "preview:\n";
        std::size_t show = std::min(max_elems, a.data.size());
        for (std::size_t i = 0; i < show; ++i) {
            if (!a.data[i].has_value()) std::cout << "  [" << i << "] <missing>\n";
            else std::cout << "  [" << i << "] \"" << *a.data[i] << "\"\n";
        }
        return;
    }

    if (std::holds_alternative<gbin::CharArray>(v.v)) {
        const auto& a = std::get<gbin::CharArray>(v.v);
        std::cout << "char:\n";
        std::cout << "  shape=" << fmt_shape(a.shape) << "\n";
        std::cout << "  numel=" << gbin::numel(a.shape) << "\n";
        std::string s = utf16_to_utf8_lossy(a.utf16);
        std::cout << "preview:\n";
        std::cout << "  \"" << s << "\"\n";
        return;
    }

    if (std::holds_alternative<gbin::DateTimeArray>(v.v)) {
        const auto& a = std::get<gbin::DateTimeArray>(v.v);
        std::size_t n = gbin::numel(a.shape);
        std::cout << "datetime: shape=" << fmt_shape(a.shape) << " numel=" << n
                  << " tz=\"" << a.timezone << "\" format=\"" << a.format << "\"\n";
        std::size_t show = std::min(max_elems, n);
        for (std::size_t i = 0; i < show; ++i) {
            if (i < a.nat_mask.size() && a.nat_mask[i]) std::cout << "  [" << i << "] NaT\n";
            else if (i < a.unix_ms.size()) std::cout << "  [" << i << "] unix_ms=" << a.unix_ms[i] << "\n";
        }
        return;
    }

    if (std::holds_alternative<gbin::DurationArray>(v.v)) {
        const auto& a = std::get<gbin::DurationArray>(v.v);
        std::size_t n = gbin::numel(a.shape);
        std::cout << "duration: shape=" << fmt_shape(a.shape) << " numel=" << n << "\n";
        std::size_t show = std::min(max_elems, n);
        for (std::size_t i = 0; i < show; ++i) {
            if (i < a.nan_mask.size() && a.nan_mask[i]) std::cout << "  [" << i << "] NaN\n";
            else if (i < a.ms.size()) std::cout << "  [" << i << "] ms=" << a.ms[i] << "\n";
        }
        return;
    }

    if (std::holds_alternative<gbin::CalendarDurationArray>(v.v)) {
        const auto& a = std::get<gbin::CalendarDurationArray>(v.v);
        std::size_t n = gbin::numel(a.shape);
        std::cout << "calendarDuration: shape=" << fmt_shape(a.shape) << " numel=" << n << "\n";
        std::size_t show = std::min(max_elems, n);
        for (std::size_t i = 0; i < show; ++i) {
            std::cout << "  [" << i << "] months=" << (i < a.months.size() ? a.months[i] : 0)
                      << " days=" << (i < a.days.size() ? a.days[i] : 0)
                      << " time_ms=" << (i < a.time_ms.size() ? a.time_ms[i] : 0)
                      << " mask=" << (i < a.mask.size() ? (int)a.mask[i] : 0)
                      << "\n";
        }
        return;
    }

    if (std::holds_alternative<gbin::CategoricalArray>(v.v)) {
        const auto& a = std::get<gbin::CategoricalArray>(v.v);
        std::size_t n = gbin::numel(a.shape);
        std::cout << "categorical: shape=" << fmt_shape(a.shape)
                  << " categories=" << a.categories.size()
                  << " codes=" << a.codes.size() << "\n";
        std::size_t show = std::min(max_elems, n);
        for (std::size_t i = 0; i < show; ++i) {
            std::uint32_t code = (i < a.codes.size()) ? a.codes[i] : 0;
            if (code == 0) {
                std::cout << "  [" << i << "] <undefined>\n";
            } else {
                std::size_t idx = static_cast<std::size_t>(code - 1);
                std::string label = (idx < a.categories.size()) ? a.categories[idx] : "<?>";
                std::cout << "  [" << i << "] " << code << " => " << label << "\n";
            }
        }
        return;
    }

    if (std::holds_alternative<gbin::OpaqueValue>(v.v)) {
        const auto& a = std::get<gbin::OpaqueValue>(v.v);
        std::cout << "opaque: kind=" << a.kind << " class=" << a.class_name
                  << " shape=" << fmt_shape(a.shape)
                  << " bytes=" << a.bytes.size()
                  << " encoding=" << a.encoding << "\n";
        return;
    }

    std::cout << "<unhandled>\n";
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, a)) {
        usage();
        return 2;
    }

    Ansi ansi;
    ansi.enabled = !a.no_color && is_tty();

    try {
        if (a.cmd == "header") {
            auto [hdr, header_len, raw_json] = gbin::read_header_only(a.file, gbin::ReadOptions{a.validate});

            std::cout << ansi.bold() << "File" << ansi.reset() << ": " << a.file << "\n";
            std::cout << ansi.bold() << "Magic" << ansi.reset() << ": " << hdr.magic << "\n";
            std::cout << ansi.bold() << "Header len" << ansi.reset() << ": " << header_len << " bytes\n";
            std::cout << ansi.bold() << "Payload start" << ansi.reset() << ": " << hdr.payload_start << "\n";
            std::cout << ansi.bold() << "File size" << ansi.reset() << ": " << hdr.file_size << "\n";
            std::cout << ansi.bold() << "Header CRC" << ansi.reset() << ": " << hdr.header_crc32_hex << "\n";

            if (a.raw) {
                std::cout << raw_json << "\n";
            } else {
                std::cout << ansi.dim() << "(use --raw to print raw header JSON)\n" << ansi.reset();
            }
            return 0;
        }

        if (a.cmd == "tree") {
            auto [hdr, header_len, raw_json] = gbin::read_header_only(a.file, gbin::ReadOptions{a.validate});

            TreeNode root;
            for (const auto& f : hdr.fields) {
                tree_insert(root, f.name, &f);
            }

            const TreeNode* node = &root;
            if (!a.prefix.empty()) {
                node = tree_find(root, a.prefix);
                if (!node) {
                    std::cerr << "prefix not found: " << a.prefix << "\n";
                    return 2;
                }
                std::cout << ansi.dim() << "prefix: " << a.prefix << ansi.reset() << "\n";
            }

            std::cout << ansi.bold() << "GBF variable tree" << ansi.reset() << ": " << a.file << "\n";
            print_tree(*node, ansi, 0, 0, a.max_depth, a.details);
            return 0;
        }

        if (a.cmd == "show") {
            // Interactive TUI browser (Rust-like).
            auto [hdr, header_len, raw_json] = gbin::read_header_only(a.file, gbin::ReadOptions{a.validate});

            UiNode root;
            root.name = "<root>";
            root.full_path.clear();
            for (const auto& f : hdr.fields) {
                ui_insert(root, f.name, &f);
            }

            const UiNode* start = &root;
            if (!a.var.empty() && a.var != "<root>") {
                const UiNode* found = ui_find(root, a.var);
                if (!found) {
                    std::cerr << ansi.red() << "Error" << ansi.reset() << ": prefix not found: " << a.var << "\n";
                    return 2;
                }
                start = found;
            }

            using namespace ftxui;

            std::set<std::string> expanded;
            // Expand the start node to show its children.
            if (!start->full_path.empty()) expanded.insert(start->full_path);

            int selected = 0;
            int left_scroll = 0; // first visible row index in left pane

            struct StatusKV {
                std::string k;
                std::string v;
            };
            std::vector<StatusKV> status_kv;

            std::string preview;
            std::string selected_path;

            auto rebuild = [&]() -> std::vector<UiRow> {
                std::vector<UiRow> out;
                flatten_rows(*start, expanded, 0, out);
                if (out.empty()) {
                    selected = 0;
                } else {
                    if (selected < 0) selected = 0;
                    if (selected >= (int)out.size()) selected = (int)out.size() - 1;
                }
                return out;
            };

            auto rows = rebuild();

            auto load_preview_for_selected = [&]() {
                if (rows.empty()) return;
                const UiRow* pr = safe_row_at(rows, selected);
                if (!pr) return;
                const UiRow& r = *pr;
                if (!r.node) return;
                selected_path = r.node->full_path;

                // For non-leaf, show metadata only.
                if (!r.is_leaf) {
                    preview.clear();
                    status_kv.clear();
                    status_kv.push_back({"type", "node"});
                    return;
                }

                gbin::ReadOptions ro;
                ro.validate = a.validate;

                try {
                    // Read leaf value on-demand.
                    gbin::GbfValue v = gbin::read_var(a.file, selected_path, ro);
                    preview = preview_to_string(v, a.max_elems, a.rows, a.cols);

                    status_kv.clear();
                    if (r.node->leaf) {
                        const auto& f = *r.node->leaf;
                        status_kv.push_back({"kind", f.kind});
                        status_kv.push_back({"class", f.class_name});
                        status_kv.push_back({"shape", fmt_shape_u64(f.shape)});
                        status_kv.push_back({"complex", (f.complex ? "true" : "false")});
                        status_kv.push_back({"comp", f.compression});
                        status_kv.push_back({"off", std::to_string(f.offset)});
                        status_kv.push_back({"csize", std::to_string(f.csize)});
                        status_kv.push_back({"usize", std::to_string(f.usize)});
                        status_kv.push_back({"crc32", hex8(f.crc32)});
                        if (!f.encoding.empty()) {
                            status_kv.push_back({"encoding", f.encoding});
                        }
                    }
                } catch (const std::exception& e) {
                    preview.clear();
                    status_kv.clear();
                    status_kv.push_back({"error", e.what()});
                }
            };

            load_preview_for_selected();

            auto left_pane = Renderer([&] {
                rows = rebuild();

                // --- Scrollable list viewport ---
                // The UI height is clamped to the terminal. The left pane must therefore
                // scroll inside its own viewport (and never grow the overall DOM).
                auto dim = ftxui::Terminal::Size();
                int term_h = std::max(10, dim.dimy);

                // header (1) + separator (1) + border (2) + a bit of margin
                int visible_rows = std::max(3, term_h - 6);

                // Clamp scroll and keep selection visible.
                int total = (int)rows.size();
                if (total <= 0) {
                    left_scroll = 0;
                } else {
                    left_scroll = std::max(0, std::min(left_scroll, std::max(0, total - visible_rows)));
                    if (selected < left_scroll) left_scroll = selected;
                    if (selected >= left_scroll + visible_rows) left_scroll = selected - visible_rows + 1;
                    left_scroll = std::max(0, std::min(left_scroll, std::max(0, total - visible_rows)));
                }

                int begin = left_scroll;
                int end = std::min(total, begin + visible_rows);

                std::vector<Element> items;
                items.reserve((std::size_t)std::max(0, end - begin) + 2);

                constexpr int kLeftLineMax = 58; // pane width is 60, leave room for borders

                if (begin > 0) {
                    items.push_back(text("↑ more") | color(Color::GrayDark));
                }

                for (int i = begin; i < end; ++i) {
                    const UiRow& r = rows[(std::size_t)i];
                    const UiNode* n = r.node;
                    std::string name = n ? n->name : "";

                    std::string glyph = "  ";
                    if (r.is_dir && n) glyph = (expanded.find(n->full_path) != expanded.end()) ? "▾ " : "▸ ";
                    else glyph = "• ";

                    std::string indent((std::size_t)r.depth * 2, ' ');

                    std::string meta;
                    if (r.is_leaf && n && n->leaf) {
                        meta = fmt_shape_u64(n->leaf->shape) + "  " + n->leaf->class_name;
                    }

                    Element left_txt = text(indent + glyph + name) | color(Color::Cyan) | flex;
                    Element right_txt = text(meta) | color(Color::Yellow);
                    Element line = hbox({ left_txt, right_txt }) | size(WIDTH, LESS_THAN, kLeftLineMax);

                    if (i == selected) {
                        line = line | inverted;
                    }
                    items.push_back(line);
                }

                if (end < total) {
                    items.push_back(text("↓ more") | color(Color::GrayDark));
                }

                auto header = hbox({
                    text("GBF") | bold | color(Color::White),
                    text("  "),
                    text(a.file) | color(Color::GrayDark),
                    filler(),
                    text("q") | bold | color(Color::Yellow),
                    text(" quit  ") | color(Color::GrayDark),
                    text("←→") | bold | color(Color::Yellow),
                    text(" collapse/expand  ") | color(Color::GrayDark),
                    text("↑↓") | bold | color(Color::Yellow),
                    text(" move  ") | color(Color::GrayDark),
                    text("PgUp/PgDn") | bold | color(Color::Yellow),
                    text(" page  ") | color(Color::GrayDark),
                    text("Wheel") | bold | color(Color::Yellow),
                    text(" scroll  ") | color(Color::GrayDark),
                    text("Enter") | bold | color(Color::Yellow),
                    text(" preview") | color(Color::GrayDark)
                });

                auto list = vbox(std::move(items)) | flex;

                return vbox({
                           header,
                           separator(),
                           list,
                       }) |
                       flex |
                       border;
            });

            auto right_pane = Renderer([&] {
                std::vector<Element> meta_lines;
                meta_lines.reserve(status_kv.size() + 1);
                for (const auto& kv : status_kv) {
                    meta_lines.push_back(
                        hbox({
                            text(kv.k) | bold | color(Color::Yellow),
                            text(": ") | color(Color::GrayDark),
                            text(kv.v) | color(Color::GrayLight) | flex,
                        })
                    );
                }
                if (meta_lines.empty()) {
                    meta_lines.push_back(text("(no metadata)") | color(Color::GrayDark));
                }

                Element top = vbox({
                    text(selected_path.empty() ? "<root>" : selected_path) | bold | color(Color::Green),
                    separator(),
                    vbox(std::move(meta_lines)) | flex,
                }) | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 10) | flex;

                Element body = vbox({
                    text("preview") | bold | color(Color::Magenta),
                    separator(),
                    render_preview_colored(preview) | flex,
                }) | vscroll_indicator | frame | flex;

                return vbox({top, separator(), body}) |
                       flex |
                       border;
            });

            auto layout = Renderer([&] {
                // Hard-clamp left pane width and rely on per-row truncation.
                // This prevents the right pane from disappearing when many nodes are expanded.
                int left_w = 60;

                // Clamp the whole UI to the terminal viewport. Without this, DOM elements like
                // `paragraph()` may expand the document height and "push" panes off-screen.
                auto dim = ftxui::Terminal::Size();
                int term_w = std::max(20, dim.dimx);
                int term_h = std::max(10, dim.dimy);

                auto ui = hbox({
                    left_pane->Render() | size(WIDTH, EQUAL, left_w),
                    right_pane->Render() | flex,
                }) | flex;

                // Force the root document to be exactly the viewport size. Internal panes use
                // `frame | vscroll_indicator` so they scroll inside their borders instead.
                return ui
                    | size(WIDTH, EQUAL, term_w)
                    | size(HEIGHT, EQUAL, term_h);
            });

            auto screen = ScreenInteractive::TerminalOutput();
            // Enable mouse support (wheel scrolling, clicks) when available.
            // FTXUI exposes this via TrackMouse() in current releases.
            screen.TrackMouse(true);

            auto app = CatchEvent(layout, [&](Event e) {
                rows = rebuild();

                if (e == Event::Character('q') || e == Event::Escape) {
                    screen.Exit();
                    return true;
                }
                if (rows.empty()) return false;

                const UiRow* pr = safe_row_at(rows, selected);
                if (!pr) return false;
                const UiRow& r = *pr;
                const UiNode* n = r.node;

                if (e == Event::ArrowUp) {
                    if (selected > 0) selected--;
                    return true;
                }
                if (e == Event::ArrowDown) {
                    if (selected + 1 < (int)rows.size()) selected++;
                    return true;
                }
                if (e == Event::PageUp) {
                    selected = std::max(0, selected - 25);
                    return true;
                }
                if (e == Event::PageDown) {
                    selected = std::min((int)rows.size() - 1, selected + 25);
                    return true;
                }

                // Mouse wheel scrolling
                if (e.is_mouse()) {
                    auto m = e.mouse();
                    if (m.button == Mouse::WheelUp) {
                        selected = std::max(0, selected - 3);
                        return true;
                    }
                    if (m.button == Mouse::WheelDown) {
                        selected = std::min((int)rows.size() - 1, selected + 3);
                        return true;
                    }
                }

                if (e == Event::ArrowRight) {
                    if (r.is_dir && n) {
                        expanded.insert(n->full_path);
                        rows = rebuild();
                    }
                    return true;
                }
                if (e == Event::ArrowLeft) {
                    if (r.is_dir && n) {
                        expanded.erase(n->full_path);
                        rows = rebuild();
                    }
                    return true;
                }
                if (e == Event::Return) {
                    load_preview_for_selected();
                    return true;
                }

                return false;
            });

            screen.Loop(app);
            return 0;
        }

    } catch (const gbin::GbfError& e) {
        std::cerr << ansi.red() << "Error" << ansi.reset() << ": " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << ansi.red() << "Error" << ansi.reset() << ": " << e.what() << "\n";
        return 1;
    }

    usage();
    return 2;
}

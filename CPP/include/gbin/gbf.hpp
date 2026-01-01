
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace gbin {

// ------------------------------
// Error model
// ------------------------------

enum class ErrorKind {
    Io,
    BadMagic,
    HeaderJsonParse,
    HeaderCrcMismatch,
    FieldCrcMismatch,
    ZlibError,
    Truncated,
    NotFound,
    Unsupported,
    InvalidData,
};

class GbfError : public std::runtime_error {
public:
    GbfError(ErrorKind k, const std::string& msg);
    ErrorKind kind() const noexcept;

private:
    ErrorKind kind_;
};

// ------------------------------
// Public data model
// ------------------------------

enum class NumericClass {
    Double,
    Single,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Unknown,
};

std::string to_string(NumericClass c);
NumericClass numeric_class_from_string(const std::string& s);

struct NumericArray {
    NumericClass class_id{NumericClass::Unknown};
    std::vector<std::size_t> shape{};
    bool complex{false};

    // Little-endian element bytes in MATLAB column-major order.
    std::vector<std::uint8_t> real_le{};
    // If complex=true, imag bytes are appended on disk after real bytes.
    std::optional<std::vector<std::uint8_t>> imag_le{};
};

struct LogicalArray {
    std::vector<std::size_t> shape{};
    // 0/1 bytes, length = numel(shape).
    std::vector<std::uint8_t> data{};
};

struct StringArray {
    std::vector<std::size_t> shape{};
    // Column-major order, length = numel(shape).
    std::vector<std::optional<std::string>> data{};
};

struct CharArray {
    std::vector<std::size_t> shape{};
    // UTF-16 code units, little-endian on disk. Length = numel(shape).
    std::vector<std::uint16_t> utf16{};
};

struct DateTimeArray {
    std::vector<std::size_t> shape{};
    std::string timezone{}; // empty => "naive"
    std::string locale{};   // may be empty
    std::string format{};   // may be empty
    std::vector<std::uint8_t> nat_mask{}; // 0/1 bytes, length = numel(shape)
    std::vector<std::int64_t> unix_ms{};  // milliseconds since Unix epoch (UTC), length = numel(shape)
};

struct DurationArray {
    std::vector<std::size_t> shape{};
    std::vector<std::uint8_t> nan_mask{}; // 0/1 bytes, length = numel(shape)
    std::vector<std::int64_t> ms{};       // milliseconds, length = numel(shape)
};

struct CalendarDurationArray {
    std::vector<std::size_t> shape{};
    std::vector<std::uint8_t> mask{};   // per-element mask, length = numel(shape)
    std::vector<std::int32_t> months{}; // length = numel(shape)
    std::vector<std::int32_t> days{};   // length = numel(shape)
    std::vector<std::int64_t> time_ms{}; // length = numel(shape)
};

struct CategoricalArray {
    std::vector<std::size_t> shape{};
    std::vector<std::string> categories{};
    // Codes are 1-based indices into categories; 0 means <undefined>.
    std::vector<std::uint32_t> codes{};
};

struct OpaqueValue {
    std::string kind{};
    std::string class_name{};
    std::vector<std::size_t> shape{};
    bool complex{false};
    std::string encoding{};
    // Uncompressed payload bytes (after decompression if any).
    std::vector<std::uint8_t> bytes{};
};

struct GbfValue {
    using Struct = std::map<std::string, GbfValue>;

    std::variant<
        Struct,
        NumericArray,
        LogicalArray,
        StringArray,
        CharArray,
        DateTimeArray,
        DurationArray,
        CalendarDurationArray,
        CategoricalArray,
        OpaqueValue
    > v;

    // Convenience constructors
    static GbfValue make_struct();
    static GbfValue make_struct(const Struct& m);

    static GbfValue make_numeric(const NumericArray& a);
    static GbfValue make_logical(const LogicalArray& a);
    static GbfValue make_string(const StringArray& a);
    static GbfValue make_char(const CharArray& a);
    static GbfValue make_datetime(const DateTimeArray& a);
    static GbfValue make_duration(const DurationArray& a);
    static GbfValue make_calendarduration(const CalendarDurationArray& a);
    static GbfValue make_categorical(const CategoricalArray& a);
    static GbfValue make_opaque(const OpaqueValue& a);

    bool is_struct() const noexcept;
    const Struct& as_struct() const;
    Struct& as_struct();
};

// ------------------------------
// Header model
// ------------------------------

struct FieldMeta {
    std::string name{};
    std::string kind{};
    std::string class_name{};
    std::vector<std::uint64_t> shape{};
    bool complex{false};
    std::string encoding{};
    std::string compression{"none"}; // "none" | "zlib"
    std::uint64_t offset{0}; // relative to payload_start
    std::uint64_t csize{0};
    std::uint64_t usize{0};
    std::uint32_t crc32{0};
};

struct Header {
    std::string format{"GBF"};
    std::string magic{"GREDBIN"};
    int version{1};
    std::string endianness{"little"};
    std::string order{"column-major"};
    std::string root{"struct"};
    std::string created_utc{};
    std::string matlab_version{};
    std::vector<FieldMeta> fields{};

    std::uint64_t payload_start{0};
    std::uint64_t file_size{0};
    std::string header_crc32_hex{};
};

struct ReadOptions {
    bool validate{false}; // validate header CRC + per-field CRC (when present)
};

enum class CompressionMode {
    Never,
    Always,
    Auto,
};

struct WriteOptions {
    CompressionMode compression{CompressionMode::Auto};
    bool include_crc32{true};
    int zlib_level{6}; // 0..9
};

// ------------------------------
// API
// ------------------------------

/// Read header without touching payload (fast). Returns (header, header_len, raw_json_string).
std::tuple<Header, std::uint32_t, std::string> read_header_only(
    const std::filesystem::path& file,
    const ReadOptions& opts = ReadOptions{}
);

/// Read full file and reconstruct the root value (typically a struct).
GbfValue read_file(
    const std::filesystem::path& file,
    const ReadOptions& opts = ReadOptions{}
);

/// Read a variable path (leaf or subtree prefix). `var` may be "<root>" or empty to read the root.
GbfValue read_var(
    const std::filesystem::path& file,
    const std::string& var,
    const ReadOptions& opts = ReadOptions{}
);

/// Write a GBF file from a value (typically a struct).
void write_file(
    const std::filesystem::path& file,
    const GbfValue& root,
    const WriteOptions& opts = WriteOptions{}
);

// ------------------------------
// Utilities
// ------------------------------

std::size_t numel(const std::vector<std::size_t>& shape);
std::size_t numel_u64(const std::vector<std::uint64_t>& shape);

} // namespace gbin

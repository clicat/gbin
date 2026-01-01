
#include "gbin/gbf.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>



#define CHECK(cond) do { \
    if (!(cond)) { \
        std::ostringstream _oss; \
        _oss << "CHECK failed: " #cond " at " << __FILE__ << ":" << __LINE__; \
        throw std::runtime_error(_oss.str()); \
    } \
} while (0)

static std::vector<std::uint8_t> as_bytes(const std::vector<double>& v) {
    std::vector<std::uint8_t> out(v.size() * sizeof(double));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

static std::vector<std::uint8_t> as_bytes_f32(const std::vector<float>& v) {
    std::vector<std::uint8_t> out(v.size() * sizeof(float));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

static gbin::GbfValue make_sample_root() {
    gbin::GbfValue::Struct root;

    // Numeric double 2x3: values 1..6 in column-major.
    {
        gbin::NumericArray a;
        a.class_id = gbin::NumericClass::Double;
        a.shape = {2,3};
        a.complex = false;
        std::vector<double> vals = {
            1,2, 3,4, 5,6  // column-major: [ [1 3 5]; [2 4 6] ]
        };
        a.real_le = as_bytes(vals);
        root["A"] = gbin::GbfValue::make_numeric(a);
    }

    // Logical 1x4
    {
        gbin::LogicalArray a;
        a.shape = {1,4};
        a.data = {1,0,1,1};
        root["mask"] = gbin::GbfValue::make_logical(a);
    }

    // String 2x3 with a missing
    {
        gbin::StringArray a;
        a.shape = {2,3};
        a.data = {
            std::string(""), std::string("ascii"),
            std::nullopt, std::string("€"),
            std::string("caffè"), std::string("line1\\nline2")
        };
        root["s"] = gbin::GbfValue::make_string(a);
    }

    // Char 1x4 "ABC1"
    {
        gbin::CharArray a;
        a.shape = {1,4};
        a.utf16 = {67,65,67,49}; // intentionally not "ABC1" to ensure non-trivial? Actually use "CAC1".
        root["txt"] = gbin::GbfValue::make_char(a);
    }

    // Duration 1x3: [1.5s NaN 3s]
    {
        gbin::DurationArray a;
        a.shape = {1,3};
        a.nan_mask = {0,1,0};
        a.ms = {1500,0,3000};
        root["du"] = gbin::GbfValue::make_duration(a);
    }

    // CalendarDuration 1x3
    {
        gbin::CalendarDurationArray a;
        a.shape = {1,3};
        a.mask = {0,1,0};
        a.months = {1,0,2};
        a.days = {10,0,5};
        a.time_ms = {0,0,60000};
        root["cd"] = gbin::GbfValue::make_calendarduration(a);
    }

    // Categorical 2x2
    {
        gbin::CategoricalArray a;
        a.shape = {2,2};
        a.categories = {"x","y","z"};
        // Column-major 2x2: [ [1 0]; [2 3] ] => linear [1,2,0,3]
        a.codes = {1,2,0,3};
        root["cat"] = gbin::GbfValue::make_categorical(a);
    }

    // Datetime 1x2: [unix 0, NaT]
    {
        gbin::DateTimeArray a;
        a.shape = {1,2};
        a.timezone = "UTC";
        a.locale = "";
        a.format = "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'";
        a.nat_mask = {0,1};
        a.unix_ms = {0,0};
        root["dt"] = gbin::GbfValue::make_datetime(a);
    }

    return gbin::GbfValue::make_struct(root);
}

static void flip_one_payload_byte(const std::filesystem::path& p) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(static_cast<bool>(f));
    // Read header len
    f.seekg(8, std::ios::beg);
    std::uint32_t hlen = 0;
    unsigned char b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    CHECK(static_cast<bool>(f));
    hlen = (std::uint32_t)b[0] | ((std::uint32_t)b[1]<<8) | ((std::uint32_t)b[2]<<16) | ((std::uint32_t)b[3]<<24);
    std::uint64_t payload_start = 8ull + 4ull + hlen;

    // Flip first payload byte (if any)
    f.seekg(static_cast<std::streamoff>(payload_start), std::ios::beg);
    char c;
    f.read(&c, 1);
    CHECK(static_cast<bool>(f));
    c ^= 0x01;
    f.seekp(static_cast<std::streamoff>(payload_start), std::ios::beg);
    f.write(&c, 1);
    CHECK(static_cast<bool>(f));
}

static void flip_one_header_byte(const std::filesystem::path& p) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(static_cast<bool>(f));
    // Read header len
    f.seekg(8, std::ios::beg);
    unsigned char b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    CHECK(static_cast<bool>(f));
    std::uint32_t hlen = (std::uint32_t)b[0] | ((std::uint32_t)b[1]<<8) | ((std::uint32_t)b[2]<<16) | ((std::uint32_t)b[3]<<24);

    // Flip one byte somewhere in header, but avoid the crc field itself by flipping early.
    std::uint64_t header_pos = 8ull + 4ull + 10ull; // inside JSON
    if (header_pos >= 8ull + 4ull + hlen) header_pos = 8ull + 4ull;
    f.seekg(static_cast<std::streamoff>(header_pos), std::ios::beg);
    char c;
    f.read(&c, 1);
    CHECK(static_cast<bool>(f));
    c ^= 0x01;
    f.seekp(static_cast<std::streamoff>(header_pos), std::ios::beg);
    f.write(&c, 1);
    CHECK(static_cast<bool>(f));
}

int main() {
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "gbin_cpp_test.gbf";
    std::filesystem::remove(tmp);

    gbin::GbfValue root = make_sample_root();

    // Write + read validate
    {
        gbin::WriteOptions wo;
        wo.compression = gbin::CompressionMode::Auto;
        wo.include_crc32 = true;
        wo.zlib_level = 6;

        gbin::write_file(tmp, root, wo);

        auto [hdr, hlen, raw] = gbin::read_header_only(tmp, gbin::ReadOptions{true});
        CHECK(hdr.fields.size() > 0);

        gbin::GbfValue round = gbin::read_file(tmp, gbin::ReadOptions{true});
        CHECK(std::holds_alternative<gbin::GbfValue::Struct>(round.v));
        const auto& m = std::get<gbin::GbfValue::Struct>(round.v);
        CHECK(m.find("A") != m.end());
        CHECK(m.find("s") != m.end());
    }

    // Random-access read leaf
    {
        gbin::GbfValue vA = gbin::read_var(tmp, "A", gbin::ReadOptions{true});
        CHECK(std::holds_alternative<gbin::NumericArray>(vA.v));
        const auto& a = std::get<gbin::NumericArray>(vA.v);
        CHECK(a.shape.size() == 2);
        CHECK(a.shape[0] == 2 && a.shape[1] == 3);
        CHECK(a.real_le.size() == 6 * sizeof(double));
    }

    // Corrupt payload => field CRC mismatch
    {
        std::filesystem::path p = tmp;
        flip_one_payload_byte(p);
        bool threw = false;
        try {
            (void)gbin::read_file(p, gbin::ReadOptions{true});
        } catch (const gbin::GbfError& e) {
            threw = (e.kind() == gbin::ErrorKind::FieldCrcMismatch) || (e.kind() == gbin::ErrorKind::ZlibError);
        }
        CHECK(threw);
    }

    // Rewrite clean, then corrupt header => header CRC mismatch
    {
        gbin::write_file(tmp, root, gbin::WriteOptions{});
        flip_one_header_byte(tmp);
        bool threw = false;
        try {
            (void)gbin::read_header_only(tmp, gbin::ReadOptions{true});
        } catch (const gbin::GbfError& e) {
            threw = (e.kind() == gbin::ErrorKind::HeaderCrcMismatch) || (e.kind() == gbin::ErrorKind::HeaderJsonParse);
        }
        CHECK(threw);
    }

    std::filesystem::remove(tmp);
    std::cout << "All tests passed.\n";
    return 0;
}

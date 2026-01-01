
#include "gbin/gbf.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

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

static double ms_since(const std::chrono::high_resolution_clock::time_point& t0) {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(high_resolution_clock::now() - t0).count();
}

static gbin::GbfValue make_payload(std::size_t rows, std::size_t cols) {
    gbin::GbfValue::Struct root;

    // Large double matrix
    {
        gbin::NumericArray a;
        a.class_id = gbin::NumericClass::Double;
        a.shape = {rows, cols};
        a.complex = false;
        std::vector<double> v(rows * cols);
        std::mt19937_64 rng(123);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        for (auto& x : v) x = dist(rng);
        a.real_le = as_bytes(v);
        root["A_double"] = gbin::GbfValue::make_numeric(a);
    }

    // Large single matrix
    {
        gbin::NumericArray a;
        a.class_id = gbin::NumericClass::Single;
        a.shape = {rows, cols};
        a.complex = false;
        std::vector<float> v(rows * cols);
        std::mt19937 rng(456);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        for (auto& x : v) x = dist(rng);
        a.real_le = as_bytes_f32(v);
        root["A_single"] = gbin::GbfValue::make_numeric(a);
    }

    return gbin::GbfValue::make_struct(root);
}

static void bench_one(const std::filesystem::path& file, gbin::CompressionMode comp) {
    std::size_t rows = 1200;
    std::size_t cols = 1200;
    gbin::GbfValue root = make_payload(rows, cols);

    gbin::WriteOptions wo;
    wo.compression = comp;
    wo.include_crc32 = true;
    wo.zlib_level = 6;

    std::cout << "=== " << (comp == gbin::CompressionMode::Never ? "compression=none" :
                             comp == gbin::CompressionMode::Always ? "compression=zlib" : "compression=auto")
              << " ===\n";

    auto t0 = std::chrono::high_resolution_clock::now();
    gbin::write_file(file, root, wo);
    double w_ms = ms_since(t0);

    std::uintmax_t sz = std::filesystem::file_size(file);
    double mb = static_cast<double>(sz) / (1024.0 * 1024.0);

    std::cout << "write: " << w_ms << " ms, file=" << mb << " MiB, throughput=" << (mb / (w_ms / 1000.0)) << " MiB/s\n";

    t0 = std::chrono::high_resolution_clock::now();
    gbin::GbfValue read = gbin::read_file(file, gbin::ReadOptions{true});
    double r_ms = ms_since(t0);
    std::cout << "read : " << r_ms << " ms, throughput=" << (mb / (r_ms / 1000.0)) << " MiB/s\n";
}

int main(int argc, char** argv) {
    std::filesystem::path file = (argc >= 2) ? argv[1] : (std::filesystem::temp_directory_path() / "gbin_cpp_bench.gbf");
    try {
        bench_one(file, gbin::CompressionMode::Never);
        bench_one(file, gbin::CompressionMode::Always);
        bench_one(file, gbin::CompressionMode::Auto);
    } catch (const std::exception& e) {
        std::cerr << "bench error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

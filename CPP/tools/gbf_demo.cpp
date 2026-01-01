#include "gbin/gbf_easy.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>


// Column-major fill for 2D: idx = r + c*rows
static std::vector<double> make_A_2x3_colmajor() {
    // A = [ 1 2 3
    //       4 5 6 ]
    // column-major storage: [1,4, 2,5, 3,6]
    return {1,4, 2,5, 3,6};
}

int main() {
    try {
        using namespace gbin;

        // Build root struct
        GbfValue::Struct root;

        // 2x3 double matrix
        NumericArray A;
        A.class_id = NumericClass::Double;
        A.shape = {2,3};
        auto dataA = make_A_2x3_colmajor();
        A.real_le = gbin::easy::pack_le(dataA);
        root["A"] = GbfValue::make_numeric(A);

        // 1x4 single vector (float)
        NumericArray B;
        B.class_id = NumericClass::Single;
        B.shape = {1,4};
        std::vector<float> dataB = {0.1f, 0.2f, 0.3f, 0.4f};
        B.real_le = gbin::easy::pack_le(dataB);
        root["B"] = GbfValue::make_numeric(B);

        // 3x3x4 int32
        NumericArray C;
        C.class_id = NumericClass::Int32;
        C.shape = {3,3,4};
        std::vector<std::int32_t> dataC;
        dataC.resize(3*3*4);
        for (std::size_t i = 0; i < dataC.size(); ++i) dataC[i] = (std::int32_t)i;
        C.real_le = gbin::easy::pack_le(dataC);
        root["C"] = GbfValue::make_numeric(C);

        // char array "hello"
        CharArray msg;
        msg.shape = {1,5};
        msg.utf16 = {(std::uint16_t)'h',(std::uint16_t)'e',(std::uint16_t)'l',(std::uint16_t)'l',(std::uint16_t)'o'};
        root["msg"] = GbfValue::make_char(msg);

        // Write
        WriteOptions wo;
        wo.compression = CompressionMode::Auto;
        wo.include_crc32 = true;
        wo.zlib_level = 6;

        std::string file = "demo_out.gbf";
        write_file(file, GbfValue::make_struct(root), wo);

        std::cout << "Wrote: " << file << "\n";

        // Read back root
        GbfValue read_root = read_file(file, ReadOptions{.validate=true});

        // Read a leaf and show quick info
        GbfValue readA = read_var(file, "A", ReadOptions{.validate=true});
        if (std::holds_alternative<NumericArray>(readA.v)) {
            const auto& a = std::get<NumericArray>(readA.v);
            std::cout << "Read A: class=" << to_string(a.class_id)
                      << " shape=[" << a.shape[0] << " x " << a.shape[1] << "]"
                      << " bytes=" << a.real_le.size()
                      << "\n";
        }

        GbfValue readMsg = read_var(file, "msg", ReadOptions{.validate=true});
        if (std::holds_alternative<CharArray>(readMsg.v)) {
            const auto& s = std::get<CharArray>(readMsg.v);
            std::cout << "Read msg utf16 length=" << s.utf16.size() << "\n";
        }

        std::cout << "OK\n";
        return 0;

    } catch (const gbin::GbfError& e) {
        std::cerr << "GBF error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
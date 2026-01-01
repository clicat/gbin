#pragma once

#include "gbin/gbf.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gbin::easy {

// Pack a typed vector into little-endian bytes.
// On modern macOS/Linux/Windows on x86_64/arm64 this is already little-endian.
// If you ever run on big-endian, you must swap bytes yourself.
template <typename T>
inline std::vector<std::uint8_t> pack_le(const std::vector<T>& v) {
    static_assert(std::is_trivially_copyable_v<T>, "pack_le requires trivially copyable types");
    std::vector<std::uint8_t> out(sizeof(T) * v.size());
    if (!out.empty()) {
        std::memcpy(out.data(), v.data(), out.size());
    }
    return out;
}

inline NumericArray make_numeric_doubles(std::vector<std::size_t> shape,
                                        const std::vector<double>& data_colmajor) {
    NumericArray a;
    a.class_id = NumericClass::Double;
    a.shape = std::move(shape);
    a.complex = false;
    a.real_le = pack_le(data_colmajor);
    return a;
}

inline NumericArray make_numeric_floats(std::vector<std::size_t> shape,
                                       const std::vector<float>& data_colmajor) {
    NumericArray a;
    a.class_id = NumericClass::Single;
    a.shape = std::move(shape);
    a.complex = false;
    a.real_le = pack_le(data_colmajor);
    return a;
}

inline NumericArray make_numeric_i32(std::vector<std::size_t> shape,
                                    const std::vector<std::int32_t>& data_colmajor) {
    NumericArray a;
    a.class_id = NumericClass::Int32;
    a.shape = std::move(shape);
    a.complex = false;
    a.real_le = pack_le(data_colmajor);
    return a;
}

// Convenience: store ASCII/UTF-8 bytes as UTF-16 code units.
// This is fine for typical labels. For full UTF-8 → UTF-16 correctness, add a converter.
inline CharArray make_char_utf8(std::string s) {
    CharArray c;
    c.shape = {1, s.size()};
    c.utf16.reserve(s.size());
    for (unsigned char ch : s) {
        c.utf16.push_back(static_cast<std::uint16_t>(ch));
    }
    return c;
}

inline void set(GbfValue::Struct& root, std::string key, GbfValue v) {
    root[std::move(key)] = std::move(v);
}

} // namespace gbin::easy
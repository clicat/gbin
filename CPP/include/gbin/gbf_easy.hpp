#pragma once

#include "gbin/gbf.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gbin::easy {

// Pack a typed vector into little-endian element bytes.
// The GBF on-disk representation is always little-endian.
namespace detail {
inline bool is_little_endian() {
    const std::uint16_t x = 1;
    return *reinterpret_cast<const std::uint8_t*>(&x) == 1;
}

inline void bswap_inplace(std::uint8_t* buf, std::size_t elem_size, std::size_t n_elems) {
    if (!buf || elem_size <= 1 || n_elems == 0) return;
    for (std::size_t i = 0; i < n_elems; ++i) {
        std::uint8_t* p = buf + i * elem_size;
        for (std::size_t a = 0, b = elem_size - 1; a < b; ++a, --b) {
            std::uint8_t t = p[a];
            p[a] = p[b];
            p[b] = t;
        }
    }
}
} // namespace detail

template <typename T>
inline std::vector<std::uint8_t> pack_le(const std::vector<T>& v) {
    static_assert(std::is_trivially_copyable_v<T>, "pack_le requires trivially copyable types");
    std::vector<std::uint8_t> out(sizeof(T) * v.size());
    if (!out.empty()) {
        std::memcpy(out.data(), v.data(), out.size());
        if (!detail::is_little_endian()) {
            detail::bswap_inplace(out.data(), sizeof(T), v.size());
        }
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
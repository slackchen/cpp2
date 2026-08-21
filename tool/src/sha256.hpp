// SHA-256(FIPS 180-4;.c2i 接口哈希与缓存键,v1 冻结格式的一部分)
// 自包含零依赖:与 rt 同一约定——纯头文件、无外部构建依赖。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <cstring>
#include <vector>

namespace cpp2::sha256 {

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline std::array<std::uint8_t, 32> digest(std::string const& msg)
{
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    std::vector<std::uint8_t> data(msg.begin(), msg.end());
    std::uint64_t bitlen = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>(bitlen >> (i * 8)));

    std::uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    for (size_t off = 0; off < data.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(data[off + i * 4]) << 24)
                 | (std::uint32_t(data[off + i * 4 + 1]) << 16)
                 | (std::uint32_t(data[off + i * 4 + 2]) << 8)
                 |  std::uint32_t(data[off + i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 3; j >= 0; --j)
            out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> ((3 - j) * 8));
    return out;
}

inline std::string hex(std::array<std::uint8_t, 32> const& d)
{
    static char const* digits = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; ++i) {
        s[i * 2]     = digits[d[i] >> 4];
        s[i * 2 + 1] = digits[d[i] & 0xf];
    }
    return s;
}

inline std::string hex_of(std::string const& msg) { return hex(digest(msg)); }

// 原始 32 字节 → hex(.c2i 读侧)
inline std::string hex_of_bytes(void const* p)
{
    std::array<std::uint8_t, 32> d;
    std::memcpy(d.data(), p, 32);
    return hex(d);
}

// hex(64 字符)→ 原始 32 字节(.c2i 写侧);非法输入返回全零
inline std::array<std::uint8_t, 32> digest_from_hex(std::string const& s)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, 32> d{};
    if (s.size() != 64) return d;
    for (int i = 0; i < 32; ++i) {
        int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::array<std::uint8_t, 32>{};
        d[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return d;
}

} // namespace cpp2::sha256

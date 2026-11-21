// sha256.cpp — FIPS 180-4 SHA-256；只用于把静态 key 规范化成不可逆摘要。
#include "llmopt/sha256.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace llmopt {
namespace {

constexpr std::array<std::uint32_t, 64> kK = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
};

std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t offset = index * 4;
        words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                       (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                       (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                       static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const std::uint32_t s0 = rotr(words[index - 15], 7U) ^ rotr(words[index - 15], 18U) ^
                                 (words[index - 15] >> 3U);
        const std::uint32_t s1 = rotr(words[index - 2], 17U) ^ rotr(words[index - 2], 19U) ^
                                 (words[index - 2] >> 10U);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    const std::uint32_t a0 = state[0];
    const std::uint32_t b0 = state[1];
    const std::uint32_t c0 = state[2];
    const std::uint32_t d0 = state[3];
    const std::uint32_t e0 = state[4];
    const std::uint32_t f0 = state[5];
    const std::uint32_t g0 = state[6];
    const std::uint32_t h0 = state[7];
    std::uint32_t a = a0;
    std::uint32_t b = b0;
    std::uint32_t c = c0;
    std::uint32_t d = d0;
    std::uint32_t e = e0;
    std::uint32_t f = f0;
    std::uint32_t g = g0;
    std::uint32_t h = h0;
    for (std::size_t index = 0; index < 64; ++index) {
        const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kK[index] + words[index];
        const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void append_length(std::vector<std::uint8_t>& buffer, std::uint64_t length_bytes) {
    const std::uint64_t length_bits = length_bytes * 8U;
    for (int shift = 56; shift >= 0; shift -= 8) {
        buffer.push_back(static_cast<std::uint8_t>((length_bits >> shift) & 0xffU));
    }
}

} // namespace

std::string sha256_hex(std::string_view data) {
    std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                          0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::vector<std::uint8_t> buffer(data.begin(), data.end());
    const std::uint64_t length_bytes = buffer.size();
    buffer.push_back(0x80U);
    while (buffer.size() % 64U != 56U) {
        buffer.push_back(0U);
    }
    append_length(buffer, length_bytes);
    for (std::size_t offset = 0; offset < buffer.size(); offset += 64U) {
        compress(state, buffer.data() + offset);
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint32_t word : state) {
        out << std::setw(8) << word;
    }
    return out.str();
}

} // namespace llmopt

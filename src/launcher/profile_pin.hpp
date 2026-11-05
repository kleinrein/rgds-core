#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

namespace rgds {
// A PIN keeps a child out of a profile and out of the settings. It cannot keep out someone with the card and a computer,
// who can read everything on it (docs/PROFILES_PLAN.md, section 6). Hashed so it cannot be read in a text editor; four
// digits can be tried offline, so this is no more than that.
inline std::array<uint8_t, 32> sha256(const std::string& message) {
    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string data = message;
    data.push_back(char(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    const uint64_t bits = uint64_t(message.size()) * 8;
    for (int i = 7; i >= 0; i--) data.push_back(char((bits >> (i * 8)) & 0xff));
    const auto rotate = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; i++)
            w[i] = uint32_t(uint8_t(data[chunk + i * 4])) << 24 | uint32_t(uint8_t(data[chunk + i * 4 + 1])) << 16 |
                   uint32_t(uint8_t(data[chunk + i * 4 + 2])) << 8 | uint32_t(uint8_t(data[chunk + i * 4 + 3]));
        for (size_t i = 16; i < 64; i++) {
            const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3),
                           s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h;
        for (size_t i = 0; i < 64; i++) {
            const uint32_t s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25),
                           choice = (v[4] & v[5]) ^ (~v[4] & v[6]), one = v[7] + s1 + choice + k[i] + w[i],
                           s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22),
                           majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]), two = s0 + majority;
            v = {one + two, v[0], v[1], v[2], v[3] + one, v[4], v[5], v[6]};
        }
        for (size_t i = 0; i < 8; i++) h[i] += v[i];
    }
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 8; i++)
        for (size_t j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h[i] >> (24 - j * 8));
    return out;
}

inline std::string pin_hex(const uint8_t* bytes, size_t size) {
    std::string text;
    char pair[3];
    for (size_t i = 0; i < size; i++) {
        std::snprintf(pair, sizeof(pair), "%02x", bytes[i]);
        text += pair;
    }
    return text;
}

inline std::string pin_digest(const std::string& salt, const std::string& pin) {
    std::string round = salt + ":" + pin;
    for (int i = 0; i < 2000; i++) {
        const auto digest = sha256(round + salt);
        round = pin_hex(digest.data(), digest.size());
    }
    return round;
}

inline std::string pin_salt() {
    std::array<uint8_t, 8> bytes{};
    std::ifstream random("/dev/urandom", std::ios::binary);
    random.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!random) { // never on a real system; a salt only has to differ
        const auto now = uint64_t(std::time(nullptr));
        for (size_t i = 0; i < bytes.size(); i++) bytes[i] = uint8_t(now >> (i * 8));
    }
    return pin_hex(bytes.data(), bytes.size());
}

// Stored as salt:digest.
inline std::string pin_store(const std::string& pin) {
    const auto salt = pin_salt();
    return salt + ":" + pin_digest(salt, pin);
}

inline bool pin_matches(const std::string& stored, const std::string& pin) {
    const auto colon = stored.find(':');
    return colon != std::string::npos && pin.size() == 4 &&
           pin_digest(stored.substr(0, colon), pin) == stored.substr(colon + 1);
}

// Five wrong tries wait 30 seconds, then doubling to five minutes.
inline long pin_wait_after(int tries) {
    if (tries < 5) return 0;
    long wait = 30;
    for (int i = 5; i < tries && wait < 300; i++) wait *= 2;
    return wait > 300 ? 300 : wait;
}
}

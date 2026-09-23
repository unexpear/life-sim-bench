// png.hpp — a minimal PNG writer.
//
// Stored (uncompressed) deflate blocks, which is a dozen lines and means the
// bench can emit images without taking an image-library dependency for
// something that gets viewed in any image viewer.

#pragma once
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <vector>

namespace bench {

// ── minimal PNG writer ──────────────────────────────────────────────────────
inline std::uint32_t crc32_of(const std::uint8_t* d, std::size_t n, std::uint32_t crc = 0) {
    static std::uint32_t tab[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = tab[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(std::uint8_t(x >> 24)); v.push_back(std::uint8_t(x >> 16));
    v.push_back(std::uint8_t(x >> 8));  v.push_back(std::uint8_t(x));
}

inline void chunk(std::vector<std::uint8_t>& out, const char* type,
           const std::vector<std::uint8_t>& data) {
    be32(out, std::uint32_t(data.size()));
    std::vector<std::uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32_of(td.data(), td.size()));
}

inline bool write_png(const char* path, int w, int h, const std::uint8_t* rgba) {
    std::vector<std::uint8_t> raw;
    raw.reserve(std::size_t(h) * (1 + w * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);                              // filter: none
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = rgba + (std::size_t(y) * w + x) * 4;
            raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]);
        }
    }
    // zlib stream with stored blocks
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::size_t off = 0;
    while (off < raw.size()) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = (off + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back(std::uint8_t(n & 0xFF));        z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n & 0xFF));       z.push_back(std::uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);

    std::vector<std::uint8_t> png{0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, std::uint32_t(w)); be32(ihdr, std::uint32_t(h));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}


} // namespace bench

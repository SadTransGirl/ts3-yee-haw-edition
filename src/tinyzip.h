// Minimal zip reader + DEFLATE decoder (after Mark Adler's puff.c), enough to
// read single entries from the game's own archives (stored or deflated).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace tinyzip {

struct Bits {
    const uint8_t* in; size_t inLen, inPos;
    uint32_t buf; int cnt;
    uint8_t* out; size_t outLen, outPos;
    bool err;
};

static int Need(Bits& s, int n) {
    uint32_t v = s.buf;
    while (s.cnt < n) {
        if (s.inPos >= s.inLen) { s.err = true; return 0; }
        v |= (uint32_t)s.in[s.inPos++] << s.cnt;
        s.cnt += 8;
    }
    s.buf = v >> n;
    s.cnt -= n;
    return (int)(v & ((1u << n) - 1));
}

struct Huff { short count[16]; short symbol[288]; };

static int Decode(Bits& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= Need(s, 1);
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s.err) return -1;
    }
    s.err = true;
    return -1;
}

static int Build(Huff& h, const short* length, int n) {
    for (int len = 0; len < 16; len++) h.count[len] = 0;
    for (int sym = 0; sym < n; sym++) h.count[length[sym]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;                 // over-subscribed
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h.symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static bool Put(Bits& s, uint8_t b) {
    if (s.outPos >= s.outLen) { s.err = true; return false; }
    s.out[s.outPos++] = b;
    return true;
}

static bool Codes(Bits& s, const Huff& lencode, const Huff& distcode) {
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                     257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = Decode(s, lencode);
        if (sym < 0 || s.err) return false;
        if (sym < 256) {
            if (!Put(s, (uint8_t)sym)) return false;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = lbase[sym] + Need(s, lext[sym]);
            int ds = Decode(s, distcode);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = (size_t)dbase[ds] + Need(s, dext[ds]);
            if (s.err || dist > s.outPos) return false;
            while (len--) if (!Put(s, s.out[s.outPos - dist])) return false;
        }
    }
}

static bool Stored(Bits& s) {
    s.buf = 0; s.cnt = 0;                          // discard leftover bits
    if (s.inPos + 4 > s.inLen) return false;
    unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xffff) || s.inPos + len > s.inLen || s.outPos + len > s.outLen) return false;
    memcpy(s.out + s.outPos, s.in + s.inPos, len);
    s.inPos += len; s.outPos += len;
    return true;
}

static bool Fixed(Bits& s) {
    static Huff lencode, distcode;
    static bool built;
    if (!built) {
        short lengths[288];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        Build(lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        Build(distcode, lengths, 30);
        built = true;
    }
    return Codes(s, lencode, distcode);
}

static bool Dynamic(Bits& s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320];
    int nlen = Need(s, 5) + 257, ndist = Need(s, 5) + 1, ncode = Need(s, 4) + 4;
    if (s.err || nlen > 286 || ndist > 30) return false;
    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (short)Need(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    Huff lencode, distcode;
    if (Build(lencode, lengths, 19) != 0) return false;
    index = 0;
    while (index < nlen + ndist) {
        int sym = Decode(s, lencode);
        if (sym < 0 || s.err) return false;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            short len = 0;
            int rep;
            if (sym == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                rep = 3 + Need(s, 2);
            } else if (sym == 17) {
                rep = 3 + Need(s, 3);
            } else {
                rep = 11 + Need(s, 7);
            }
            if (index + rep > nlen + ndist) return false;
            while (rep--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return false;
    int err = Build(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = Build(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return Codes(s, lencode, distcode);
}

// Raw DEFLATE (no zlib header). Returns bytes written, or -1.
static long long Inflate(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
    Bits s = { in, inLen, 0, 0, 0, out, outLen, 0, false };
    int last;
    do {
        last = Need(s, 1);
        int type = Need(s, 2);
        bool ok = type == 0 ? Stored(s) : type == 1 ? Fixed(s) : type == 2 ? Dynamic(s) : false;
        if (!ok || s.err) return -1;
    } while (!last);
    return (long long)s.outPos;
}

static uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Extract one entry from a zip held in memory. Returns malloc'd data (caller frees) or NULL.
static uint8_t* Extract(const uint8_t* zip, size_t zipLen, const char* name, size_t* outLen) {
    if (zipLen < 22) return NULL;
    size_t eocd = (size_t)-1;
    for (size_t i = zipLen - 22; ; i--) {
        if (U32(zip + i) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || zipLen - i > 22 + 65535) break;
    }
    if (eocd == (size_t)-1) return NULL;
    unsigned entries = U16(zip + eocd + 10);
    size_t cd = U32(zip + eocd + 16);
    size_t nameLen = strlen(name);
    for (unsigned e = 0; e < entries && cd + 46 <= zipLen; e++) {
        const uint8_t* c = zip + cd;
        if (U32(c) != 0x02014b50) return NULL;
        unsigned method = U16(c + 10), fnl = U16(c + 28), exl = U16(c + 30), cml = U16(c + 32);
        size_t csize = U32(c + 20), usize = U32(c + 24), lho = U32(c + 42);
        if (fnl == nameLen && cd + 46 + fnl <= zipLen && _strnicmp((const char*)c + 46, name, nameLen) == 0) {
            if (lho + 30 > zipLen || U32(zip + lho) != 0x04034b50) return NULL;
            size_t data = lho + 30 + U16(zip + lho + 26) + U16(zip + lho + 28);
            if (data + csize > zipLen) return NULL;
            uint8_t* out = (uint8_t*)malloc(usize ? usize : 1);
            if (!out) return NULL;
            long long got = method == 0 ? (memcpy(out, zip + data, csize), (long long)csize)
                          : method == 8 ? Inflate(zip + data, csize, out, usize) : -1;
            if (got != (long long)usize) { free(out); return NULL; }
            *outLen = usize;
            return out;
        }
        cd += 46 + fnl + exl + cml;
    }
    return NULL;
}

}  // namespace tinyzip

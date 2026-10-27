// The 2010 PC script fixes for four behaviour scripts (player, player2, carriable,
// zurgbot), applied to the Complete Edition's own .sx files in memory as the game reads
// them ([harness] script_fixes_2010=1; spec re\SCRIPT_FIXES_2010.md, reference
// implementation re\tools\sx_patch.py, whose output this matches byte for byte).
//
// A compiled script (SLNG) is a header of 26 ints, tables reached through field-relative
// pointers (target = field + value + 1) and a code segment of 16-bit words. Jump and call
// targets are word indexes from the start of the code, so a patch never moves anything:
//   - missing strings / native imports: the table is copied to the end of the file with
//     the new entries appended (old entries keep their targets, so old indexes stay valid);
//   - code: the whole segment is copied to the end and the "caves" appended after it. At
//     each anchor, whole instructions are overwritten with "jmp cave" plus nops; the cave
//     re-runs the displaced instructions and jumps back;
//   - the header points at the copies; the old tables and code stay as dead bytes.
// Only the exact retail files are patched (size and SHA-256); anything else is left alone.
// Pure data work: no game calls, so re\tools can test it offline.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>

struct SxAnchor { uint32_t at, n; };                          // overwrite words [at, at + n)
struct SxNewNative { const char* name; uint32_t hash; int32_t effect; };   // hash: lookup2(name), init 0
#include "sx_fixes_2010.inc"

struct SxPatchSpec {
    const char* name;                     // file name, as the game asks for it
    uint32_t size;                        // the retail file's size
    const char* sha256;                   // and SHA-256 (lower-case hex)
    const SxAnchor* anchors; size_t nAnchors;
    const uint32_t* caveStarts;           // per anchor: the word its cave starts at
    const uint16_t* cave; size_t nCave;   // every cave, appended after the moved code
    const char* const* strings; size_t nStrings;      // appended to the string table, in order
    const SxNewNative* natives; size_t nNatives;      // appended to the native imports, in order
};
#define SX_N(a) (sizeof(a) / sizeof((a)[0]))
#define SX_NN(a) ((a)[0] ? SX_N(a) : 0)                        // "{nullptr}" = none
#define SX_NNN(a) ((a)[0].name ? SX_N(a) : 0)
static const SxPatchSpec kSxPatches[] = {
    { "player.sx", 224772, "b5edf2b424a115990d31e9fad85794d8457c6a8041d9bb4ed3fa0ec970677011",
      kPlayerAnchors, SX_N(kPlayerAnchors), kPlayerCaveStarts, kPlayerCave, SX_N(kPlayerCave),
      kPlayerNewStrings, SX_NN(kPlayerNewStrings), kPlayerNewNatives, SX_NNN(kPlayerNewNatives) },
    { "player2.sx", 56192, "6a6ac52c5ba8b0ad831de0c2fdd8b3505d7c39bbff246f2afa8571a621b5e581",
      kPlayer2Anchors, SX_N(kPlayer2Anchors), kPlayer2CaveStarts, kPlayer2Cave, SX_N(kPlayer2Cave),
      kPlayer2NewStrings, SX_NN(kPlayer2NewStrings), kPlayer2NewNatives, SX_NNN(kPlayer2NewNatives) },
    { "carriable.sx", 37932, "a4a3ce3caf742180a7a09205fbcc6ff87025ec5b2bb310db350c32278cc8d248",
      kCarriableAnchors, SX_N(kCarriableAnchors), kCarriableCaveStarts, kCarriableCave, SX_N(kCarriableCave),
      kCarriableNewStrings, SX_NN(kCarriableNewStrings), kCarriableNewNatives, SX_NNN(kCarriableNewNatives) },
    { "zurgbot.sx", 37792, "24b84d5e3aa42eb711ed27049c40ce4e2c15aef2e3d991b39b811ef8e31f4921",
      kZurgbotAnchors, SX_N(kZurgbotAnchors), kZurgbotCaveStarts, kZurgbotCave, SX_N(kZurgbotCave),
      kZurgbotNewStrings, SX_NN(kZurgbotNewStrings), kZurgbotNewNatives, SX_NNN(kZurgbotNewNatives) },
};

// Header fields: [3] file size, [6] code words, [11] native count, [12] string count;
// pointers [16] code, [21] natives (16-byte records), [25] strings (4-byte pointers).
enum { SX_SIZE = 3, SX_N_CODE = 6, SX_N_NATIVES = 11, SX_N_STRINGS = 12, SX_CODE = 16, SX_NATIVES = 21, SX_STRINGS = 25 };

// The patched copy of in (which must be spec's retail file; the caller checks the hash).
static std::vector<uint8_t> SxBuildPatched(const uint8_t* in, uint32_t size, const SxPatchSpec& spec) {
    std::vector<uint8_t> out(in, in + size);
    auto rdIn = [&](size_t at) { int32_t v; memcpy(&v, in + at, 4); return v; };
    auto wr = [&](size_t at, int32_t v) { memcpy(&out[at], &v, 4); };
    auto tabIn = [&](int f) -> size_t { int32_t v = rdIn(f * 4); return v ? (size_t)(f * 4 + v + 1) : 0; };   // original header
    auto relIn = [&](size_t at) -> size_t { int32_t v = rdIn(at); return v ? (size_t)(at + v + 1) : 0; };
    auto relTo = [](size_t at, size_t target) { return target ? (int32_t)(target - at - 1) : 0; };
    auto align = [&] { while (out.size() % 16) out.push_back(0); };
    auto setField = [&](int f, size_t target) { wr(f * 4, (int32_t)(target - f * 4 - 1)); };
    auto appendStr = [&](const char* s) { size_t at = out.size(); out.insert(out.end(), s, s + strlen(s) + 1); return at; };

    if (spec.nStrings) {
        align();
        std::vector<size_t> addr;
        for (size_t j = 0; j < spec.nStrings; j++) addr.push_back(appendStr(spec.strings[j]));
        align();
        size_t oldN = (size_t)rdIn(SX_N_STRINGS * 4), old = tabIn(SX_STRINGS), table = out.size();
        out.resize(table + 4 * (oldN + spec.nStrings));
        for (size_t i = 0; i < oldN; i++) wr(table + 4 * i, relTo(table + 4 * i, relIn(old + 4 * i)));
        for (size_t j = 0; j < spec.nStrings; j++) wr(table + 4 * (oldN + j), relTo(table + 4 * (oldN + j), addr[j]));
        setField(SX_STRINGS, table);
        wr(SX_N_STRINGS * 4, (int32_t)(oldN + spec.nStrings));
    }
    if (spec.nNatives) {
        align();
        std::vector<size_t> addr;
        for (size_t j = 0; j < spec.nNatives; j++) addr.push_back(appendStr(spec.natives[j].name));
        align();
        size_t oldN = (size_t)rdIn(SX_N_NATIVES * 4), old = tabIn(SX_NATIVES), table = out.size();
        out.resize(table + 16 * (oldN + spec.nNatives));
        for (size_t i = 0; i < oldN; i++) {                    // {name, hash, 0, stack effect}: re-point the name
            size_t at = table + 16 * i;
            wr(at, relTo(at, relIn(old + 16 * i)));
            memcpy(&out[at + 4], in + old + 16 * i + 4, 12);
        }
        for (size_t j = 0; j < spec.nNatives; j++) {
            size_t at = table + 16 * (oldN + j);
            wr(at, relTo(at, addr[j]));
            wr(at + 4, (int32_t)spec.natives[j].hash);
            wr(at + 8, 0);
            wr(at + 12, spec.natives[j].effect);
        }
        setField(SX_NATIVES, table);
        wr(SX_N_NATIVES * 4, (int32_t)(oldN + spec.nNatives));
    }
    align();
    size_t nCode = (size_t)rdIn(SX_N_CODE * 4), codeAt = out.size();
    std::vector<uint16_t> words(nCode);
    memcpy(words.data(), in + tabIn(SX_CODE), nCode * 2);
    for (size_t k = 0; k < spec.nAnchors; k++) {
        uint32_t at = spec.anchors[k].at, cave = spec.caveStarts[k];
        words[at] = (uint16_t)(0x2A | ((cave >> 16) << 8));    // jmp cave
        words[at + 1] = (uint16_t)(cave & 0xFFFF);
        for (uint32_t i = at + 2; i < at + spec.anchors[k].n; i++) words[i] = 0x36;   // nop (the cave jumps past)
    }
    words.insert(words.end(), spec.cave, spec.cave + spec.nCave);
    out.insert(out.end(), (const uint8_t*)words.data(), (const uint8_t*)(words.data() + words.size()));
    setField(SX_CODE, codeAt);
    wr(SX_N_CODE * 4, (int32_t)words.size());
    align();
    wr(SX_SIZE * 4, (int32_t)out.size());
    return out;
}

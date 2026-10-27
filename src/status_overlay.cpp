// status_overlay.cpp -- the on-screen status banner: its own layered window and thread, art
// loaded from the game's UI zip, software drawing, Status() and ShowNotice().
//
// Entry points: Status() and ShowNotice(), called by every other module from any thread
// (the game thread, packet handling, start-up). They only store the text, under a lock;
// the first call starts the overlay thread, which owns the window and does all the drawing
// from a timer (OvUpdate). A message stays up for its time or until the next one replaces
// it: there is one banner, not a queue.

#include "proxy_shared.h"

#include "tinyzip.h"

// ------------------------------------------------------------------ on-screen status
//
// A small click-through banner at the top of the game window, so players can
// see what netplay is doing without reading the log. It never takes focus,
// and hides while another program is in front.
//
// It is drawn in the game's own style from the game's own files, read at run
// time (nothing of the game's is copied into this source): the blue
// mission-alert panel and icon bubble (ui\screens\mediummissionalert.zip), an
// icon from ui\icons\icons.zip, and the game's bitmap fonts (Gill Sans Bold
// headline, DIN Condensed body; ui\fonts\fonts.zip). It drops in and settles
// like the mission alert does (spriteanimations\mediummissionalert.lua).
// Everything runs on the overlay's own thread on wall-clock time, never on
// the game's clock, so it can't affect lockstep. (One-PC notices must not go
// through the game's own alert or tip screens: those pause mission timing on
// the PC showing them.) If the art can't be read it falls back to a plain box.

// What Status() last asked for, under g_ovLock: the text (first line the headline), when it
// goes (GetTickCount64 time; ~0 = never), its accent colour, and g_ovSeq, bumped on every
// change. g_ovDrawnSeq is the one the overlay thread last laid out.
static CRITICAL_SECTION g_ovLock;
static INIT_ONCE g_ovOnce = INIT_ONCE_STATIC_INIT;
static wchar_t   g_ovText[1024];
static ULONGLONG g_ovUntil;
static COLORREF  g_ovAccent;
static LONG      g_ovSeq, g_ovDrawnSeq = -1;
static HWND      g_ovWnd, g_ovGame;        // the banner's own window; the game window it sits over
static HFONT     g_ovFont;                 // plain-box fallback only

// The game's main window: this process's largest visible top-level window (the banner
// excluded). Looked up again whenever the one found before is gone.
static HWND OvFindGameWindow() {
    struct S { DWORD pid; HWND best; LONG area; } st = { GetCurrentProcessId(), NULL, 0 };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        S* st = (S*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != st->pid || h == g_ovWnd || !IsWindowVisible(h)) return TRUE;
        RECT r;
        GetClientRect(h, &r);
        LONG a = (r.right - r.left) * (r.bottom - r.top);
        if (a > st->area) { st->area = a; st->best = h; }
        return TRUE;
    }, (LPARAM)&st);
    return st.best;
}

// --- the game's art ---

struct OvTex { int w = 0, h = 0; std::vector<float> px; };          // premultiplied RGBA, 0..1
struct OvGlyph { int x0, y0, x1, y1; float left, top, adv; };      // atlas rect; offsets/advance in atlas pixels
struct OvFont {
    int w = 0, h = 0;
    std::vector<uint8_t> a;                // atlas coverage (the atlas is white, coverage in alpha)
    std::vector<int16_t> cmap;             // code point - 1 -> glyph number (1-based, 0 = none)
    std::vector<OvGlyph> g;
    float lh = 0;                          // line height
};
enum { OVI_COOP, OVI_OK, OVI_WARN, OVI_FAIL, OVI_WAIT, OVI_N };
static OvTex  g_ovPanel, g_ovCircle, g_ovIcon[OVI_N];
static OvFont g_ovHead, g_ovBody;
static bool   g_ovArt;                     // all of it loaded: draw the game-styled banner

// A whole file from the game's assets\ui folder (rel is relative to it); empty if it's
// missing or couldn't be read in full.
static std::vector<uint8_t> OvReadUiFile(const wchar_t* rel) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\assets\\ui\\%s", g_exeDir, rel);
    std::vector<uint8_t> v;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return v;
    DWORD size = GetFileSize(f, NULL), got = 0;
    if (size != INVALID_FILE_SIZE) {
        v.resize(size);
        if (!ReadFile(f, v.data(), size, &got, NULL) || got != size) v.clear();
    }
    CloseHandle(f);
    return v;
}

// The texture inside a .oct: a DDS blob, uncompressed 32-bit BGRA. Returns mip 0's pixels or NULL.
// Found by searching for the DDS magic plus the header's size byte ("DDS " then 124 = '|').
// From the magic: height +12, width +16, bits per pixel +88; mip 0 follows the 128 bytes.
static const uint8_t* OvDds(const uint8_t* oct, size_t len, int* w, int* h) {
    for (size_t i = 0; i + 128 <= len; i++) {
        if (memcmp(oct + i, "DDS |", 5) != 0) continue;
        const uint8_t* d = oct + i;
        uint32_t hh = *(const uint32_t*)(d + 12), ww = *(const uint32_t*)(d + 16), bits = *(const uint32_t*)(d + 88);
        if (bits != 32 || !ww || !hh || ww > 4096 || hh > 4096 || i + 128 + (size_t)ww * hh * 4 > len) return NULL;
        *w = (int)ww;
        *h = (int)hh;
        return d + 128;
    }
    return NULL;
}

// Texture 'name' out of a UI zip, as premultiplied float RGBA (the file's pixels are BGRA,
// straight alpha). False if it's missing or not an uncompressed 32-bit DDS.
static bool OvLoadTex(const std::vector<uint8_t>& zip, const char* name, OvTex& t) {
    size_t len = 0;
    uint8_t* oct = zip.empty() ? NULL : tinyzip::Extract(zip.data(), zip.size(), name, &len);
    int w = 0, h = 0;
    const uint8_t* p = oct ? OvDds(oct, len, &w, &h) : NULL;
    if (p) {
        t.w = w;
        t.h = h;
        t.px.resize((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            float a = p[i * 4 + 3] / 255.0f;
            t.px[i * 4 + 0] = p[i * 4 + 2] / 255.0f * a;
            t.px[i * 4 + 1] = p[i * 4 + 1] / 255.0f * a;
            t.px[i * 4 + 2] = p[i * 4 + 0] / 255.0f * a;
            t.px[i * 4 + 3] = a;
        }
    }
    free(oct);
    return p != NULL;
}

// A "fnt2" font (layout as read by the debug menu's LoadFont) and its atlas .oct.
// Header, as 32-bit words: [4] offset of the metrics (line height first), [9]/[10] the
// char map's offset and count, [11]/[12] the glyph table's offset and count (44-byte
// records: atlas rect x0 y0 x1 y1, then left, top and, at [8], advance). Metrics are 16.16
// fixed point. Only the atlas's alpha is kept. False if any part is missing or out of range.
static bool OvLoadFont(const std::vector<uint8_t>& zip, const char* base, OvFont& f) {
    char name[96];
    size_t fl = 0, ol = 0;
    sprintf_s(name, "%s.fnt", base);
    uint8_t* fnt = zip.empty() ? NULL : tinyzip::Extract(zip.data(), zip.size(), name, &fl);
    sprintf_s(name, "%s.oct", base);
    uint8_t* oct = zip.empty() ? NULL : tinyzip::Extract(zip.data(), zip.size(), name, &ol);
    int w = 0, h = 0;
    const uint8_t* px = oct ? OvDds(oct, ol, &w, &h) : NULL;
    bool ok = false;
    if (fnt && px && fl > 0x40 && memcmp(fnt, "fnt2", 4) == 0) {
        const uint32_t* hd = (const uint32_t*)fnt;
        uint32_t metOff = hd[4], cmapOff = hd[9], cmapN = hd[10], gOff = hd[11], gN = hd[12];
        if (metOff + 4 <= fl && cmapOff + (size_t)cmapN * 2 <= fl && gOff + (size_t)gN * 44 <= fl) {
            f.w = w;
            f.h = h;
            f.a.resize((size_t)w * h);
            for (size_t i = 0; i < f.a.size(); i++) f.a[i] = px[i * 4 + 3];
            f.cmap.assign((const int16_t*)(fnt + cmapOff), (const int16_t*)(fnt + cmapOff) + cmapN);
            f.g.resize(gN);
            for (uint32_t i = 0; i < gN; i++) {
                const int32_t* r = (const int32_t*)(fnt + gOff + (size_t)i * 44);
                f.g[i] = { r[0], r[1], r[2], r[3], r[4] / 65536.0f, r[5] / 65536.0f, r[8] / 65536.0f };
            }
            f.lh = *(const int32_t*)(fnt + metOff) / 65536.0f;
            ok = f.lh > 0;
        }
    }
    free(fnt);
    free(oct);
    return ok;
}

// Loads the panel, the icon bubble, the five icons and the two fonts, once, on the overlay
// thread. g_ovArt is set only if every piece loaded; otherwise the plain box is used.
static void OvLoadArt() {
    std::vector<uint8_t> screens = OvReadUiFile(L"screens\\mediummissionalert.zip"),
                         icons = OvReadUiFile(L"icons\\icons.zip"), fonts = OvReadUiFile(L"fonts\\fonts.zip");
    static const char* kIcon[OVI_N] = { "icon_coop.oct", "icon_taskcomplete.oct", "icon_warning.oct",
                                        "icon_failed.oct", "icon_loading.oct" };
    bool ok = OvLoadTex(screens, "ui/screens/backbox8b_slice.oct", g_ovPanel) &&
              OvLoadTex(screens, "ui/screens/backcircle8b.oct", g_ovCircle) &&
              OvLoadFont(fonts, "urwegyptienne_english_ntsc", g_ovHead) &&
              OvLoadFont(fonts, "tradegothic_english_ntsc", g_ovBody);
    for (int i = 0; ok && i < OVI_N; i++) ok = OvLoadTex(icons, kIcon[i], g_ovIcon[i]);
    g_ovArt = ok;
    Log(ok ? "status banner: drawn with the game's alert art and fonts"
           : "status banner: couldn't read the game's UI art; using a plain box");
}

// --- software drawing (overlay thread only) ---

// A float image the banner is composed in before it's turned into the window's 8-bit pixels.
struct OvCanvas {
    int w = 0, h = 0;
    std::vector<float> px;                 // premultiplied RGBA
    void Reset(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 4, 0.0f); }   // resize, fully transparent
    // One premultiplied pixel composited over what's there ("over"); off-canvas is ignored.
    void Over(int x, int y, float r, float g, float b, float a) {
        if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h || a <= 0) return;
        float* d = &px[((size_t)y * w + x) * 4];
        float ia = 1 - a;
        d[0] = r + d[0] * ia; d[1] = g + d[1] * ia; d[2] = b + d[2] * ia; d[3] = a + d[3] * ia;
    }
};

// Bilinear sample at (u, v), clamped to the source rect so 9-slice pieces don't bleed.
static void OvSample(const OvTex& t, float u, float v, int sx0, int sy0, int sx1, int sy1, float* out) {
    u = (std::min)((std::max)(u, (float)sx0), (float)(sx1 - 1));
    v = (std::min)((std::max)(v, (float)sy0), (float)(sy1 - 1));
    int x0 = (int)u, y0 = (int)v, x1 = (std::min)(x0 + 1, sx1 - 1), y1 = (std::min)(y0 + 1, sy1 - 1);
    float fx = u - x0, fy = v - y0;
    const float *a = &t.px[((size_t)y0 * t.w + x0) * 4], *b = &t.px[((size_t)y0 * t.w + x1) * 4],
                *c = &t.px[((size_t)y1 * t.w + x0) * 4], *d = &t.px[((size_t)y1 * t.w + x1) * 4];
    for (int i = 0; i < 4; i++) out[i] = (a[i] + (b[i] - a[i]) * fx) * (1 - fy) + (c[i] + (d[i] - c[i]) * fx) * fy;
}

// Draws source rect (sx0,sy0)-(sx1,sy1) of t into the destination rect: scaled (supersampled
// when shrinking) and optionally turned by angle radians about the rect's centre.
static void OvBlit(OvCanvas& c, const OvTex& t, int sx0, int sy0, int sx1, int sy1,
                   float fx0, float fy0, float fx1, float fy1, float angle = 0) {
    int dx0 = (int)lroundf(fx0), dy0 = (int)lroundf(fy0), dx1 = (int)lroundf(fx1), dy1 = (int)lroundf(fy1);
    if (dx1 <= dx0 || dy1 <= dy0 || sx1 <= sx0 || sy1 <= sy0) return;
    float kx = (float)(sx1 - sx0) / (dx1 - dx0), ky = (float)(sy1 - sy0) / (dy1 - dy0);
    int n = (std::min)(4, (std::max)(1, (int)ceilf((std::max)(kx, ky))));
    float ca = cosf(angle), sa = sinf(angle), cx = (dx0 + dx1) * 0.5f, cy = (dy0 + dy1) * 0.5f;
    float m = 1.0f / (n * n), s[4];
    for (int y = (std::max)(dy0, 0); y < (std::min)(dy1, c.h); y++)
        for (int x = (std::max)(dx0, 0); x < (std::min)(dx1, c.w); x++) {
            float acc[4] = {};
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    float px = x + (i + 0.5f) / n, py = y + (j + 0.5f) / n;
                    if (angle != 0) {
                        float rx = px - cx, ry = py - cy;
                        px = cx + rx * ca + ry * sa;
                        py = cy - rx * sa + ry * ca;
                        if (px < dx0 || px >= dx1 || py < dy0 || py >= dy1) continue;
                    }
                    OvSample(t, sx0 + (px - dx0) * kx - 0.5f, sy0 + (py - dy0) * ky - 0.5f, sx0, sy0, sx1, sy1, s);
                    for (int q = 0; q < 4; q++) acc[q] += s[q];
                }
            c.Over(x, y, acc[0] * m, acc[1] * m, acc[2] * m, acc[3] * m);
        }
}

// 9-slice: corners keep their shape (bs source pixels drawn bd wide), edges and middle stretch.
static void OvNine(OvCanvas& c, const OvTex& t, int bs, float bd, float x0, float y0, float x1, float y1) {
    int xs[4] = { 0, bs, t.w - bs, t.w }, ys[4] = { 0, bs, t.h - bs, t.h };
    float dxs[4] = { x0, x0 + bd, x1 - bd, x1 }, dys[4] = { y0, y0 + bd, y1 - bd, y1 };
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) OvBlit(c, t, xs[i], ys[j], xs[i + 1], ys[j + 1], dxs[i], dys[j], dxs[i + 1], dys[j + 1]);
}

// The font's glyph for ch through its char map, or NULL if it has none.
static const OvGlyph* OvFindGlyph(const OvFont& f, wchar_t ch) {
    unsigned cp = ch;
    if (cp == 0 || cp > f.cmap.size()) return NULL;
    int v = f.cmap[cp - 1];
    return v > 0 && v <= (int)f.g.size() ? &f.g[v - 1] : NULL;
}

// Letters the font lacks (a Steam name in another alphabet) show as '?'.
static const OvGlyph* OvGlyphFor(const OvFont& f, wchar_t ch) {
    const OvGlyph* g = OvFindGlyph(f, ch);
    return g || ch == L' ' ? g : OvFindGlyph(f, L'?');
}

// Width in screen pixels of s[0..n) drawn at scale k (screen pixels per atlas pixel).
// A space (no glyph) advances a quarter of the line height.
static float OvTextWidth(const OvFont& f, float k, const wchar_t* s, size_t n) {
    float pen = 0;
    for (size_t i = 0; i < n; i++) {
        const OvGlyph* g = OvGlyphFor(f, s[i]);
        pen += g ? g->left + (g->x1 - g->x0) + g->adv : f.lh * 0.25f;
    }
    return pen * k;
}

// Greedy word wrap of s[0..n) to maxW.
static void OvWrap(const OvFont& f, float k, const wchar_t* s, size_t n, float maxW, std::vector<std::wstring>& out) {
    std::wstring line;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != L' ') j++;
        std::wstring word(s + i, j - i);
        std::wstring tryLine = line.empty() ? word : line + L' ' + word;
        if (!line.empty() && OvTextWidth(f, k, tryLine.c_str(), tryLine.size()) > maxW) {
            out.push_back(line);
            line = word;
        } else {
            line = tryLine;
        }
        i = j + 1;
    }
    out.push_back(line);
}

// Adds one line of text, top-left at (ox, oy), to a coverage mask the size of the canvas.
static void OvTextMask(const OvFont& f, float k, const std::wstring& s, float ox, float oy, std::vector<float>& m, int W, int H) {
    int n = (std::min)(4, (std::max)(1, (int)ceilf(1.0f / k)));
    float pen = ox;
    for (wchar_t ch : s) {
        const OvGlyph* g = OvGlyphFor(f, ch);
        if (!g) { pen += f.lh * 0.25f * k; continue; }
        if (g->x1 > g->x0 && g->y1 > g->y0 && g->x0 >= 0 && g->y0 >= 0 && g->x1 <= f.w && g->y1 <= f.h) {
            float X0 = pen + g->left * k, Y0 = oy + g->top * k;
            float X1 = X0 + (g->x1 - g->x0) * k, Y1 = Y0 + (g->y1 - g->y0) * k;
            for (int y = (std::max)(0, (int)Y0); y < (std::min)(H, (int)ceilf(Y1)); y++)
                for (int x = (std::max)(0, (int)X0); x < (std::min)(W, (int)ceilf(X1)); x++) {
                    float acc = 0;
                    for (int j = 0; j < n; j++)
                        for (int i = 0; i < n; i++) {
                            float u = g->x0 + (x + (i + 0.5f) / n - X0) / k - 0.5f, v = g->y0 + (y + (j + 0.5f) / n - Y0) / k - 0.5f;
                            u = (std::min)((std::max)(u, (float)g->x0), (float)(g->x1 - 1));
                            v = (std::min)((std::max)(v, (float)g->y0), (float)(g->y1 - 1));
                            int iu = (int)u, iv = (int)v, iu1 = (std::min)(iu + 1, g->x1 - 1), iv1 = (std::min)(iv + 1, g->y1 - 1);
                            float fu = u - iu, fv = v - iv;
                            const uint8_t* a = f.a.data();
                            float t0 = a[iv * f.w + iu] + (a[iv * f.w + iu1] - a[iv * f.w + iu]) * fu;
                            float t1 = a[iv1 * f.w + iu] + (a[iv1 * f.w + iu1] - a[iv1 * f.w + iu]) * fu;
                            acc += (t0 + (t1 - t0) * fv) / 255.0f;
                        }
                    float& d = m[(size_t)y * W + x];
                    d = (std::max)(d, acc / (n * n));
                }
        }
        pen += (g->left + (g->x1 - g->x0) + g->adv) * k;
    }
}

// Paints a canvas-sized coverage mask in one colour at the given opacity, moved down dy
// rows (dy > 0: the drop shadow).
static void OvPaintMask(OvCanvas& c, const std::vector<float>& m, float r, float g, float b, float alpha, int dy) {
    for (int y = (std::max)(0, dy); y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            float a = m[(size_t)(y - dy) * c.w + x] * alpha;
            if (a > 0) c.Over(x, y, r * a, g * a, b * a, a);
        }
}

// --- the banner ---

// The laid-out banner, kept until the text, its kind or the game window's size changes.
struct OvBanner {
    OvCanvas base;                         // panel, bubble and text; the icon goes on per frame (it may spin)
    int kind = -1;
    float s = 1;                           // size scale
    float ix0, iy0, ix1, iy1;              // icon rect
    int cw, ch;                            // game client size it was laid out for
};
// Overlay thread state. g_ovFrame: the base plus this frame's icon. g_ovMemDC / g_ovDib /
// g_ovBits: the 32-bit bitmap UpdateLayeredWindow copies from, grown as needed, never
// shrunk. g_ovShown / g_ovShowAt: the banner is up, and since when (its drop-in animation
// runs from then). g_ovPopAt: when it last re-settled for a new kind of message (0: not).
// g_ovTimerMs: the refresh timer's current period.
static OvBanner g_ovB;
static OvCanvas g_ovFrame;
static HDC      g_ovMemDC;
static HBITMAP  g_ovDib;
static uint8_t* g_ovBits;
static int      g_ovDibW, g_ovDibH;
static bool     g_ovShown;
static ULONGLONG g_ovShowAt, g_ovPopAt;
static UINT     g_ovTimerMs = 100;

// Waiting messages get the spinning ring; the rest go by their colour.
static int OvKind(const wchar_t* text, COLORREF accent) {
    const wchar_t* nl = wcschr(text, L'\n');
    std::wstring first(text, nl ? (size_t)(nl - text) : wcslen(text));
    if (first.find(L"...") != std::wstring::npos) return OVI_WAIT;
    return accent == kOvGreen ? OVI_OK : accent == kOvAmber ? OVI_WARN : accent == kOvRed ? OVI_FAIL : OVI_COOP;
}

// Lays the banner out for a game client of cw x ch and draws everything but the icon.
// Sizes are those of the game's alert at 1080p, scaled with the window height.
// The first line is the headline (Gill Sans); the rest is body text (DIN).
static void OvLayout(const wchar_t* text, int kind, int cw, int ch) {
    OvBanner& b = g_ovB;
    b.kind = kind;
    b.cw = cw;
    b.ch = ch;
    // Scale: s for the layout (clamped so tiny or huge windows stay readable), kh and kb
    // the atlas-to-screen factors that give 46 and 38 px lines (at s = 1). D: the bubble's
    // diameter; the panel starts under the bubble and the text to its right.
    float s = b.s = (std::min)(1.5f, (std::max)(0.3f, 0.6f * ch / 1080.0f));
    float kh = 46 * s / g_ovHead.lh, kb = 38 * s / g_ovBody.lh;
    float D = roundf(120 * s), panelX = roundf(D * 0.55f), textX = D + 14 * s;
    float maxW = (std::max)(200 * s, cw * 0.62f - textX - 50 * s);
    // Split at '\n' and word-wrap each piece; only the first piece is headline.
    struct Line { std::wstring s; bool head; };
    std::vector<Line> lines;
    const wchar_t* p = text;
    for (bool head = true;; head = false) {
        const wchar_t* e = wcschr(p, L'\n');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        std::vector<std::wstring> w;
        OvWrap(head ? g_ovHead : g_ovBody, head ? kh : kb, p, n, maxW, w);
        for (auto& l : w) lines.push_back({ l, head });
        if (!e) break;
        p = e + 1;
    }
    // Size the panel to the text block, then the canvas to the panel and bubble.
    float tw = 0, th = 0;
    for (auto& l : lines) {
        const OvFont& f = l.head ? g_ovHead : g_ovBody;
        float k = l.head ? kh : kb;
        tw = (std::max)(tw, OvTextWidth(f, k, l.s.c_str(), l.s.size()));
        th += f.lh * k;
    }
    float panelW = roundf(textX - panelX + tw + 50 * s), panelH = roundf((std::max)(th + 48 * s, D * 0.86f));
    int W = (int)(panelX + panelW) + 4, H = (int)(std::max)(D, panelH) + 4;
    b.base.Reset(W, H);
    // The panel (9-slice, 44 px corners), the bubble at the left, and the icon's rect in it.
    float py = floorf((H - panelH) / 2);
    OvNine(b.base, g_ovPanel, 44, roundf(44 * s), panelX, py, panelX + panelW, py + panelH);
    float cy = floorf((H - D) / 2);
    OvBlit(b.base, g_ovCircle, 0, 0, g_ovCircle.w, g_ovCircle.h, 0, cy, D, cy + D);
    float ic = D * 0.70f, ioff = D * 0.02f;    // centred on the bubble's face, not its shadow
    b.ix0 = (D - ic) / 2 - ioff;
    b.iy0 = cy + (D - ic) / 2 - ioff;
    b.ix1 = b.ix0 + ic;
    b.iy1 = b.iy0 + ic;
    // Text: a navy outline (and a soft drop shadow of it) under white, like the game's alert text.
    std::vector<float> mHead((size_t)W * H), mBody((size_t)W * H), all((size_t)W * H), outline((size_t)W * H);
    float y = py + (panelH - th) / 2;
    for (auto& l : lines) {
        const OvFont& f = l.head ? g_ovHead : g_ovBody;
        float k = l.head ? kh : kb;
        OvTextMask(f, k, l.s, textX, y, l.head ? mHead : mBody, W, H);
        y += f.lh * k;
    }
    for (size_t i = 0; i < all.size(); i++) all[i] = (std::max)(mHead[i], mBody[i]);
    // The outline: all the text grown by r pixels (each pixel takes the strongest coverage
    // within a disc of radius r). Painted shadow first, outline, then the text over it.
    int r = (std::max)(1, (int)lroundf(3 * s));
    for (int yy = 0; yy < H; yy++)
        for (int xx = 0; xx < W; xx++) {
            float best = 0;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int x2 = xx + dx, y2 = yy + dy;
                    if (dx * dx + dy * dy > r * r + r || (unsigned)x2 >= (unsigned)W || (unsigned)y2 >= (unsigned)H) continue;
                    best = (std::max)(best, all[(size_t)y2 * W + x2]);
                }
            outline[(size_t)yy * W + xx] = best;
        }
    OvPaintMask(b.base, outline, 0.0f, 0.03f, 0.12f, 0.55f, r);
    OvPaintMask(b.base, outline, 0.02f, 0.08f, 0.25f, 1.0f, 0);
    OvPaintMask(b.base, mBody, 0.86f, 0.93f, 1.0f, 1.0f, 0);
    OvPaintMask(b.base, mHead, 1.0f, 1.0f, 1.0f, 1.0f, 0);
}

// Puts the frame on screen at (x, y), stretched vertically by sy about its centre, faded by
// alpha. Rows above clipTop (the top of the game's client area) are cut off, so the drop-in
// never covers the title bar.
static void OvPresent(int x, int y, float sy, float alpha, int clipTop) {
    const OvCanvas& f = g_ovFrame;
    int H2 = (std::max)(1, (int)lroundf(f.h * sy)), top = y - (H2 - f.h) / 2, cut = (std::max)(0, clipTop - top);
    if (cut >= H2) { ShowWindow(g_ovWnd, SW_HIDE); return; }
    // The bitmap, grown if this frame is bigger than any before.
    if (!g_ovMemDC) g_ovMemDC = CreateCompatibleDC(NULL);
    if (f.w > g_ovDibW || H2 > g_ovDibH) {
        if (g_ovDib) DeleteObject(g_ovDib);
        g_ovDibW = (std::max)(f.w, g_ovDibW);
        g_ovDibH = (std::max)(H2, g_ovDibH);
        BITMAPINFO bi = {};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), g_ovDibW, -g_ovDibH, 1, 32, BI_RGB };
        void* bits = NULL;
        g_ovDib = CreateDIBSection(g_ovMemDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        g_ovBits = (uint8_t*)bits;
        if (!g_ovDib) { g_ovDibW = g_ovDibH = 0; return; }
        SelectObject(g_ovMemDC, g_ovDib);
    }
    // Each output row, stretched by linear filtering between source rows, into the
    // bitmap's premultiplied BGRA bytes (what UpdateLayeredWindow's per-pixel alpha wants).
    for (int r = 0; r < H2; r++) {
        float v = (std::min)((std::max)((r + 0.5f) / sy - 0.5f, 0.0f), (float)(f.h - 1));
        int r0 = (int)v, r1 = (std::min)(r0 + 1, f.h - 1);
        float fv = v - r0;
        const float *a = &f.px[(size_t)r0 * f.w * 4], *b = &f.px[(size_t)r1 * f.w * 4];
        uint8_t* d = g_ovBits + (size_t)r * g_ovDibW * 4;
        for (int i = 0; i < f.w; i++, a += 4, b += 4, d += 4) {
            float c[4];
            for (int q = 0; q < 4; q++) c[q] = (std::min)(1.0f, a[q] + (b[q] - a[q]) * fv);
            d[0] = (uint8_t)(c[2] * 255 + 0.5f);
            d[1] = (uint8_t)(c[1] * 255 + 0.5f);
            d[2] = (uint8_t)(c[0] * 255 + 0.5f);
            d[3] = (uint8_t)(c[3] * 255 + 0.5f);
        }
    }
    // Move, resize and repaint the window in one call; alpha fades the whole window.
    POINT dst = { x, top + cut }, src = { 0, cut };
    SIZE size = { f.w, H2 - cut };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)lroundf((std::min)(1.0f, (std::max)(0.0f, alpha)) * 255), AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(g_ovWnd, screen, &dst, &size, g_ovMemDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    if (!IsWindowVisible(g_ovWnd)) {
        SetWindowPos(g_ovWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(g_ovWnd, SW_SHOWNOACTIVATE);
    }
}

// The refresh timer's period: 16 ms while the banner moves, 33 while its icon spins, 100
// otherwise. Reset only when it changes (SetTimer restarts the count).
static void OvSetTimer(UINT ms) {
    if (ms != g_ovTimerMs) { g_ovTimerMs = ms; SetTimer(g_ovWnd, 1, ms, NULL); }
}

// The alert's drop-in: falls into place with a stretch, then settles with the wobble from
// mediummissionalert.lua (y 390 410 394 408 396 404 398 around 400, about 2 frames each).
static const float kOvSlideMs = 220, kOvStepMs = 35, kOvExitMs = 250;
static const float kOvSettle[] = { 10, -10, 6, -8, 4, -4, 2, 0 };
static const int   kOvSettleN = sizeof kOvSettle / sizeof kOvSettle[0];

// The wobble's vertical offset (unscaled; the caller multiplies by the banner's s) ms into the settle,
// eased linearly from one kOvSettle step to the next; 0 once it's over.
static float OvSettle(float ms) {
    int i = (int)(ms / kOvStepMs);
    if (i >= kOvSettleN) return 0;
    float a = i ? kOvSettle[i - 1] : 0, f = (ms - i * kOvStepMs) / kOvStepMs;
    return a + (kOvSettle[i] - a) * f;
}

// One refresh, on every tick of the overlay thread's timer: shows, moves, animates or hides
// the banner to match the latest Status() and the game window.
static void OvUpdate() {
    // A copy of the request, so the lock isn't held while drawing.
    wchar_t text[1024];
    ULONGLONG until;
    LONG seq;
    COLORREF accent;
    EnterCriticalSection(&g_ovLock);
    wcscpy_s(text, g_ovText);
    until = g_ovUntil;
    seq = g_ovSeq;
    accent = g_ovAccent;
    LeaveCriticalSection(&g_ovLock);
    // Wanted: there is text and its time isn't up (the art banner gets kOvExitMs more, to fade out).
    ULONGLONG now = GetTickCount64();
    bool forever = until == ~0ULL;
    bool want = text[0] && (forever || now < until + (g_ovArt ? (ULONGLONG)kOvExitMs : 0));
    if (want && (!g_ovGame || !IsWindow(g_ovGame))) g_ovGame = OvFindGameWindow();
    // One-PC co-op test (g_localTest >= 0): the two game windows' titles say which player
    // each is, once the window is first found.
    static bool titled;
    if (g_localTest >= 0 && g_ovGame && !titled) {
        titled = true;
        SetWindowTextW(g_ovGame, g_localTest ? L"Toy Story 3 - local co-op test - PLAYER 2 (client)"
                                             : L"Toy Story 3 - local co-op test - PLAYER 1 (host)");
    }
    if (want && g_ovGame) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid != GetCurrentProcessId() || IsIconic(g_ovGame)) want = false;   // the game isn't in front
    }
    if (!want || !g_ovGame) {
        if (IsWindowVisible(g_ovWnd)) ShowWindow(g_ovWnd, SW_HIDE);
        g_ovShown = false;
        OvSetTimer(100);
        return;
    }
    RECT cr;
    GetClientRect(g_ovGame, &cr);
    POINT org = { 0, 0 };
    ClientToScreen(g_ovGame, &org);
    // The game-styled banner.
    if (g_ovArt) {
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        if (cw < 64 || ch < 64) return;
        // Lay it out again for new text or a resized game window.
        bool relaid = false;
        if (seq != g_ovDrawnSeq || cw != g_ovB.cw || ch != g_ovB.ch) {
            int kind = OvKind(text, accent);
            if (g_ovShown && kind != g_ovB.kind) g_ovPopAt = now;    // new kind of news while up: settle again
            OvLayout(text, kind, cw, ch);
            g_ovDrawnSeq = seq;
            relaid = true;
        }
        // The animation, from the time since it appeared (t): drop in from above the
        // window's top with a vertical stretch and fade-in, then settle; a re-settle for a
        // new kind of message; on expiry, fade out while rising. off: pixels from the rest
        // position, sy: vertical stretch, alpha: opacity.
        if (!g_ovShown) { g_ovShown = true; g_ovShowAt = now; g_ovPopAt = 0; }
        float s = g_ovB.s, t = (float)(now - g_ovShowAt), off = 0, sy = 1, alpha = 1;
        int restY = (int)lroundf((std::max)(12.0f, ch * 0.03f));
        if (t < kOvSlideMs) {
            float u = 1 - t / kOvSlideMs, e = 1 - u * u * u;
            off = -(restY + g_ovB.base.h) * (1 - e);
            sy = 1.3f - 0.2f * e;
            alpha = (std::min)(1.0f, t / 120);
        } else {
            float u = t - kOvSlideMs;
            off = OvSettle(u) * s;
            if (u < kOvStepMs) sy = 1.1f - 0.1f * u / kOvStepMs;
        }
        bool popping = g_ovPopAt && now - g_ovPopAt < kOvSettleN * kOvStepMs;
        if (popping) off += OvSettle((float)(now - g_ovPopAt)) * s * 0.6f;
        bool leaving = !forever && now > until;
        if (leaving) {
            float u = (std::min)(1.0f, (now - until) / kOvExitMs);
            alpha *= 1 - u;
            off -= 24 * s * u * u;
        }
        // The icon goes onto a copy of the base: once per layout, or every refresh while the
        // waiting ring turns (one turn per 0.9 s).
        bool spin = g_ovB.kind == OVI_WAIT;
        float angle = spin ? -(float)(now % 900) / 900 * 6.2831853f : 0;
        static float lastAngle = -1;
        if (relaid || angle != lastAngle) {
            g_ovFrame = g_ovB.base;
            const OvTex& ic = g_ovIcon[g_ovB.kind];
            OvBlit(g_ovFrame, ic, 0, 0, ic.w, ic.h, g_ovB.ix0, g_ovB.iy0, g_ovB.ix1, g_ovB.iy1, angle);
            lastAngle = angle;
        }
        // Centred at the top of the client area; pushed to the screen only when something
        // changed, then the timer is set for how busy the next ticks will be.
        int x = org.x + (cw - g_ovFrame.w) / 2, y = org.y + restY + (int)lroundf(off);
        static int lastX, lastY;
        static float lastSy, lastAlpha;
        bool moving = t < kOvSlideMs + kOvSettleN * kOvStepMs || popping || leaving;
        if (relaid || spin || moving || x != lastX || y != lastY || sy != lastSy || alpha != lastAlpha ||
            !IsWindowVisible(g_ovWnd)) {
            OvPresent(x, y, sy, alpha, org.y);
            lastX = x; lastY = y; lastSy = sy; lastAlpha = alpha;
        }
        OvSetTimer(moving ? 16 : spin ? 33 : 100);
        return;
    }
    // The plain box (no art): size the window to the wrapped text, place it near the top of
    // the game window, and repaint (WM_PAINT draws it) when the text changed.
    HDC dc = GetDC(g_ovWnd);
    HGDIOBJ old = SelectObject(dc, g_ovFont);
    RECT tr = { 0, 0, (cr.right - cr.left) * 2 / 3, 0 };
    if (tr.right < 300) tr.right = 300;
    DrawTextW(dc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
    SelectObject(dc, old);
    ReleaseDC(g_ovWnd, dc);
    int w = (tr.right - tr.left) + 48, h = (tr.bottom - tr.top) + 24;
    int x = org.x + ((cr.right - cr.left) - w) / 2, y = org.y + 28;
    SetWindowPos(g_ovWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (seq != g_ovDrawnSeq) { g_ovDrawnSeq = seq; InvalidateRect(g_ovWnd, NULL, TRUE); }
}

// The banner window's messages. The timer drives OvUpdate; hit tests fall through to the
// game window (click-through) and a click never activates the banner. WM_PAINT draws only
// the plain box: a dark panel, an accent bar at the left, the text centred.
static LRESULT CALLBACK OvProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: OvUpdate(); return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        if (g_ovArt) { ValidateRect(h, NULL); return 0; }   // UpdateLayeredWindow owns the pixels
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        wchar_t text[1024];
        COLORREF accent;
        EnterCriticalSection(&g_ovLock);
        wcscpy_s(text, g_ovText);
        accent = g_ovAccent;
        LeaveCriticalSection(&g_ovLock);
        HBRUSH bg = CreateSolidBrush(RGB(22, 24, 34));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HBRUSH bar = CreateSolidBrush(accent);
        RECT br = { 0, 0, 8, r.bottom };
        FillRect(dc, &br, bar);
        DeleteObject(bar);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(245, 245, 250));
        HGDIOBJ old = SelectObject(dc, g_ovFont);
        RECT tr = { 28, 12, r.right - 20, r.bottom - 12 };
        DrawTextW(dc, text, -1, &tr, DT_WORDBREAK | DT_CENTER);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// The overlay thread: loads the art, makes the banner window (topmost, layered,
// click-through, never activated, kept off the taskbar) and runs its message loop.
static DWORD WINAPI OvThread(LPVOID) {
    OvLoadArt();
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = OvProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"TS3CoopStatus";
    RegisterClassExW(&wc);
    g_ovFont = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_ovWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              L"TS3CoopStatus", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (!g_ovWnd) return 0;
    // Per-pixel alpha (UpdateLayeredWindow) for the game-styled banner; one alpha for the
    // whole window for the plain box. A layered window can only use one of the two.
    if (!g_ovArt) SetLayeredWindowAttributes(g_ovWnd, 0, 235, LWA_ALPHA);
    SetTimer(g_ovWnd, 1, 100, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

// Run once, by the first Status(): the lock and the overlay thread.
static BOOL CALLBACK OvInit(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_ovLock);
    if (HANDLE t = CreateThread(NULL, 0, OvThread, NULL, 0, NULL)) CloseHandle(t);
    return TRUE;
}

// Show a status line for ms milliseconds (0 = until replaced). Not logged here: callers
// that want it in the log write their own line.
// printf-style (wide); the first line is the headline. Any thread; returns at once.
// The accent colour picks the icon (kOvGreen, kOvAmber, kOvRed; any other, such as kOvBlue,
// the co-op one), and a headline with
// "..." in it gets the spinning wait ring. Calling again with the same text and colour
// only restarts its time (no redraw).
void Status(DWORD ms, COLORREF accent, const wchar_t* fmt, ...) {
    wchar_t text[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_net.testInputSeed && g_localTest < 0) return;   // automated tests: no window
    InitOnceExecuteOnce(&g_ovOnce, OvInit, NULL, NULL);
    EnterCriticalSection(&g_ovLock);
    if (wcscmp(g_ovText, text) != 0 || g_ovAccent != accent) {   // some callers repeat every frame: redraw only on change
        wcscpy_s(g_ovText, text);
        g_ovAccent = accent;
        g_ovSeq++;
    }
    g_ovUntil = ms ? GetTickCount64() + ms : ~0ULL;
    LeaveCriticalSection(&g_ovLock);
}

// A notice the player can see (the log is only read afterwards). Shown on
// its own thread so the game keeps running behind it.
void ShowNotice(const wchar_t* text) { Status(30000, kOvAmber, L"%s", text); }

// screens_assets.h -- Edition des graphiques d'ecran supplementaires :
// - Textes ASCII ecrans (screen_imaincdc, screen_gameovertxt, screen_pausedtxt)
//   : meme encodage @=espace/0xFF/0xFE que screen_imaptext
// - Bitmaps 4bpp (pic_congrats, pic_haf, pic_splash)
//   : memes nibble-packed U32 que tiles, palette fixe ST 16 couleurs
//
// Le chargement depuis un vrai binaire xrick (ELF32) utilise les symboles
// ELF existants via find_symbol_file_offset().
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "screens_assets_data.h" // U8 screen_imaincdc[], screen_gameovertxt[], screen_pausedtxt[]
}
#include "xrick_patch.h" // find_symbol_file_offset, patch_symbol
#include "mapdata.h"     // RED[], GREEN[], BLUE[]

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ASCII Text Screens (same encoding as screen_imaptext)
// ---------------------------------------------------------------------------

struct AsciiTextScreen
{
    std::string text; // editable, @ = space, real display chars
};

// Decode raw ASCII screen (same as parseImapText in screens_text.h but
// returning a single concatenated string with newlines for 0xFF).
inline AsciiTextScreen decodeAsciiTextScreen(const uint8_t *raw, size_t rawSize)
{
    AsciiTextScreen ats;
    std::string cur;
    for (size_t i = 0; i < rawSize; i++)
    {
        uint8_t c = raw[i];
        if (c == 0xFE) break;
        if (c == 0xFF)
        {
            ats.text += cur;
            ats.text += '\n';
            cur.clear();
            while (i + 1 < rawSize && raw[i + 1] == 0xFF) { i++; ats.text += '\n'; }
        }
        else
            cur.push_back(c == '@' ? ' ' : (char)c);
    }
    ats.text += cur;
    return ats;
}

// Encode back to raw format.
inline std::vector<uint8_t> encodeAsciiTextScreen(const AsciiTextScreen &ats)
{
    std::vector<uint8_t> bytes;
    std::string line;
    for (char ch : ats.text)
    {
        if (ch == '\n')
        {
            for (char c : line)
                bytes.push_back((uint8_t)(c == ' ' ? '@' : c));
            bytes.push_back(0xFF);
            line.clear();
        }
        else
            line.push_back(ch);
    }
    if (!line.empty())
    {
        for (char c : line)
            bytes.push_back((uint8_t)(c == ' ' ? '@' : c));
    }
    bytes.push_back(0xFE);
    return bytes;
}

// Default texts from compiled-in data.
inline AsciiTextScreen defaultImainCDC() { return decodeAsciiTextScreen(screen_imaincdc, SCREEN_IMAINCDC_SIZE); }
inline AsciiTextScreen defaultGameoverTxt() { return decodeAsciiTextScreen(screen_gameovertxt, SCREEN_GAMEOVERTXT_SIZE); }
inline AsciiTextScreen defaultPausedTxt() { return decodeAsciiTextScreen(screen_pausedtxt, SCREEN_PAUSEDTXT_SIZE); }

static const char *ASCII_TEXT_SCREEN_SYMBOLS[] = {
    "screen_imaincdc", "screen_gameovertxt", "screen_pausedtxt"
};
static const char *ASCII_TEXT_SCREEN_LABELS[] = {
    "Copyright / Press Space", "Game Over", "Paused"
};
static const size_t ASCII_TEXT_SCREEN_SIZES[] = {
    SCREEN_IMAINCDC_SIZE, SCREEN_GAMEOVERTXT_SIZE, SCREEN_PAUSEDTXT_SIZE
};

// ---------------------------------------------------------------------------
// Bitmap Pictures (4bpp nibble-packed U32, fixed ST 16-color palette)
// ---------------------------------------------------------------------------

struct BitmapPic
{
    int w = 0, h = 0;
    std::vector<uint32_t> pixels; // decoded RGBA (from decode_pic)

    bool empty() const { return w == 0 || h == 0; }
};

// Decode a 4bpp nibble-packed U32 array into RGBA pixels.
inline BitmapPic decode_pic(const uint32_t *raw, int w, int h)
{
    BitmapPic pic;
    pic.w = w;
    pic.h = h;
    pic.pixels.resize(w * h, 0xFF000000u);

    int strideU32 = (w + 7) / 8;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            int wordIdx = y * strideU32 + x / 8;
            int shift = 28 - (x % 8) * 4;
            int c = (raw[wordIdx] >> shift) & 0xF;
            uint8_t r = (uint8_t)RED[c];
            uint8_t g = (uint8_t)GREEN[c];
            uint8_t b = (uint8_t)BLUE[c];
            pic.pixels[y * w + x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (0xFFu << 24);
        }
    }
    return pic;
}

// Encode RGBA pixels back to nibble-packed U32 array.
inline std::vector<uint32_t> encode_pic(const BitmapPic &pic)
{
    int strideU32 = (pic.w + 7) / 8;
    std::vector<uint32_t> raw(strideU32 * pic.h, 0);
    for (int y = 0; y < pic.h; y++)
    {
        for (int x = 0; x < pic.w; x++)
        {
            uint32_t px = pic.pixels[y * pic.w + x];
            uint8_t pr = px & 0xFF, pg = (px >> 8) & 0xFF, pb = (px >> 16) & 0xFF;
            int bestC = 0;
            int bestDist = INT32_MAX;
            for (int c = 0; c < 16; c++)
            {
                int dr = (int)pr - (int)RED[c];
                int dg = (int)pg - (int)GREEN[c];
                int db = (int)pb - (int)BLUE[c];
                int dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) { bestDist = dist; bestC = c; }
            }
            int wordIdx = y * strideU32 + x / 8;
            int shift = 28 - (x % 8) * 4;
            raw[wordIdx] |= ((uint32_t)bestC & 0xF) << shift;
        }
    }
    return raw;
}

// Symbol names and sizes for the pics.
static const char *PIC_SYMBOLS[] = { "pic_congrats", "pic_haf", "pic_splash" };
static const char *PIC_LABELS[] = { "Congratulations background", "Hall of Fame background", "Title screen (splash)" };
static const int PIC_W[] = { PIC_CONGRATS_W, PIC_HAF_W, PIC_SPLASH_W };
static const int PIC_H[] = { PIC_CONGRATS_H, PIC_HAF_H, PIC_SPLASH_H };
static const int PIC_U32_SIZES[] = { PIC_CONGRATS_W * PIC_CONGRATS_H / 8, PIC_HAF_W * PIC_HAF_H / 8, PIC_SPLASH_W * PIC_SPLASH_H / 8 };
static const int PIC_COUNT = 3;

// In-memory pic data (loaded from xrick binary).
struct PicData
{
    BitmapPic pics[PIC_COUNT]; // [0]=congrats, [1]=haf, [2]=splash
    bool loaded[PIC_COUNT] = {};

    void reset()
    {
        for (int i = 0; i < PIC_COUNT; i++) { pics[i] = BitmapPic(); loaded[i] = false; }
    }
};

// Load all pics from an xrick binary via ELF symbols.
inline bool loadPicsFromXrickBinary(const fs::path &path, PicData &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    bool anyOk = false;
    for (int i = 0; i < PIC_COUNT; i++)
    {
        size_t off = 0, symSize = 0;
        std::string lerr;
        if (!find_symbol_file_offset(buf, PIC_SYMBOLS[i], off, symSize, lerr))
            continue;
        if (symSize < (size_t)PIC_U32_SIZES[i] * 4) continue;
        out.pics[i] = decode_pic(reinterpret_cast<const uint32_t*>(buf.data() + off), PIC_W[i], PIC_H[i]);
        out.loaded[i] = true;
        anyOk = true;
    }
    if (!anyOk)
        err = "No bitmap pictures found in the binary (pic_congrats, pic_haf, pic_splash)";
    return anyOk;
}

// Load a single pic by index from an xrick binary.
inline bool loadPicFromXrickBinary(const fs::path &path, int picIdx, BitmapPic &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t off = 0, symSize = 0;
    if (!find_symbol_file_offset(buf, PIC_SYMBOLS[picIdx], off, symSize, err)) return false;
    if (symSize < (size_t)PIC_U32_SIZES[picIdx] * 4)
    { err = std::string(PIC_SYMBOLS[picIdx]) + " too small in the binary"; return false; }
    out = decode_pic(reinterpret_cast<const uint32_t*>(buf.data() + off), PIC_W[picIdx], PIC_H[picIdx]);
    return true;
}

// Import a bitmap pic from an image file (PNG/BMP/JPG/TGA).
// Resamples to target dimensions using nearest-neighbor, quantizes
// to the 16-color game palette, stores RGBA in out.
inline bool importPicFromImage(const fs::path &path, BitmapPic &out, int targetW, int targetH, std::string &err)
{
    std::vector<uint8_t> fileData;
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) { err = "Could not open " + path.string(); return false; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); err = "File is empty"; return false; }
    fileData.resize((size_t)sz);
    bool rd = std::fread(fileData.data(), 1, (size_t)sz, f) == (size_t)sz;
    std::fclose(f);
    if (!rd) { err = "Read error"; return false; }

    int srcW = 0, srcH = 0;
    unsigned char *pixels = stbi_load_from_memory(fileData.data(), (int)fileData.size(), &srcW, &srcH, nullptr, 4);
    if (!pixels) { err = "Could not decode image: "; err += stbi_failure_reason(); return false; }

    out.w = targetW;
    out.h = targetH;
    out.pixels.resize(targetW * targetH, 0xFF000000u);

    for (int dy = 0; dy < targetH; dy++)
    {
        for (int dx = 0; dx < targetW; dx++)
        {
            int sx = dx * srcW / targetW;
            int sy = dy * srcH / targetH;
            unsigned char *px = &pixels[(sy * srcW + sx) * 4];
            // Find closest palette color
            int bestC = 0, bestD = INT32_MAX;
            for (int c = 0; c < 16; c++)
            {
                int dr = (int)px[0] - (int)RED[c];
                int dg = (int)px[1] - (int)GREEN[c];
                int db = (int)px[2] - (int)BLUE[c];
                int d = dr * dr + dg * dg + db * db;
                if (d < bestD) { bestD = d; bestC = c; }
            }
            uint8_t r = (uint8_t)RED[bestC];
            uint8_t g = (uint8_t)GREEN[bestC];
            uint8_t b = (uint8_t)BLUE[bestC];
            out.pixels[dy * targetW + dx] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (0xFFu << 24);
        }
    }

    stbi_image_free(pixels);
    return true;
}

// ---------------------------------------------------------------------------
// Asset state bundle for the Assets Editor
// ---------------------------------------------------------------------------

struct AssetsEditorState
{
    bool open = false;

    // Bank 0 tile grid (for quick import/edit/delete of individual tiles)
    int selectedTile = -1;  // -1 = nothing selected yet
    int batchStartTile = 0; // "Batch import..." starting tile index

    // ASCII text screens
    AsciiTextScreen asciiScreens[3]; // [0]=copyright, [1]=gameover, [2]=paused

    // Bitmap pictures
    PicData pics;

    // Which pic is selected for detail view
    int selectedPic = -1;
    int exportPicIdx = -1;
    int importPicIdx = -1;

    void loadDefaults()
    {
        selectedTile = -1;
        batchStartTile = 0;
        asciiScreens[0] = defaultImainCDC();
        asciiScreens[1] = defaultGameoverTxt();
        asciiScreens[2] = defaultPausedTxt();
        pics.reset();
        selectedPic = -1;
    }
};

// Load all assets from an xrick binary (ASCII texts + pics).
inline bool loadAssetsFromXrickBinary(const fs::path &path, AssetsEditorState &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    // ASCII text screens
    for (int i = 0; i < 3; i++)
    {
        size_t off = 0, symSize = 0;
        std::string lerr;
        if (!find_symbol_file_offset(buf, ASCII_TEXT_SCREEN_SYMBOLS[i], off, symSize, lerr))
        { err = "Could not find " + std::string(ASCII_TEXT_SCREEN_SYMBOLS[i]) + ": " + lerr; return false; }
        out.asciiScreens[i] = decodeAsciiTextScreen(buf.data() + off, symSize);
    }

    // Bitmap pictures (best effort -- don't fail if any is missing)
    std::string picErr;
    loadPicsFromXrickBinary(path, out.pics, picErr);

    return true;
}

// tile_import.h -- import d'une tuile graphique 8x8 depuis un fichier
// image (PNG/BMP/JPG/TGA/...) via stb_image, avec quantification vers
// la palette 16 couleurs du jeu (RED/GREEN/BLUE, cf. mapdata.h) puis
// ré-encodage au format tile_t (8 x U32, cf. tiles.h et decode_tile()
// dans tiles_render.h -- ce fichier fait exactement l'inverse).
//
// L'image source n'a pas besoin de faire 8x8 pile : elle est
// ré-échantillonnée (moyenne des pixels source par case de la grille
// 8x8 destination, "box filter") avant quantification, ce qui la
// redimensionne proprement dans les deux sens.
#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tiles.h" // tile_t
#include "mapdata.h" // RED[], GREEN[], BLUE[]
#include "xrick_patch.h" // find_symbol_file_offset, patch_symbol

#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#include "stb/stb_image.h"

namespace fs = std::filesystem;

// Lit un fichier entier en memoire. Utilise par tout ce qui charge une
// image ou un binaire ELF dans ce header (stb_image et
// find_symbol_file_offset veulent un buffer en memoire, pas un
// chemin -- voir la remarque UTF-8/fs::path plus bas).
inline bool readWholeFile(const fs::path &path, std::vector<uint8_t> &out, std::string &err)
{
    // On lit nous-memes le fichier (plutot que stbi_load qui prend un
    // chemin C) pour rester coherent avec le filesystem deja utilise
    // ailleurs (fs::path gere l'UTF-8/les caracteres non-ASCII mieux que
    // de passer le chemin brut a fopen() sous Windows -- pas un souci
    // ici sous Linux, mais autant garder une seule facon de lire un
    // fichier dans tout l'editeur).
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) { err = "Could not open " + path.string(); return false; }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); err = "File is empty or unreadable"; return false; }
    out.resize((size_t)size);
    bool ok = std::fread(out.data(), 1, (size_t)size, f) == (size_t)size;
    std::fclose(f);
    if (!ok) { err = "Read error"; return false; }
    return true;
}

// Plus proche couleur de la palette (indices 0-15 seulement -- les
// couleurs 16-31 de mapdata.h sont les "cheat colors", jamais
// atteignables par une tuile puisque son encodage ne stocke qu'un
// chiffre hexadecimal, 0-F, par pixel).
inline int nearestPaletteIndex(int r, int g, int b)
{
    int best = 0;
    long bestDist = LONG_MAX;
    for (int i = 0; i < 16; i++)
    {
        long dr = r - RED[i], dg = g - GREEN[i], db = b - BLUE[i];
        long d = dr * dr + dg * dg + db * db;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// Quantifie un carre 8x8 de pixels RGBA (deja extrait/prepare par
// l'appelant) vers la palette, et l'encode dans outTile. Coeur partage
// par l'import d'une tuile isolee (avec rééchantillonnage, voir plus
// bas) et l'import par lot (crop exact, aucun rééchantillonnage).
// `stride` = nombre d'octets entre deux lignes source dans `pixels8x8`
// (permet de lire directement une sous-region d'une image plus grande
// sans la copier d'abord).
inline void encodeTileFromRGBA8x8(const unsigned char *pixels, int stride, tile_t &outTile)
{
    uint8_t indices[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            const unsigned char *p = pixels + y * stride + x * 4;
            indices[y * 8 + x] = (uint8_t)nearestPaletteIndex(p[0], p[1], p[2]);
        }
    // Ré-encodage : le pixel le plus a gauche de la ligne devient le
    // chiffre hexadecimal le plus significatif de la valeur U32 de
    // cette ligne, et ainsi de suite jusqu'au pixel le plus a droite
    // (chiffre le moins significatif) -- c'est exactement l'inverse de
    // la boucle de decode_tile() dans tiles_render.h.
    for (int row = 0; row < 8; row++)
    {
        uint32_t val = 0;
        for (int col = 0; col < 8; col++)
            val = (val << 4) | indices[row * 8 + col];
        outTile[row] = val;
    }
}

// Charge une image depuis le disque, la ré-échantillonne en 8x8 (moyenne
// par case) et quantifie chaque case vers la palette. Ecrit directement
// dans outTile (tiles_data[bank][idx], typiquement) -- ne touche rien si
// ça échoue.
inline bool importTileFromImage(const fs::path &path, tile_t &outTile, std::string &err)
{
    std::vector<uint8_t> buf;
    if (!readWholeFile(path, buf, err)) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &channels, 4);
    if (!data) { err = std::string("Could not decode image: ") + stbi_failure_reason(); return false; }
    if (w <= 0 || h <= 0) { stbi_image_free(data); err = "Image has no pixels"; return false; }

    uint8_t indices[64];
    for (int ty = 0; ty < 8; ty++)
    {
        int sy0 = ty * h / 8;
        int sy1 = std::max(sy0 + 1, (ty + 1) * h / 8);
        for (int tx = 0; tx < 8; tx++)
        {
            int sx0 = tx * w / 8;
            int sx1 = std::max(sx0 + 1, (tx + 1) * w / 8);
            long sr = 0, sg = 0, sb = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < h; sy++)
            {
                for (int sx = sx0; sx < sx1 && sx < w; sx++)
                {
                    const unsigned char *p = data + ((size_t)sy * w + sx) * 4;
                    sr += p[0]; sg += p[1]; sb += p[2];
                    n++;
                }
            }
            int r = n ? (int)(sr / n) : 0, g = n ? (int)(sg / n) : 0, b = n ? (int)(sb / n) : 0;
            indices[ty * 8 + tx] = (uint8_t)nearestPaletteIndex(r, g, b);
        }
    }
    stbi_image_free(data);

    for (int row = 0; row < 8; row++)
    {
        uint32_t val = 0;
        for (int col = 0; col < 8; col++)
            val = (val << 4) | indices[row * 8 + col];
        outTile[row] = val;
    }

    err.clear();
    return true;
}

// Vrai si tous les pixels RGBA d'une case w x h sont strictement
// identiques (meme rouge/vert/bleu/alpha) -- utilise par les imports
// par lot (tuiles et sprites) pour detecter une case "vide" (fond uni,
// ou entierement transparente) et l'ignorer plutot que d'ecraser une
// tuile/un sprite existant avec du contenu vide.
inline bool isCellUniformRGBA(const unsigned char *pixels, int stride, int w, int h)
{
    const unsigned char *first = pixels;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const unsigned char *p = pixels + (size_t)y * stride + (size_t)x * 4;
            if (p[0] != first[0] || p[1] != first[1] || p[2] != first[2] || p[3] != first[3])
                return false;
        }
    return true;
}

// Resultat d'un import par lot -- rempli meme si imported==0, pour que
// l'appelant puisse afficher un recapitulatif utile dans tous les cas
// (y compris "tout etait deja hors plage").
struct BatchImportResult
{
    int cols = 0, rows = 0;         // grille de tuiles 8x8 detectee dans l'image
    int leftoverPixelsX = 0;        // pixels ignores a droite (largeur pas multiple de 8)
    int leftoverPixelsY = 0;        // pixels ignores en bas (hauteur pas multiple de 8)
    int startTile = 0;
    int imported = 0;               // nombre de tuiles effectivement ecrites
    int endTile = -1;               // dernier index du lot traite (ecrit ou ignore comme case vide), -1 si aucun
    int skippedOverflow = 0;        // cases de la grille qui auraient depasse la tuile 255
    int skippedUniform = 0;         // cases "vides" (tous pixels identiques) ignorees, dans la plage traitee
};

// Importe TOUTE une image comme une grille de tuiles 8x8, sans aucun
// rééchantillonnage (contrairement a importTileFromImage ci-dessus) :
// chaque case 8x8 exacte de l'image devient une tuile, dans l'ordre
// naturel de lecture (gauche->droite, puis haut->bas), a partir de
// startTile et en avançant d'un cran par case. S'arrete a la tuile 255
// (une seule banque a la fois -- pas de debordement vers la banque
// suivante). Une case dont tous les pixels sont identiques (fond uni ou
// entierement transparent -- typiquement une case vide d'une planche de
// sprites/tuiles) est ignoree : rien n'est ecrit pour cette tuile, elle
// garde son contenu precedent, et elle compte dans skippedUniform plutot
// que dans imported. N'ecrit rien si le fichier ne peut pas etre charge ;
// ecrit direct dans tiles_data[bank][...] pour chaque case non-vide du
// lot traite sinon (comme le reste de ce fichier -- l'appelant
// reconstruit les atlas et gere st.dirty).
inline bool importTilesBatchFromImage(const fs::path &path, int bank, int startTile, BatchImportResult &result, std::string &err)
{
    std::vector<uint8_t> buf;
    if (!readWholeFile(path, buf, err)) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &channels, 4);
    if (!data) { err = std::string("Could not decode image: ") + stbi_failure_reason(); return false; }
    if (w < 8 || h < 8) { stbi_image_free(data); err = "Image is smaller than a single 8x8 tile"; return false; }

    result = BatchImportResult{};
    result.startTile = startTile;
    result.cols = w / 8;
    result.rows = h / 8;
    result.leftoverPixelsX = w - result.cols * 8;
    result.leftoverPixelsY = h - result.rows * 8;

    int totalCells = result.cols * result.rows;
    int available = (startTile <= 255) ? (256 - startTile) : 0;
    int toImport = std::min(totalCells, available);
    result.skippedOverflow = totalCells - toImport;

    int stride = w * 4;
    int nextTile = startTile;
    for (int n = 0; n < toImport; n++)
    {
        int r = n / result.cols, c = n % result.cols;
        const unsigned char *cellStart = data + (size_t)(r * 8) * stride + (size_t)(c * 8) * 4;
        if (isCellUniformRGBA(cellStart, stride, 8, 8)) { result.skippedUniform++; continue; }
        if (nextTile > 255) { result.skippedOverflow++; continue; }
        encodeTileFromRGBA8x8(cellStart, stride, tiles_data[bank][nextTile]);
        nextTile++;
        result.imported++;
    }
    stbi_image_free(data);

    result.endTile = nextTile > startTile ? nextTile - 1 : -1;
    err.clear();
    return true;
}

// Reads tiles_data (all TILES_NBR_BANKS banks -- 0, 1 and 2 -- straight
// from a real xrick binary's own compiled-in data, via its ELF symbol)
// into the live global array. Same "trust the real binary over our
// compiled-in default" pattern as loadEflgFromXrickBinary() in
// xrick_eflg.h and loadXrickConnections()/loadXrickMarks() in
// xrick_levels.h/xrick_marks.h -- offered alongside those from the same
// "Import ... from xrick binary..." menu action. Caller is responsible
// for rebuilding the tile/block atlas textures afterwards (needs the
// SDL renderer, not available in this header).
inline bool loadTilesFromXrickBinary(const fs::path &path, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    size_t off = 0, size = 0;
    if (!find_symbol_file_offset(buf, "tiles_data", off, size, err)) return false;
    size_t expected = sizeof(tiles_data);
    if (size != expected)
    {
        err = "tiles_data has a different size (" + std::to_string(size)
            + " bytes, expected " + std::to_string(expected) + ") -- likely an incompatible xrick build.";
        return false;
    }
    if (off + expected > buf.size()) { err = "tiles_data location falls outside the file"; return false; }
    std::memcpy(tiles_data, buf.data() + off, expected);
    err.clear();
    return true;
}

// Reads map_blocks (which tile goes in which of the 16 cells of each of
// the 256 blocks) straight from a real xrick binary's own compiled-in
// data, via its ELF symbol, into the live global array. The real binary
// stores each entry as a single byte (see map_blocks_as_bytes() in
// xrick_patch.h for why) -- widened to this editor's own `int` here.
// Same "trust the real binary" pattern as loadTilesFromXrickBinary()
// above.
inline bool loadBlocksFromXrickBinary(const fs::path &path, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    size_t off = 0, size = 0;
    if (!find_symbol_file_offset(buf, "map_blocks", off, size, err)) return false;
    size_t expected = 0x100 * 16; // block_t = U8[16] in the real engine
    if (size != expected)
    {
        err = "map_blocks has a different size (" + std::to_string(size)
            + " bytes, expected " + std::to_string(expected) + ") -- likely an incompatible xrick build.";
        return false;
    }
    if (off + expected > buf.size()) { err = "map_blocks location falls outside the file"; return false; }
    for (int b = 0; b < 0x100; b++)
        for (int i = 0; i < 16; i++)
            map_blocks[b][i] = buf[off + b * 16 + i];
    err.clear();
    return true;
}

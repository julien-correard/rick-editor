// sprite_import.h -- import de sprites graphiques (32x21 pixels) depuis
// un fichier image, en simple (avec rééchantillonnage) ou en lot (crop
// exact, sans rééchantillonnage) -- miroir de tile_import.h, pour le
// meme genre de workflow que les tuiles mais applique aux sprites.
//
// Difference cle avec les tuiles : l'indice de palette 0 signifie
// TRANSPARENT pour un sprite (draw_sprite() n'ecrit pas le pixel quand
// le nibble vaut 0 -- voir sprites_render.h), et non une couleur opaque
// comme pour les tuiles. Un pixel opaque de l'image source est donc
// quantifie vers l'un des indices 1-15 seulement (jamais 0), et un
// pixel avec un canal alpha faible devient transparent (indice 0).
#pragma once

#include "tile_import.h"    // readWholeFile(), RED/GREEN/BLUE, stb_image (deja inclus + implemente)
#include "sprites_render.h" // sprite_t (via sprites.h), SPRITE_W/SPRITE_H, SPRITES_NBR_SPRITES

// En dessous de ce seuil alpha (0-255), un pixel source devient
// transparent (indice 0) plutot que d'etre quantifie vers une couleur.
constexpr int SPRITE_ALPHA_THRESHOLD = 128;

// Plus proche couleur de la palette parmi les indices OPAQUES (1-15
// seulement -- l'indice 0 ne peut jamais representer un pixel opaque
// pour un sprite, contrairement aux tuiles).
inline int nearestSpritePaletteIndex(int r, int g, int b)
{
    int best = 1;
    long bestDist = LONG_MAX;
    for (int i = 1; i < 16; i++)
    {
        long dr = r - RED[i], dg = g - GREEN[i], db = b - BLUE[i];
        long d = dr * dr + dg * dg + db * db;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// Quantifie un rectangle SPRITE_W x SPRITE_H de pixels RGBA (deja
// extrait/prepare par l'appelant, `stride` = octets entre deux lignes)
// vers la palette + transparence, et l'encode dans outSprite. Coeur
// partage par l'import simple (avec rééchantillonnage, plus bas) et
// l'import par lot (crop exact).
inline void encodeSpriteFromRGBA(const unsigned char *pixels, int stride, sprite_t &outSprite)
{
    uint8_t indices[SPRITE_W * SPRITE_H];
    for (int y = 0; y < SPRITE_H; y++)
        for (int x = 0; x < SPRITE_W; x++)
        {
            const unsigned char *p = pixels + y * stride + x * 4;
            indices[y * SPRITE_W + x] = (p[3] < SPRITE_ALPHA_THRESHOLD)
                ? 0 : (uint8_t)nearestSpritePaletteIndex(p[0], p[1], p[2]);
        }
    // Meme sens d'encodage que les tuiles (voir tile_import.h) : le
    // pixel le plus a gauche d'un groupe de 8 devient le chiffre
    // hexadecimal le plus significatif du U32 de ce groupe -- inverse
    // exact de la boucle de decode_sprite() dans sprites_render.h.
    for (int row = 0; row < SPRITE_H; row++)
        for (int col4 = 0; col4 < 4; col4++)
        {
            uint32_t val = 0;
            for (int k = 0; k < 8; k++)
                val = (val << 4) | indices[row * SPRITE_W + col4 * 8 + k];
            outSprite[row][col4] = val;
        }
}

// Charge une image depuis le disque, la ré-échantillonne en 32x21
// (moyenne par case, y compris le canal alpha) et quantifie vers la
// palette + transparence. Ecrit directement dans outSprite.
inline bool importSpriteFromImage(const fs::path &path, sprite_t &outSprite, std::string &err)
{
    std::vector<uint8_t> buf;
    if (!readWholeFile(path, buf, err)) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &channels, 4);
    if (!data) { err = std::string("Could not decode image: ") + stbi_failure_reason(); return false; }
    if (w <= 0 || h <= 0) { stbi_image_free(data); err = "Image has no pixels"; return false; }

    uint8_t indices[SPRITE_W * SPRITE_H];
    for (int ty = 0; ty < SPRITE_H; ty++)
    {
        int sy0 = ty * h / SPRITE_H;
        int sy1 = std::max(sy0 + 1, (ty + 1) * h / SPRITE_H);
        for (int tx = 0; tx < SPRITE_W; tx++)
        {
            int sx0 = tx * w / SPRITE_W;
            int sx1 = std::max(sx0 + 1, (tx + 1) * w / SPRITE_W);
            long sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < h; sy++)
                for (int sx = sx0; sx < sx1 && sx < w; sx++)
                {
                    const unsigned char *p = data + ((size_t)sy * w + sx) * 4;
                    sr += p[0]; sg += p[1]; sb += p[2]; sa += p[3];
                    n++;
                }
            int r = n ? (int)(sr / n) : 0, g = n ? (int)(sg / n) : 0, b = n ? (int)(sb / n) : 0, a = n ? (int)(sa / n) : 0;
            indices[ty * SPRITE_W + tx] = (a < SPRITE_ALPHA_THRESHOLD) ? 0 : (uint8_t)nearestSpritePaletteIndex(r, g, b);
        }
    }
    stbi_image_free(data);

    for (int row = 0; row < SPRITE_H; row++)
        for (int col4 = 0; col4 < 4; col4++)
        {
            uint32_t val = 0;
            for (int k = 0; k < 8; k++)
                val = (val << 4) | indices[row * SPRITE_W + col4 * 8 + k];
            outSprite[row][col4] = val;
        }

    err.clear();
    return true;
}

// Resultat d'un import par lot -- rempli meme si imported==0 (meme
// convention que BatchImportResult dans tile_import.h).
struct SpriteBatchImportResult
{
    int cols = 0, rows = 0;         // grille de sprites 32x21 detectee dans l'image
    int leftoverPixelsX = 0;        // pixels ignores a droite (largeur pas multiple de 32)
    int leftoverPixelsY = 0;        // pixels ignores en bas (hauteur pas multiple de 21)
    int startSprite = 0;
    int imported = 0;               // nombre de sprites effectivement ecrits
    int endSprite = -1;             // dernier index du lot traite (ecrit ou ignore comme case vide), -1 si aucun
    int skippedOverflow = 0;        // cases de la grille qui auraient depasse le sprite 212
    int skippedUniform = 0;         // cases "vides" (pixels identiques ou entierement transparentes) ignorees
};

// Vrai si tous les pixels d'une case ont un alpha sous le seuil de
// transparence des sprites -- une case "vide" en PNG a souvent un RGB
// residuel incoherent sous la transparence (artefact d'export courant),
// donc l'egalite stricte de isCellUniformRGBA() ne suffit pas a la
// detecter ; ce test-ci la complete pour les sprites uniquement (les
// tuiles n'ont pas de notion de transparence).
inline bool isCellFullyTransparent(const unsigned char *pixels, int stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const unsigned char *p = pixels + (size_t)y * stride + (size_t)x * 4;
            if (p[3] >= SPRITE_ALPHA_THRESHOLD) return false;
        }
    return true;
}

// Importe TOUTE une image comme une grille de sprites 32x21, sans
// aucun rééchantillonnage : chaque case exacte devient un sprite, dans
// l'ordre naturel de lecture (gauche->droite, puis haut->bas), a partir
// de startSprite. S'arrete au dernier sprite (SPRITES_NBR_SPRITES-1) --
// meme logique que importTilesBatchFromImage() dans tile_import.h, sans
// notion de banque ici (il n'y a qu'un seul tableau sprites_data). Une
// case "vide" (tous les pixels strictement identiques, OU entierement
// transparente) est ignoree : rien n'est ecrit pour ce sprite, il garde
// son contenu precedent, et elle compte dans skippedUniform plutot que
// dans imported.
inline bool importSpritesBatchFromImage(const fs::path &path, int startSprite, SpriteBatchImportResult &result, std::string &err)
{
    std::vector<uint8_t> buf;
    if (!readWholeFile(path, buf, err)) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &channels, 4);
    if (!data) { err = std::string("Could not decode image: ") + stbi_failure_reason(); return false; }
    if (w < SPRITE_W || h < SPRITE_H)
    { stbi_image_free(data); err = "Image is smaller than a single 32x21 sprite"; return false; }

    result = SpriteBatchImportResult{};
    result.startSprite = startSprite;
    result.cols = w / SPRITE_W;
    result.rows = h / SPRITE_H;
    result.leftoverPixelsX = w - result.cols * SPRITE_W;
    result.leftoverPixelsY = h - result.rows * SPRITE_H;

    int totalCells = result.cols * result.rows;
    int available = (startSprite <= SPRITES_NBR_SPRITES - 1) ? (SPRITES_NBR_SPRITES - startSprite) : 0;
    int toImport = std::min(totalCells, available);
    result.skippedOverflow = totalCells - toImport;

    int stride = w * 4;
    int nextSprite = startSprite;
    for (int n = 0; n < toImport; n++)
    {
        int r = n / result.cols, c = n % result.cols;
        const unsigned char *cellStart = data + (size_t)(r * SPRITE_H) * stride + (size_t)(c * SPRITE_W) * 4;
        if (isCellUniformRGBA(cellStart, stride, SPRITE_W, SPRITE_H) || isCellFullyTransparent(cellStart, stride, SPRITE_W, SPRITE_H))
        { result.skippedUniform++; continue; }
        if (nextSprite >= SPRITES_NBR_SPRITES) { result.skippedOverflow++; continue; }
        encodeSpriteFromRGBA(cellStart, stride, sprites_data[nextSprite]);
        nextSprite++;
        result.imported++;
    }
    stbi_image_free(data);

    result.endSprite = nextSprite > startSprite ? nextSprite - 1 : -1;
    err.clear();
    return true;
}

// Reads sprites_data straight from a real xrick binary's own
// compiled-in data, via its ELF symbol, into the live global array.
// Same "trust the real binary" pattern as loadTilesFromXrickBinary()
// and loadBlocksFromXrickBinary() in tile_import.h. Caller is
// responsible for rebuilding the sprite atlas texture afterwards
// (needs the SDL renderer, not available in this header).
inline bool loadSpritesFromXrickBinary(const fs::path &path, std::string &err)
{
    std::vector<uint8_t> buf;
    if (!readWholeFile(path, buf, err)) return false;

    size_t off = 0, size = 0;
    if (!elf32_find_symbol_file_offset(buf, "sprites_data", off, size, err)) return false;
    size_t expected = sizeof(sprites_data);
    if (size != expected)
    {
        err = "sprites_data has a different size (" + std::to_string(size)
            + " bytes, expected " + std::to_string(expected) + ") -- likely an incompatible xrick build.";
        return false;
    }
    if (off + expected > buf.size()) { err = "sprites_data location falls outside the file"; return false; }
    std::memcpy(sprites_data, buf.data() + off, expected);
    err.clear();
    return true;
}

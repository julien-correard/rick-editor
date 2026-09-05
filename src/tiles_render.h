// tiles_render.h -- decodage des tuiles graphiques et construction des
// atlas de textures (tuiles et blocs) pour l'affichage accelere via
// SDL_Renderer. Code nouveau (pas issu du projet d'origine), mais qui
// reproduit fidelement l'algorithme de decodage original (voir
// decode_tile() ci-dessous) afin de garder un rendu pixel pour pixel
// identique a celui de l'ancien drawtile().
#pragma once

#include <SDL2/SDL.h>

extern "C" {
#include "tiles.h" // tile_t, tiles_data[][], TILES_NBR_BANKS
}
#include "mapdata.h" // RED[], GREEN[], BLUE[], map_blocks[]

// Cote d'une tuile en pixels (8x8, format d'origine).
constexpr int TILE_PX = 8;
// Une banque compte 256 tuiles ; on les range en grille 16x16 dans l'atlas.
constexpr int ATLAS_TILES_PER_ROW = 16;
constexpr int TILE_ATLAS_PX = ATLAS_TILES_PER_ROW * TILE_PX; // 128x128

// Un bloc = grille 4x4 de tuiles = 32x32 pixels. 256 blocs -> atlas 16x16
// blocs de 32x32 = 512x512.
constexpr int BLOCK_PX = 4 * TILE_PX; // 32
constexpr int ATLAS_BLOCKS_PER_ROW = 16;
constexpr int BLOCK_ATLAS_PX = ATLAS_BLOCKS_PER_ROW * BLOCK_PX; // 512x512

// --- RUxF unified tile/block space -----------------------------------
// A RUxF map addresses tiles and blocks as one unified space of 1024
// elements (4 pages of 256) -- no active bank anywhere: a block's cell
// stores the *absolute* tile index 0..1023, which selects the physical
// bank/tile directly. Bank 0 (font + cutscene decor) stays separate and
// is never addressed from a block. These helpers map an absolute game
// tile index back to its physical tiles_data bank/tile for when the
// per-page builders below are used.
constexpr int RUxF_GAME_PAGES = 4;          // pages 1..4 of 256 game tiles
constexpr int RUxF_PAGE_TILES = 0x100;      // 256 tiles per page
constexpr int RUxF_GAME_TILES = RUxF_GAME_PAGES * RUxF_PAGE_TILES; // 1024
constexpr int RUxF_BLOCKS = 0x400;          // 1024 blocks (4 pages of 256)
constexpr int FIRST_GAME_BANK = 1;          // physical banks 1..4 = pages 1..4

inline int gameTileToBank(int u) { return FIRST_GAME_BANK + u / RUxF_PAGE_TILES; }
inline int gameTileToOff (int u) { return u % RUxF_PAGE_TILES; }

// Unified (RUxF) atlas grid: 32x32 tiles of 8px = 256x256 for the 1024
// game tiles; 32x32 blocks of 32px = 1024x1024 for the 1024 blocks.
constexpr int UNI_TILES_PER_ROW = 32;
constexpr int UNI_TILE_ATLAS_PX = UNI_TILES_PER_ROW * TILE_PX;
constexpr int UNI_BLOCKS_PER_ROW = 32;
constexpr int UNI_BLOCK_ATLAS_PX = UNI_BLOCKS_PER_ROW * BLOCK_PX;

// Decode une tuile (bank, NbTile) en 64 pixels RGBA (ordre ligne par ligne,
// gauche a droite). Reprend exactement l'algorithme original de
// drawtile() : chaque unsigned long de tiles_data[bank][NbTile][ligne] est
// un nombre dont les chiffres hexadecimaux (une fois les zeros de poids
// fort restitues via le tableau pre-rempli a zero) donnent les 8 indices
// de couleur de la ligne.
inline void decode_tile(int bank, int NbTile, Uint32 out_rgba[TILE_PX * TILE_PX])
{
    unsigned long tile[8];
    for (int i = 0; i < 8; i++)
        tile[i] = tiles_data[bank][NbTile][i];

    int tiletab[64] = {0};
    for (int i = 0; i < 8; i++)
    {
        int z = 0;
        while (tile[i] != 0)
        {
            tiletab[(7 - z++) + (i * 8)] = tile[i] % 0x10;
            tile[i] /= 0x10;
        }
    }

    for (int i = 0; i < 64; i++)
    {
        int c = tiletab[i];
        Uint8 r = (Uint8)RED[c];
        Uint8 g = (Uint8)GREEN[c];
        Uint8 b = (Uint8)BLUE[c];
        // Couleur 0 = fond/transparence historique (comme dans l'editeur
        // d'origine, qui l'affichait en noir opaque) ; on la garde opaque
        // ici aussi pour un rendu identique, sauf que la valeur RVB de
        // l'indice 0 est deja (0,0,0) dans la palette d'origine.
        out_rgba[i] = (Uint32)r | ((Uint32)g << 8) | ((Uint32)b << 16) | (0xFFu << 24);
    }
}

// Construit une texture 128x128 (SDL_PIXELFORMAT_RGBA32) contenant les 256
// tuiles d'une banque, rangees en grille 16x16.
inline SDL_Texture* build_tile_atlas(SDL_Renderer* renderer, int bank)
{
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          TILE_ATLAS_PX, TILE_ATLAS_PX);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

    static Uint32 pixels[TILE_ATLAS_PX * TILE_ATLAS_PX];
    for (auto &p : pixels) p = 0xFF000000u; // fond noir opaque

    for (int t = 0; t < 0x100; t++)
    {
        Uint32 tile_px[TILE_PX * TILE_PX];
        decode_tile(bank, t, tile_px);
        int cx = (t % ATLAS_TILES_PER_ROW) * TILE_PX;
        int cy = (t / ATLAS_TILES_PER_ROW) * TILE_PX;
        for (int y = 0; y < TILE_PX; y++)
            for (int x = 0; x < TILE_PX; x++)
                pixels[(cy + y) * TILE_ATLAS_PX + (cx + x)] = tile_px[y * TILE_PX + x];
    }

    SDL_UpdateTexture(tex, nullptr, pixels, TILE_ATLAS_PX * sizeof(Uint32));
    return tex;
}

// Construit une texture 512x512 contenant un apercu (deja assemble) des
// 256 blocs de map_blocks[], pour la banque de tuiles donnee. Utilise un
// rendu vers texture (render target) a partir de l'atlas de tuiles.
inline SDL_Texture* build_block_atlas(SDL_Renderer* renderer, SDL_Texture* tileAtlas)
{
    SDL_Texture* blockAtlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_TARGET,
                                                 BLOCK_ATLAS_PX, BLOCK_ATLAS_PX);
    SDL_SetTextureBlendMode(blockAtlas, SDL_BLENDMODE_NONE);

    SDL_Texture* prevTarget = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, blockAtlas);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    for (int b = 0; b < 0x100; b++)
    {
        int bx = (b % ATLAS_BLOCKS_PER_ROW) * BLOCK_PX;
        int by = (b / ATLAS_BLOCKS_PER_ROW) * BLOCK_PX;
        for (int i = 0; i < 16; i++)
        {
            int NbTile = map_blocks[b][i];
            int tx = (NbTile % ATLAS_TILES_PER_ROW) * TILE_PX;
            int ty = (NbTile / ATLAS_TILES_PER_ROW) * TILE_PX;
            SDL_Rect src{tx, ty, TILE_PX, TILE_PX};
            // meme disposition 4x4 que l'ancien drawblock() : colonne =
            // i%4, ligne = i/4.
            SDL_Rect dst{bx + (i % 4) * TILE_PX, by + (i / 4) * TILE_PX, TILE_PX, TILE_PX};
            SDL_RenderCopy(renderer, tileAtlas, &src, &dst);
        }
    }

    SDL_SetRenderTarget(renderer, prevTarget);
    return blockAtlas;
}

// Unified (RUxF) tile atlas: all 1024 game tiles in one texture, composed
// from the physical per-page banks (banks 1-4).
inline SDL_Texture* build_tile_atlas_unified(SDL_Renderer* renderer)
{
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          UNI_TILE_ATLAS_PX, UNI_TILE_ATLAS_PX);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    static Uint32 pixels[UNI_TILE_ATLAS_PX * UNI_TILE_ATLAS_PX];
    for (auto &p : pixels) p = 0xFF000000u;
    for (int u = 0; u < RUxF_GAME_TILES; u++)
    {
        Uint32 tile_px[TILE_PX * TILE_PX];
        decode_tile(gameTileToBank(u), gameTileToOff(u), tile_px);
        int cx = (u % UNI_TILES_PER_ROW) * TILE_PX;
        int cy = (u / UNI_TILES_PER_ROW) * TILE_PX;
        for (int y = 0; y < TILE_PX; y++)
            for (int x = 0; x < TILE_PX; x++)
                pixels[(cy + y) * UNI_TILE_ATLAS_PX + (cx + x)] = tile_px[y * TILE_PX + x];
    }
    SDL_UpdateTexture(tex, nullptr, pixels, UNI_TILE_ATLAS_PX * sizeof(Uint32));
    return tex;
}

// Unified (RUxF) block atlas: all 1024 blocks in one texture, each cell
// holding an absolute tile index (0-1023) looked up in the unified tile
// atlas.
inline SDL_Texture* build_block_atlas_unified(SDL_Renderer* renderer, SDL_Texture* tileAtlasUni)
{
    SDL_Texture* blockAtlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_TARGET,
                                                 UNI_BLOCK_ATLAS_PX, UNI_BLOCK_ATLAS_PX);
    SDL_SetTextureBlendMode(blockAtlas, SDL_BLENDMODE_NONE);
    SDL_Texture* prevTarget = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, blockAtlas);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    for (int b = 0; b < RUxF_BLOCKS; b++)
    {
        int bx = (b % UNI_BLOCKS_PER_ROW) * BLOCK_PX;
        int by = (b / UNI_BLOCKS_PER_ROW) * BLOCK_PX;
        for (int i = 0; i < 16; i++)
        {
            int u = map_blocks[b][i];                 // absolute 0..1023
            int tx = (u % UNI_TILES_PER_ROW) * TILE_PX;
            int ty = (u / UNI_TILES_PER_ROW) * TILE_PX;
            SDL_Rect src{tx, ty, TILE_PX, TILE_PX};
            SDL_Rect dst{bx + (i % 4) * TILE_PX, by + (i / 4) * TILE_PX, TILE_PX, TILE_PX};
            SDL_RenderCopy(renderer, tileAtlasUni, &src, &dst);
        }
    }
    SDL_SetRenderTarget(renderer, prevTarget);
    return blockAtlas;
}

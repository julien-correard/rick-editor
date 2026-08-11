// sprites_render.h -- decodes real sprite graphics (sprites_data, from
// the vendored dat_spritesST.c) and builds an atlas texture, so entity
// markers can show actual game art instead of a placeholder dot.
//
// Format confirmed directly from the original source (draw.c,
// draw_sprite() under GFXST): each sprite is 21 rows x 4 U32 (32 pixels
// wide), read row-major. Same nibble-per-pixel packing as tiles (see
// tiles_render.h's decode_tile() for the "why this works" explanation),
// except nibble value 0 means TRANSPARENT here (draw_sprite() skips
// writing the pixel when `d & 0x0F` is zero), not opaque black.
#pragma once

#include <SDL2/SDL.h>
#include <vector>

extern "C" {
#include "sprites.h" // sprite_t, sprites_data[], SPRITES_NBR_SPRITES
}
#include "mapdata.h" // RED[], GREEN[], BLUE[]

constexpr int SPRITE_W = 32;
constexpr int SPRITE_H = 0x15; // 21
constexpr int SPRITE_ATLAS_PER_ROW = 16;
constexpr int SPRITE_ATLAS_W = SPRITE_ATLAS_PER_ROW * SPRITE_W;                         // 512
constexpr int SPRITE_ATLAS_ROWS = (SPRITES_NBR_SPRITES + SPRITE_ATLAS_PER_ROW - 1) / SPRITE_ATLAS_PER_ROW;
constexpr int SPRITE_ATLAS_H = SPRITE_ATLAS_ROWS * SPRITE_H;

// Decodes sprite `number` into 32*21 RGBA pixels (0 alpha = transparent).
inline void decode_sprite(int number, Uint32 out_rgba[SPRITE_W * SPRITE_H])
{
    for (int row = 0; row < SPRITE_H; row++)
    {
        for (int col4 = 0; col4 < 4; col4++)
        {
            U32 d = sprites_data[number][row][col4];
            for (int k = 7; k >= 0; k--, d >>= 4)
            {
                int c = d & 0xF;
                int px = col4 * 8 + k;
                Uint32 pixel;
                if (c == 0)
                    pixel = 0; // transparent
                else
                {
                    Uint8 r = (Uint8)RED[c], g = (Uint8)GREEN[c], b = (Uint8)BLUE[c];
                    pixel = (Uint32)r | ((Uint32)g << 8) | ((Uint32)b << 16) | (0xFFu << 24);
                }
                out_rgba[row * SPRITE_W + px] = pixel;
            }
        }
    }
}

// Builds one atlas texture containing every sprite (16 per row), with
// alpha blending enabled for transparency.
inline SDL_Texture* build_sprite_atlas(SDL_Renderer* renderer)
{
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          SPRITE_ATLAS_W, SPRITE_ATLAS_H);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    std::vector<Uint32> pixels(SPRITE_ATLAS_W * SPRITE_ATLAS_H, 0);
    for (int n = 0; n < SPRITES_NBR_SPRITES; n++)
    {
        Uint32 spr[SPRITE_W * SPRITE_H];
        decode_sprite(n, spr);
        int cx = (n % SPRITE_ATLAS_PER_ROW) * SPRITE_W;
        int cy = (n / SPRITE_ATLAS_PER_ROW) * SPRITE_H;
        for (int y = 0; y < SPRITE_H; y++)
            for (int x = 0; x < SPRITE_W; x++)
                pixels[(cy + y) * SPRITE_ATLAS_W + (cx + x)] = spr[y * SPRITE_W + x];
    }
    SDL_UpdateTexture(tex, nullptr, pixels.data(), SPRITE_ATLAS_W * sizeof(Uint32));
    return tex;
}

// UV rect (in the atlas) for sprite `number`.
inline void sprite_uv(int number, float &u0, float &v0, float &u1, float &v1)
{
    int cx = (number % SPRITE_ATLAS_PER_ROW);
    int cy = (number / SPRITE_ATLAS_PER_ROW);
    u0 = (float)(cx * SPRITE_W) / SPRITE_ATLAS_W;
    v0 = (float)(cy * SPRITE_H) / SPRITE_ATLAS_H;
    u1 = (float)(cx * SPRITE_W + SPRITE_W) / SPRITE_ATLAS_W;
    v1 = (float)(cy * SPRITE_H + SPRITE_H) / SPRITE_ATLAS_H;
}

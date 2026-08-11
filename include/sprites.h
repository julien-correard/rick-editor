/*
 * sprites.h -- minimal header, not present in the source archive provided
 * (only .c files were included, no headers). Reconstructed from usage:
 * dat_spritesST.c ("sprite_t sprites_data[SPRITES_NBR_SPRITES]", a flat
 * literal of 84 U32 per sprite) and draw.c's draw_sprite() for GFXST,
 * which reads sprites_data[number][g++] for g in 0..83 (21 rows * 4
 * U32/row) -- confirming sprite_t is a 21x4 array of U32, and that
 * SPRITES_NBR_SPRITES = 213 (matches both the source file's sprite count
 * and this project's xrick binary's sprites_data symbol size / 336).
 */
#ifndef _SPRITES_H
#define _SPRITES_H

#include "system.h"

#define SPRITES_NBR_SPRITES 213

typedef U32 sprite_t[0x15][4]; /* 21 rows x 4 U32 (32px wide) per sprite */

extern sprite_t sprites_data[SPRITES_NBR_SPRITES];

#endif

/* eof */

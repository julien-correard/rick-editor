/*
 * xrick/include/tiles.h
 *
 * Copyright (C) 1998-2002 BigOrno (bigorno@bigorno.net). All rights reserved.
 *
 * The use and distribution terms for this software are contained in the file
 * named README, which can be found in the root of this distribution. By
 * using this software in any fashion, you are agreeing to be bound by the
 * terms of this license.
 *
 * You must not remove this notice, or any other, from this software.
 */

/*
 * NOTES
 *
 * A tile consists in one column and 8 rows of 8 U16 (cga encoding, two
 * bits per pixel). The tl_tiles array contains all tiles, with the
 * following structure:
 *
 *  0x0000 - 0x00FF  tiles for main intro
 *  0x0100 - 0x01FF  tiles for map intro
 *  0x0200 - 0x0327  unused
 *  0x0328 - 0x0427  game tiles, page 0
 *  0x0428 - 0x0527  game tiles, page 1
 *  0x0527 - 0x05FF  unused
 */

#ifndef _TILES_H
#define _TILES_H

#include "system.h"

#ifdef GFXPC
#define TILES_NBR_BANKS 4
#endif
#ifdef GFXST
#define TILES_NBR_BANKS 3
#endif

// RUxF (Rick Ultra Xpanded) expands the game-tile space. The primary
// "usable banks" are 1..4 (four pages of 256 tiles each, together a
// single unified 1024-tile space; see the Page selectors in the Tile /
// Block Editor windows). Bank 0 is separate and always holds only the
// font + cutscene decor. Legacy maps use banks 1 and 2 (256*2 tiles).
#define TILES_NBR_BANKS_MAX 5

#define TILES_SIZEOF8 (0x10)
#define TILES_SIZEOF16 (0x08)

/*
 * three special tile numbers
 */
#define TILES_BULLET 0x01
#define TILES_BOMB 0x02
#define TILES_RICK 0x03

/*
 * one single tile
 */
#ifdef GFXPC
typedef U16 tile_t[TILES_SIZEOF16];
#endif
#ifdef GFXST
typedef U32 tile_t[0x08];
#endif

/*
 * tiles banks (each bank is 0x100 tiles); RUxF needs up to 5 (bank 0 =
 * font/decor + 4 game-tile pages), so aliased to the max.
 */
extern tile_t tiles_data[TILES_NBR_BANKS_MAX][0x100];

#endif

/* eof */


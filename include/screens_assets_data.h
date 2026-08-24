/*
 * screens_assets_data.h -- extern declarations for ASCII text screens
 * ported from the original xrick source (rick/dat_screens.c).
 * Bitmap pictures (pic_congrats, pic_haf, pic_splash) are NOT
 * embedded here -- they're loaded on-demand from the xrick binary
 * by screens_assets.h's loadPicsFromXrickBinary().
 */
#ifndef _SCREENS_ASSETS_DATA_H
#define _SCREENS_ASSETS_DATA_H

#include "system.h"

/* ASCII text screens (same @=space, 0xFF, 0xFE encoding as screen_imaptext) */
#define SCREEN_IMAINCDC_SIZE   67
#define SCREEN_GAMEOVERTXT_SIZE 37
#define SCREEN_PAUSEDTXT_SIZE  34

/* Bitmap pictures: 4bpp nibble-packed U32 arrays, loaded from xrick binary */
#define PIC_CONGRATS_W 320
#define PIC_CONGRATS_H 32
#define PIC_HAF_W 320
#define PIC_HAF_H 32
#define PIC_SPLASH_W 320
#define PIC_SPLASH_H 200

extern U8 screen_imaincdc[];
extern U8 screen_gameovertxt[];
extern U8 screen_pausedtxt[];

#endif

/* eof */

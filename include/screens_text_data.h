/*
 * screens_text_data.h -- minimal header, matching the style of
 * include/sprites.h and include/tiles.h: just the extern declarations
 * for the map-intro text data ported into src/dat_screens.c, kept
 * separate from screens_text.h (which adds C++-only editing helpers)
 * so dat_screens.c can stay plain C.
 */
#ifndef _SCREENS_TEXT_DATA_H
#define _SCREENS_TEXT_DATA_H

#include "system.h"

#define SCREEN_IMAPTEXT_COUNT 5

extern U8 screen_imaptext_amazon[];
extern U8 screen_imaptext_egypt[];
extern U8 screen_imaptext_castle[];
extern U8 screen_imaptext_missile[];
extern U8 screen_imaptext_muchlater[];
extern U8 *screen_imaptext[SCREEN_IMAPTEXT_COUNT];

#endif

/* eof */

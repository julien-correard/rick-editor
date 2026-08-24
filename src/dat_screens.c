/*
 * xrick/src/data/dat_screens.c -- Port of screen data arrays from the
 * original xrick source (rick/dat_screens.c). Includes:
 * - Map intro text (screen_imaptext_*) -- edited via the Text Editor
 * - ASCII text screens (screen_imaincdc, screen_gameovertxt, screen_pausedtxt) -- edited via the Assets Editor
 * The animated-sprite list/steps/offsets (screen_imapsl, screen_imapsteps,
 * screen_imapsofs) remain out of scope.
 *
 * Copyright (C) 1998-2002 BigOrno (bigorno@bigorno.net). All rights
 * reserved. See the original xrick README for license terms.
 */

#include "screens_text_data.h"
#include "screens_assets_data.h"

/*
 * map intro, text
 * (from ds + 0x8810 + 0x2000, 0x2138, 0x2251, 0x236a, 0x2464)
 *
 * \376=0xfe \377=0xff
 */
U8 screen_imaptext_amazon[] = "\
@@@@@SOUTH@AMERICA@1945@@@@@@@\377\
RICK@DANGEROUS@CRASH@LANDS@HIS\377\
@PLANE@OVER@THE@AMAZON@WHILE@@\377\
@SEARCHING@FOR@THE@LOST@GOOLU@\377\
@@@@@@@@@@@@TRIBE.@@@@@@@@@@@@\377\377\
@BUT,@BY@A@TERRIBLE@TWIST@OF@@\377\
FATE@HE@LANDS@IN@THE@MIDDLE@OF\377\
@@@A@BUNCH@OF@WILD@GOOLUS.@@@@\377\377\
@@CAN@RICK@ESCAPE@THESE@ANGRY@\377\
@@@AMAZONIAN@ANTAGONISTS@?@@@@\376";

U8 screen_imaptext_egypt[] = "\
@@@@EGYPT,@SOMETIMES@LATER@@@@\377\
RICK@HEADS@FOR@THE@PYRAMIDS@AT\377\
@@@@THE@REQUEST@OF@LONDON.@@@@\377\377\
HE@IS@TO@RECOVER@THE@JEWEL@OF@\377\
ANKHEL@THAT@HAS@BEEN@STOLEN@BY\377\
FANATICS@WHO@THREATEN@TO@SMASH\377\
@IT,@IF@A@RANSOM@IS@NOT@PAID.@\377\377\
CAN@RICK@SAVE@THE@GEM,@OR@WILL\377\
HE@JUST@GET@A@BROKEN@ANKHEL@?@\376";

U8 screen_imaptext_castle[] = "\
@@@@EUROPE,@LATER@THAT@WEEK@@@\377\
@@RICK@RECEIVES@A@COMMUNIQUE@@\377\
@@FROM@BRITISH@INTELLIGENCE@@@\377\
@@ASKING@HIM@TO@RESCUE@ALLIED@\377\
@PRISONERS@FROM@THE@NOTORIOUS@\377\
@@@@SCHWARZENDUMPF@CASTLE.@@@@\377\377\
@@RICK@ACCEPTS@THE@MISSION.@@@\377\377\
@@@BUT@CAN@HE@LIBERATE@THE@@@@\377\
@CRUELLY@CAPTURED@COOMANDOS@?@\376";

U8 screen_imaptext_missile[] = "\
@@@@@@EUROPE,@EVEN@LATER@@@@@@\377\
RICK@LEARNS@FROM@THE@PRISONERS\377\
@THAT@THE@ENEMY@ARE@TO@LAUNCH@\377\
AN@ATTACK@ON@LONDON@FROM@THEIR\377\
@@@@@SECRET@MISSILE@BASE.@@@@@\377\377\
WITHOUT@HESITATION,@HE@DECIDES\377\
@@@TO@INFILTRATE@THE@BASE.@@@@\377\377\
CAN@RICK@SAVE@LONDON@IN@TIME@?\376";

U8 screen_imaptext_muchlater[] = "\
@@@LONDON,@MUCH,@MUCH@LATER@@@\377\
@RICK@RETURNS@TO@A@TRIUMPHANT@\377\
@@WELCOME@HOME@HAVING@HELPED@@\377\
@@@@SECURE@ALLIED@VICTORY.@@@@\377\377\
BUT,@MEANWHILE,@IN@SPACE,@THE@\377\
@@@MASSED@STARSHIPS@OF@THE@@@@\377\
@@@BARFIAN@EMPIRE@ARE@POISED@@\377\
@@@@@TO@INVADE@THE@EARTH.@@@@@\377\377\
@WHAT@WILL@RICK@DO@NEXT@...@?@\376";

U8 *screen_imaptext[5] =
{ screen_imaptext_amazon,
  screen_imaptext_egypt,
  screen_imaptext_castle,
  screen_imaptext_missile,
  screen_imaptext_muchlater
};

/*
 * main intro, Core Design copyright text
 * (from ds + 0x8810 + 0x2288)
 *
 * \376=0xfe \377=0xff
 */
U8 screen_imaincdc[] = "\
@@@@@@@@@@@@@@@@@@@\377\377\
(C)@1989@CORE@DESIGN\377\377\377\
@PRESS@SPACE@TO@START\376";

/*
 * gameover
 * (from ds + 0x8810 + 0x2864)
 *
 * \376=0xfe \377=0xff
 */
U8 screen_gameovertxt[] = "\
@@@@@@@@@@@\377\
@GAME@OVER@\377\
@@@@@@@@@@@\376";

/*
 * paused
 *
 * \376=0xfe \377=0xff
 */
U8 screen_pausedtxt[] = "\
@@@@@@@@@@\377\
@@PAUSED@@\377\
@@@@@@@@@@\376";

/* eof */

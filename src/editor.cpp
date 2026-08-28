// editor.cpp -- level editor (block-based editing: we keep the original
// map_bnums/map_blocks format for now; sprites and the rest of the engine
// will come later). Replaces the old pause()/drawmap()/leftclick() loop
// from main.cpp (kept for reference in src/legacy_main.cpp.txt) with a
// real editing UI: block palette, place/erase/pick, rectangle selection
// for bulk operations, free zoom/pan, and .map load/save.
//
// UI strings are in English (program requirement). Source comments stay
// in French, consistent with the rest of this codebase.

#include <SDL2/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>
#include <filesystem>
#include <system_error>

#include "tiles_render.h"
#include "mapdata.h"
#include "mapfile.h"
#include "xrick_patch.h"
#include "xrick_levels.h"
#include "xrick_marks.h"
#include "xrick_eflg.h"
#include "sprites_render.h"

#define STB_IMAGE_IMPLEMENTATION
#include "tile_import.h"
#include "sprite_import.h"
#include "screens_text.h"
#include "screens_assets.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;

static inline int mapIndex(int col, int row) { return col + row * MAP_COLS; }

// Nombre de cases franchies par appui sur Gauche/Droite/PageUp/PageDown
// (defilement rapide).
static const int FAST_SCROLL_CELLS = 20;

// Bank 0 holds the font and cutscene decor (see the Tile Editor's
// "Cutscene decor" section) but no block ever uses it in-game, so it's
// excluded from the Block Palette/Block Editor's bank selectors -- only
// banks 1 and 2 are "usable" in that specific sense.
static const int FIRST_USABLE_BANK = 1;
static const int LAST_USABLE_BANK = 2;

// Hazard-flag colors, shared between the Tile Editor's checkbox labels
// and drawTileHazardBorder() below -- kept in one place so the two
// never drift apart. Only Solid/Lethal/Climb/WayUp get a dedicated
// color (see drawTileHazardBorder()'s comment for why); the rest use
// the UI's normal text color.
static const ImVec4 HAZARD_COLOR_SOLID    (0.588f, 0.588f, 0.588f, 1.0f); // gray
static const ImVec4 HAZARD_COLOR_LETHAL   (0.863f, 0.235f, 0.235f, 1.0f); // red
static const ImVec4 HAZARD_COLOR_CLIMB    (0.275f, 0.784f, 0.353f, 1.0f); // green
static const ImVec4 HAZARD_COLOR_WAYUP    (0.314f, 0.549f, 0.941f, 1.0f); // blue (also the plain default border)
static const ImVec4 HAZARD_COLOR_FGND     (0.95f,  0.95f,  0.95f,  1.0f); // white
static const ImVec4 HAZARD_COLOR_SPAD     (0.95f,  0.85f,  0.15f,  1.0f); // yellow

struct Camera
{
    float x = 0.0f, y = 0.0f; // top-left corner of the view, in "world" pixels (zoom=1)
    float zoom = 2.0f;
    static constexpr float ZOOM_MIN = 0.25f;
    static constexpr float ZOOM_MAX = 16.0f;
};

struct Selection
{
    bool active = false;
    bool dragging = false;
    int c0 = 0, r0 = 0, c1 = 0, r1 = 0;

    void normalized(int &minC, int &minR, int &maxC, int &maxR) const
    {
        minC = std::min(c0, c1); maxC = std::max(c0, c1);
        minR = std::min(r0, r1); maxR = std::max(r0, r1);
    }
};

struct Clipboard
{
    bool hasData = false;
    int width = 0, height = 0;  // in blocks
    std::vector<int> blocks; // width * height block indices

    // Sprites captured within the copied block region.
    struct CopiedSprite {
        MarkEntry mark;        // full mark data
        int relTileCol;        // tile column offset from selection left edge (0-based)
        int relRowAbs;         // coarse row offset from selection top edge (0-based)
    };
    std::vector<CopiedSprite> sprites;

    void clear() { hasData = false; width = height = 0; blocks.clear(); sprites.clear(); }
};

enum class CanvasMode { Submap, Block, Sprite };

struct EditorState
{
    Camera cam;
    int bank = 1;              // same meaning as the old DisBank (originally 1 or 2; 0 is reachable too now)
    int selectedBlock = 1;     // active block in the palette, placed on left click
    Selection sel;
    bool painting = false;     // left button held = paint continuously
    bool picking = false;      // right button held = keep picking the block under the cursor
    bool panning = false;
    bool dirty = false;        // unsaved changes
    int lastPaintCol = -1, lastPaintRow = -1;
    Clipboard clip;            // copied block region
    bool pasting = false;      // paste mode: click to place clipboard
    int pasteCol = -1, pasteRow = -1; // cursor position in paste mode
    fs::path currentPath;      // empty if the map was never saved/loaded
    std::vector<fs::path> recentFiles; // last 3 opened .map files

    void addRecentFile(const fs::path &p)
    {
        for (auto it = recentFiles.begin(); it != recentFiles.end(); )
            it = (*it == p) ? recentFiles.erase(it) : it + 1;
        recentFiles.insert(recentFiles.begin(), p);
        if ((int)recentFiles.size() > 3) recentFiles.resize(3);
    }

    // Canvas mode: what a left-click on the map does. Submap = nothing
    // (just look around and inspect screen connections, e.g. to line
    // things up without risking an accidental edit) -- shows Screen
    // Connections; Block = paint/pick tiles (the old default) -- shows
    // Block Palette; Sprite = place/remove sprites (the old
    // spritePlacementMode) -- shows Sprite Tools. Each of the 3 side
    // windows is scoped to its matching mode, so only one is ever up.
    CanvasMode canvasMode = CanvasMode::Block;
    bool showGrid = false;         // always-available block-grid overlay
    bool showTriggerBoxes = true;  // show trigger-box overlay for trap entities

    // Sprites
    bool showSprites = true;       // overlay toggle -- can be turned off to focus on tile editing
    bool showMapStartPositions = true; // show map start position markers above sprites
    int selectedEnt = 4;           // entity type id to place (lowest observed in stock data)
    int hoverMarkSubmap = -1, hoverMarkIndex = -1; // nearest sprite under cursor, for right-click removal

    // Map start placement mode: -1 = off, 0..MAP_NBR_MAPS-1 = placing that map's start position
    int placeStartMode = -1;
};

// State for the standalone Tile Editor window -- separate from the map
// canvas entirely (own bank/selection, doesn't touch st.bank so picking
// a tile to edit never disturbs what's selected in the Block Palette).
struct TileEditorState
{
    bool open = false;
    int bank = 1;          // FIRST_USABLE_BANK..LAST_USABLE_BANK, same convention as st.bank
    int selectedTile = -1; // -1 = nothing selected yet
    std::set<int> multiSelected; // Ctrl+click / Shift+click multi-selection
    int anchorTile = -1; // anchor for Shift+click range select
    int batchStartTile = 0; // "Batch import..." starting tile index
    bool swapMode = false; // "Swap with..." armed -- next tile clicked swaps with selectedTile
                    bool copyMode = false; // "Copy to..." armed -- next tile clicked gets the copied tile
    tile_t copiedTile{};
    bool hasCopiedTile = false;
    bool confirmDelete = false; // true while delete confirmation popup is open
};

struct PixelEditorState
{
    bool open = false;
    enum Target { Tile, Sprite } target = Tile;
    int bank = 0;    // tile bank (only used when target == Tile)
    int index = 0;   // tile or sprite index
    int color = 1;   // current drawing color (0-15)
    tile_t backupTile{};
    sprite_t backupSprite{};
};

static inline int getTilePixel(const tile_t &tile, int x, int y)
{
    return (tile[y] >> (28 - x * 4)) & 0xF;
}

static inline void setTilePixel(tile_t &tile, int x, int y, int c)
{
    tile[y] = (tile[y] & ~(0xFu << (28 - x * 4))) | ((uint32_t)(c & 0xF) << (28 - x * 4));
}

static inline int getSpritePixel(const sprite_t &spr, int x, int y)
{
    int word = x / 8, bit = 28 - (x % 8) * 4;
    return (spr[y][word] >> bit) & 0xF;
}

static inline void setSpritePixel(sprite_t &spr, int x, int y, int c)
{
    int word = x / 8, bit = 28 - (x % 8) * 4;
    spr[y][word] = (spr[y][word] & ~(0xFu << bit)) | ((uint32_t)(c & 0xF) << bit);
}

// Returns trigger box size (in tiles) for entity types that use one,
// or (0,0) for types with no distinct trigger zone.
static void entTriggerSize(int ent, int &tw, int &th)
{
    switch (ent)
    {
    case 22: tw =  4; th = 4; return;  // special bonus
    case 23: tw =  4; th = 4; return;  // special bonus
    case 24: tw =  3; th = 3; return;  // ticking bomb
    case 25: tw =  4; th = 4; return;  // arrow trap
    case 26: tw = 16; th = 4; return;  // arrow trap, wide trigger
    case 27: tw =  4; th = 4; return;  // wall spike trap
    case 28: tw =  4; th = 4; return;  // slow left trap
    case 29: tw =  4; th = 4; return;  // bomb-like
    case 30: tw =  4; th = 4; return;  // bomb-like
    case 31: tw =  4; th = 4; return;  // rising/falling trap
    case 32: tw =  4; th = 7; return;  // arrow trap, vertical
    case 33: tw = 20; th = 4; return;  // wide trap
    case 34: tw =  4; th = 4; return;  // small trap
    case 35: tw =  4; th = 4; return;  // small trap
    case 36: tw =  3; th = 3; return;  // ticking bomb
    case 37: tw =  4; th = 4; return;  // wall crusher
    case 38: tw =  4; th = 4; return;  // grille trap
    case 39: tw =  4; th = 4; return;  // spike trap
    case 40: tw =  4; th = 4; return;  // spike wall
    case 41: tw =  4; th = 4; return;  // spike wall
    case 42: tw =  4; th = 4; return;  // spike wall
    case 43: tw =  4; th = 4; return;  // stone block, complex path
    case 44: tw =  4; th = 4; return;  // stone block, slides left
    case 45: tw = 20; th = 4; return;  // wide grille
    case 46: tw =  3; th = 3; return;  // bomb-like
    case 47: tw =  4; th = 4; return;  // bomb-like
    case 48: tw =  4; th = 4; return;  // stone block, complex path
    case 49: tw =  4; th = 4; return;  // narrow grille
    case 50: tw =  4; th = 4; return;  // small trap
    case 51: tw = 24; th = 4; return;  // wide trap
    case 52: tw =  4; th = 4; return;  // rising grille
    case 53: tw =  4; th = 4; return;  // dog, walks left slowly
    case 54: tw =  4; th = 4; return;  // door top
    case 55: tw =  4; th = 4; return;  // door bottom
    case 56: tw =  4; th = 4; return;  // shooting soldier
    case 57: tw = 16; th = 4; return;  // trap fires bullets, wide trigger zone
    case 58: tw =  4; th = 4; return;  // small trap
    case 59: tw =  3; th = 3; return;  // bomb-like
    case 60: tw =  4; th = 4; return;  // small trap
    case 61: tw = 24; th = 4; return;  // wide trap
    case 62: tw =  4; th = 4; return;  // bomb-like
    case 63: tw = 20; th = 4; return;  // fast projectile
    case 64: tw =  3; th = 3; return;  // bomb-like
    case 65: tw =  4; th = 6; return;  // spitter
    case 66: tw = 31; th = 4; return;  // dog, walks left, wide trigger zone
    case 67: tw =  4; th = 4; return;  // teleport
    case 68: tw =  4; th = 4; return;  // teleport
    case 69: tw =  4; th = 4; return;  // small trap
    case 70: tw =  4; th = 4; return;  // small trap
    case 71: tw = 24; th = 4; return;  // wide trap
    case 72: tw =  4; th = 4; return;  // stone block, slides right
    case 73: tw =  4; th = 4; return;  // slow right trap
    case 16: case 17: case 18: case 19: case 21:
    default: tw = 0;  th = 0; return;
    }
}

// Returns a short gameplay description for well-known entity types, or ""
// for types that don't need a blurb yet.
static const char *entInfoText(int ent)
{
    switch (ent)
    {
    // --- Falling grille traps (type-3, sprite 102) ---
    case 45:
        return "Grille trap (wide trigger, 160x32 px).\n"
               "Falls 32 px onto the player, holds, then retracts.\n"
               "Retriggerable. Trigger box = rick walk-through zone.";
    case 49:
        return "Grille trap (narrow trigger, 32x32 px).\n"
               "Falls 48 px onto the player, holds, slow retraction.\n"
               "Retriggerable. Trigger box = rick walk-through zone.";
    case 52:
        return "Grille trap (narrow trigger, 32x32 px).\n"
               "Rises 32 px first, then slams down 32 px.\n"
               "Retriggerable. Trigger box = rick walk-through zone.";
    // --- Walkers / climbers (type 1a/1b/2) ---
    case 4: case 7: case 10: case 13:
        return "Patrol walker (type 1a). Walks right from spawn,\n"
               "u-turns after 'patrol distance' tiles or on walls.\n"
               "3 walker/climber slots shared globally.";
    case 5: case 8: case 11: case 14:
        return "Chaser (type 1b). Pursues Rick vertically+horizontally.\n"
               "3 walker/climber slots shared globally.";
    case 6: case 9: case 12: case 15:
        return "Climber (type 2). Climbs walls toward Rick.\n"
               "3 walker/climber slots shared globally.";
    // --- Type-3 fireball traps ---
    case 29:
        return "Fireball trap (24x21). Fires a fireball LEFT.\n"
               "Projectile then arcs with a bouncing trajectory.\n"
               "Sprite = 88 (fireball). Trigger box = 4x4 tiles.";
    case 30:
        return "Fireball trap (24x21). Fires a fireball RIGHT.\n"
               "Projectile then arcs with a bouncing trajectory.\n"
               "Sprite = 88 (fireball). Trigger box = 4x4 tiles.";
    // --- Boxes / bonuses (type 0, ids 16-23) ---
    case 16:
        return "Ammo box. Refills Rick's bullets to max\n"
               "(GAME_BULLETS_INIT, 34 total) on pickup.\n"
               "Can also be blown up by bomb/bullet/stick.";
    case 17:
        return "Dynamite crate. Refills Rick's bombs to max\n"
               "(GAME_BOMBS_INIT, 3 total) on pickup.\n"
               "Can also be blown up by bomb/bullet/stick.";
    case 18: case 19: case 20: case 21:
        return "Bonus item (scores +500 on pickup, flies up\n"
               "and vanishes). Static until touched.";
    case 22: case 23:
        return "Sound-only event. Plays a sound effect and\n"
               "does nothing else (no sprite, no collision).\n"
               "Placed to trigger an audio cue at that spot.";
    // --- Arrow traps ---
    case 25:
        return "Arrow trap. Fires right toward Rick.\n"
               "Trigger box = 32x32 px. Retriggerable.";
    case 26:
        return "Arrow trap (wide trigger, 128x32 px).\n"
               "Fires left toward Rick. Retriggerable.";
    case 27:
        return "Wall spike trap. Spikes emerge from walls.\n"
               "Trigger box = 32x32 px. Retriggerable.";
    // --- Stone blocks (type-3, triggered by dynamite/bomb) ---
    case 44:
        return "Stone block (32x16). Blocks Rick's path.\n"
               "When triggered by dynamite or proximity,\n"
               "slides left ~560 px then loops.\n"
               "Use dynamite to clear the way.";
    case 48:
        return "Stone block (32x16). Blocks Rick's path.\n"
               "When triggered, follows a complex path:\n"
               "left → down → left → pause → right → up\n"
               "→ pause → right → fade → up → right → loop.\n"
               "Use dynamite to clear the way.";
    case 72:
        return "Stone block (32x16). Blocks Rick's path.\n"
               "When triggered by dynamite or proximity,\n"
               "slides right ~560 px then loops.\n"
               "Use dynamite to clear the way.";
    // --- Doors (type-3, pairs that block and vanish on bomb) ---
    case 54:
        return "Door -- top piece (18x16).\n"
               "Paired with entity 55 (bottom piece, STOPRICK).\n"
               "Both start asleep and invisible to Rick.\n"
               "Triggered ONLY by bombs (TRIGBOMB).\n"
               "On trigger: plays sound, vanishes for 8 ticks,\n"
               "then permanently deactivates (ENT_FLG_ONCE).";
    case 55:
        return "Door -- bottom piece (18x16).\n"
               "Paired with entity 54 (top piece).\n"
               "Blocks Rick's path (STOPRICK flag).\n"
               "Triggered ONLY by bombs (TRIGBOMB).\n"
               "On trigger: becomes LETHAL to Rick for 8 ticks,\n"
               "then permanently deactivates (ENT_FLG_ONCE).";
    // --- Shooting soldier (type-3, stationary) ---
    case 56:
        return "Soldier who shoots (24x21).\n"
               "Stands still and fires projectiles.\n"
               "Trigger box = 4x4 tiles.\n"
               "3 entity slots shared for projectiles.";
    // --- Bullet trap (type-3, fires left) ---
    case 57:
        return "Bullet trap (16x8). Fires projectiles LEFT.\n"
               "Trigger zone = 16x4 tiles (wide, flat).\n"
               "Projectile travels left for ~70 frames then stops.\n"
               "3 entity slots shared for projectiles.";
    // --- Dog (type-3, walks along ground) ---
    case 53:
        return "Dog (24x21). Walks left slowly at 2 px/frame.\n"
               "Trigger box = 4x4 tiles.\n"
               "Default flags: ONCE + TRIGRICK + LETHALI.";
    case 66:
        return "Dog (24x21). Walks left at 6 px/frame.\n"
               "Trigger box = 31x4 tiles (wide detection zone).\n"
               "Default flags: ONCE + TRIGRICK + LETHALI.";
    // --- Moving stones / falling rocks (type-3, sprite 85) ---
    case 24:
        return "Stone - falls. (24x16)\n"
               "Rocks in place, then falls 4 tile-rows,\n"
               "then climbs back up to spawn.\n"
               "Trigger box = 3x3 tiles.";
    case 28:
        return "Stone - shoots left. (24x16)\n"
               "Slams violently LEFT and never comes back.\n"
               "Trigger box = 4x4 tiles.";
    case 31:
        return "Stone - delayed drop. (24x16)\n"
               "Waits a long time, falls 4 tile-rows,\n"
               "then returns to spawn.\n"
               "Trigger box = 4x4 tiles.";
    case 33:
        return "Stone - sinks. (24x16)\n"
               "Falls slowly, tile-row by tile-row, and\n"
               "stays in place once it reaches the bottom.\n"
               "Wide trigger = 20x4 tiles.";
    case 36:
        return "Stone - double drop. (24x16)\n"
               "Falls 4 tile-rows, waits a long time, falls\n"
               "4 more tile-rows, then comes back up quickly.\n"
               "Trigger box = 3x3 tiles.";
    case 38:
        return "Stone - slides then slams. (24x16)\n"
               "Moves 4 tile-cols to the right, waits,\n"
               "then slams violently LEFT.\n"
               "Trigger box = 4x4 tiles.";
    case 73:
        return "Stone - shoots right. (24x16)\n"
               "Slams violently RIGHT and never comes back.\n"
               "Trigger box = 4x4 tiles.";
    // --- Egyptian treasure (type-3, 16x16) ---
    case 34:
        return "Egyptian treasure (16x16). Triggered once,\n"
               "then gone. Trigger box = 4x4 tiles.";
    case 35:
        return "Egyptian treasure (16x16). Holds, then drops\n"
               "8 px out of reach. Trigger box = 4x4 tiles.";
    // --- Wall crusher (type-3, sprite 93) ---
    case 37:
        return "Wall crusher (32x8). Slides horizontally\n"
               "toward Rick and crushes. Trigger box = 4x4 tiles.";
    // --- Spike walls (type-3) ---
    case 40:
        return "Spike wall (24x21). Slides right 2 px/frame\n"
               "until blocked. Trigger box = 4x4 tiles.";
    case 41:
        return "Spike wall (24x21). Slides right 1 px/frame\n"
               "(slower). Trigger box = 4x4 tiles.";
    case 42:
        return "Fireball ball (24x21, sprite 97).\n"
               "Rolls right, dips, then rolls left, with a\n"
               "complex path. Guide it with boulders -- carefully.\n"
               "No snakes, please. (Just a ball. Run, Rick, run!)\n"
               "Trigger box = 4x4 tiles.";
    // --- Stone block (type-3, complex path) ---
    case 43:
        return "First level's bat\n"
               "Goes down-right then up-left.";
    // --- Bomb-like (type-3) ---
    case 46:
        return "Bomb trap (24x21). Spawns asleep.\n"
               "Trigger box = 3x3 tiles.";
    case 47:
        return "Sound-trigger (24x21). Invisible, no collision.\n"
               "On trigger plays a sound effect (ent2.wav)\n"
               "once, then deactivates (ONCE). Use as an\n"
               "audio cue, e.g. next to a spike trap.\n"
               "Trigger box = 4x4 tiles.";
    case 59:
        return "Bomb trap (24x16). Holds indefinitely.\n"
               "Trigger box = 3x3 tiles.";
    case 62:
        return "Bomb trap (24x21). Holds indefinitely.\n"
               "Trigger box = 4x4 tiles.";
    case 64:
        return "Fire trap (24x16). Shoots fire upward/outward\n"
               "to burn Rick. Invisible until it triggers;\n"
               "sprite = 122 (flame).\n"
               "Trigger box = 3x3 tiles.";
    // --- Small watcher traps (type-3, 18x21) ---
    case 50:
        return "Watcher (18x21). Scurries left, then hops.\n"
               "Trigger box = 4x4 tiles.";
    case 51:
        return "Watcher (18x21, wide trigger 24x4).\n"
               "Slides right 3 px/frame. Trigger box = 24x4 tiles.";
    case 61:
        return "Watcher (18x21, wide trigger 24x4).\n"
               "Slides left 3 px/frame. Trigger box = 24x4 tiles.";
    case 71:
        return "Watcher (18x21, wide trigger 24x4).\n"
               "Shifts left, right, left, right alternately.\n"
               "Trigger box = 24x4 tiles.";
    // --- Small fast traps (type-3) ---
    case 58:
        return "Fast trap (24x16). Darts right, left, right\n"
               "with pauses. Trigger box = 4x4 tiles.";
    case 70:
        return "Fast trap (24x16). Slides left, right, left,\n"
               "right alternately. Trigger box = 4x4 tiles.";
    // --- Medium ground traps (type-3) ---
    case 60:
        return "Ground trap (24x17). Slides right, speeds up,\n"
               "then darts left. Trigger box = 4x4 tiles.";
    case 69:
        return "Crane. Slides right and left\n"
               "in alternating bursts. Trigger box = 4x4 tiles.";
    // --- Fast projectile (type-3) ---
    case 63:
        return "Fast projectile (32x8). Fires LEFT at 12 px/frame\n"
               "for 128 frames (~1536 px). Wide trigger 20x4.";
    // --- Spitter (type-3) ---
    case 65:
        return "Spitter (24x21). Fires UP at 8 px/frame.\n"
               "Trigger box = 4x6 tiles.";
    // --- Teleporters (type-3) ---
    case 67:
        return "Teleporter (24x21). Jumps Rick in a diamond\n"
               "pattern. Trigger box = 4x4 tiles.";
    case 68:
        return "Teleporter (24x21). Jumps Rick with a\n"
               "different pattern. Trigger box = 4x4 tiles.";
    // --- Special ---
    case 32:
        return "Arrow trap - vertical (4x21, sprite 90).\n"
               "Fires a projectile UP at 8 px/frame\n"
               "for ~70 frames.\n"
               "3 entity slots shared for arrows.\n"
               "Trigger box = 4x7 tiles.";
    case 39:
        return "Spike trap. Spikes rise from the ground.\n"
               "Trigger box = 32x32 px. Retriggerable.";
    default:
        if (ent >= 0x18)
            return "Type-3 entity. Starts asleep, wakes on trigger.\n"
                   "Movement sequence defined in ent_mvstep.";
        if (ent >= 4 && ent <= 15)
            return "Walker / climber (type 1a/1b/2).\n"
                   "3 slots shared globally. Moves toward Rick.";
        return "";
    }
}

// State for the standalone Block Editor window -- composes blocks out
// of tiles (map_blocks), separate from both the map canvas and the Tile
// Editor's own selection.
struct BlockEditorState
{
    bool open = false;
    int bank = 1;            // preview bank only -- map_blocks itself is bank-independent
    int selectedBlock = -1;  // -1 = nothing selected yet
    int selectedCell = 0;    // 0-15, which of the block's 16 tile slots a picked tile goes into
    bool swapMode = false;   // "Swap with..." armed -- next block clicked in the grid swaps with selectedBlock
    bool copyMode = false;   // "Copy to..." armed -- next block clicked in the grid is overwritten with selectedBlock's composition
};

// State for the standalone Sprite Editor window -- mirrors TileEditorState,
// but sprites_data has no bank split (one flat array) and no hazard
// flags (that's the marks/triggers system, unrelated).
struct SpriteEditorState
{
    bool open = false;
    int selectedSprite = -1;  // -1 = nothing selected yet
    int batchStartSprite = 0; // "Batch import..." starting sprite index
};

// State for the standalone Text Editor window -- edits the 5
// between-levels intro texts (screen_imaptext_*, see screens_text.h).
// The live editable text itself (mapTexts) lives in main()'s state
// alongside connections/sprites/eflg, not in here -- this struct is
// just which entry/row is selected in the UI.
struct TextEditorState
{
    bool open = false;
    int selected = 0; // 0-4, index into mapTexts[] / SCREEN_IMAPTEXT_LABELS[]
};

enum class DialogPurpose { OpenMap, SaveMap, PickXrickBinary, PickXrickBinaryForConnections, ImportTileImage, BatchImportTileImage, ImportSpriteImage, BatchImportSpriteImage, ExportTileBankPNG, ExportSpritePNG, ImportBank0Image, BatchImportBank0Image, ExportPicPNG, ImportPicImage };

struct FileDialog
{
    bool show = false;
    bool saveMode = false;
    DialogPurpose purpose = DialogPurpose::OpenMap;
    std::unordered_map<int, fs::path> lastDir; // per-purpose directory memory
    char filename[256] = "";
    std::string error;
    // Empty = show every regular file; otherwise only files whose
    // extension matches one of these (case-sensitive, includes the dot).
    std::vector<std::string> extFilter = {".map"};

    fs::path &currentDir() { return lastDir[(int)purpose]; }
    void initCurrentDir() { if (!lastDir.count((int)purpose)) lastDir[(int)purpose] = fs::current_path(); }
};

static fs::path configPath()
{
    const char *home = std::getenv("HOME");
    if (!home) return {};
    return fs::path(home) / ".config" / "rickeditor" / "config.txt";
}

static void saveConfig(const EditorState &st, const FileDialog &fd)
{
    auto p = configPath();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    FILE *f = std::fopen(p.string().c_str(), "w");
    if (!f) return;
    for (auto &rf : st.recentFiles)
        std::fprintf(f, "recent=%s\n", rf.string().c_str());
    for (auto &[purpose, dir] : fd.lastDir)
        std::fprintf(f, "dir:%d=%s\n", purpose, dir.string().c_str());
    std::fclose(f);
}

static void loadConfig(EditorState &st, FileDialog &fd)
{
    auto p = configPath();
    if (!fs::exists(p)) return;
    FILE *f = std::fopen(p.string().c_str(), "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), f))
    {
        // strip trailing newline
        line[strcspn(line, "\r\n")] = '\0';
        if (std::strncmp(line, "recent=", 7) == 0)
        {
            fs::path rp(line + 7);
            if (fs::exists(rp)) st.addRecentFile(rp);
        }
        else if (std::strncmp(line, "dir:", 4) == 0)
        {
            const char *eq = std::strchr(line + 4, '=');
            if (eq)
            {
                int purpose = std::atoi(line + 4);
                fd.lastDir[purpose] = fs::path(eq + 1);
            }
        }
    }
    std::fclose(f);
}

static void clampCamera(EditorState &st, int viewportW, int viewportH)
{
    float worldW = MAP_COLS * (float)BLOCK_PX;
    float worldH = MAP_ROWS * (float)BLOCK_PX;
    float viewW = viewportW / st.cam.zoom;
    float viewH = viewportH / st.cam.zoom;
    // Allow seeing a bit past the edges (comfort margin), without letting
    // the camera drift arbitrarily far away.
    float margin = 64.0f;
    st.cam.x = std::clamp(st.cam.x, -margin, std::max(-margin, worldW - viewW + margin));
    st.cam.y = std::clamp(st.cam.y, -margin, std::max(-margin, worldH - viewH + margin));
}

static void zoomAt(EditorState &st, float screenX, float screenY, float factor)
{
    float worldX = st.cam.x + screenX / st.cam.zoom;
    float worldY = st.cam.y + screenY / st.cam.zoom;
    st.cam.zoom = std::clamp(st.cam.zoom * factor, Camera::ZOOM_MIN, Camera::ZOOM_MAX);
    st.cam.x = worldX - screenX / st.cam.zoom;
    st.cam.y = worldY - screenY / st.cam.zoom;
}

// Converts a screen position (within the map view, top-left of the
// window) into map cell coordinates (column, row).
static void screenToCell(const EditorState &st, float sx, float sy, int &col, int &row)
{
    float worldX = st.cam.x + sx / st.cam.zoom;
    float worldY = st.cam.y + sy / st.cam.zoom;
    col = (int)std::floor(worldX / BLOCK_PX);
    row = (int)std::floor(worldY / BLOCK_PX);
}

static bool cellValid(int col, int row)
{
    return col >= 0 && col < MAP_COLS && row >= 0 && row < MAP_ROWS;
}

// Screen-connection and sprite rows use TILE-row units, four times finer
// than the main map's block-row grid (see the unit note in
// xrick_levels.h) -- so their valid absolute range is MAP_ROWS*4, not
// MAP_ROWS.
static const int MAP_TILE_ROWS = MAP_ROWS * 4;

// Absolute tile column (0-31, shared across the whole map -- unlike rows,
// columns aren't submap-relative in the original format) under a screen
// position.
static int screenToTileCol(const EditorState &st, float sx)
{
    float worldX = st.cam.x + sx / st.cam.zoom;
    return (int)std::floor(worldX / TILE_PX);
}

// Absolute tile row under a screen position -- same TILE_PX-based scale
// as columns, and the unit screen-connections/sprites are stored in.
static int screenToTileRow(const EditorState &st, float sy)
{
    float worldY = st.cam.y + sy / st.cam.zoom;
    return (int)std::floor(worldY / TILE_PX);
}

// Which submap "owns" a given absolute tile row: the one whose own start
// row is the closest at-or-below `rowAbs`, among all submaps. Used to
// file a newly-placed sprite under the right submap's list.
static int submapForAbsRow(const ConnectionsData &conn, int rowAbs)
{
    int best = -1, bestStart = -1;
    for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
    {
        int start = submapStartRow(conn.submaps[s]);
        if (start <= rowAbs && start > bestStart) { bestStart = start; best = s; }
    }
    return best;
}

// Nearest sprite to (col, rowAbs) within a small radius, searched across
// every submap. Used for right-click removal in sprite placement mode.
static bool findMarkNear(const MarksData &marks, int col, int rowAbs, int &outSubmap, int &outIndex)
{
    const int RADIUS = 4; // tile columns / tile rows
    int best = 1 << 30;
    outSubmap = -1; outIndex = -1;
    for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
    {
        for (int i = 0; i < (int)marks.marks[s].size(); i++)
        {
            const MarkEntry &m = marks.marks[s][i];
            int d = std::abs(m.col - col) + std::abs(m.rowAbs - rowAbs);
            if (d < best) { best = d; outSubmap = s; outIndex = i; }
        }
    }
    return best <= RADIUS;
}

static void drawMap(SDL_Renderer* renderer, SDL_Texture* blockAtlas, const EditorState &st, const ConnectionsData &conn, int viewportW, int viewportH)
{
    int firstCol = std::max(0, (int)std::floor(st.cam.x / BLOCK_PX));
    int firstRow = std::max(0, (int)std::floor(st.cam.y / BLOCK_PX));
    int lastCol = std::min(MAP_COLS - 1, (int)std::floor((st.cam.x + viewportW / st.cam.zoom) / BLOCK_PX));
    int lastRow = std::min(MAP_ROWS - 1, (int)std::floor((st.cam.y + viewportH / st.cam.zoom) / BLOCK_PX));

    float destSize = BLOCK_PX * st.cam.zoom;

    for (int row = firstRow; row <= lastRow; row++)
    {
        for (int col = firstCol; col <= lastCol; col++)
        {
            int blockIdx = map_bnums[mapIndex(col, row)];
            int bx = (blockIdx % ATLAS_BLOCKS_PER_ROW) * BLOCK_PX;
            int by = (blockIdx / ATLAS_BLOCKS_PER_ROW) * BLOCK_PX;
            SDL_Rect src{bx, by, BLOCK_PX, BLOCK_PX};

            SDL_FRect dst;
            dst.x = (col * BLOCK_PX - st.cam.x) * st.cam.zoom;
            dst.y = (row * BLOCK_PX - st.cam.y) * st.cam.zoom;
            dst.w = destSize;
            dst.h = destSize;
            SDL_RenderCopyF(renderer, blockAtlas, &src, &dst);
        }
    }

    // Grid overlay: always on when st.showGrid is set (checkbox / 'g' key
    // in the top bar), or automatically above a zoom level as a visual
    // guide either way.
    if (st.showGrid)
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        for (int col = firstCol; col <= lastCol + 1; col++)
        {
            float sx = (col * BLOCK_PX - st.cam.x) * st.cam.zoom;
            SDL_RenderDrawLineF(renderer, sx, 0, sx, (float)viewportH);
        }
        for (int row = firstRow; row <= lastRow + 1; row++)
        {
            float sy = (row * BLOCK_PX - st.cam.y) * st.cam.zoom;
            SDL_RenderDrawLineF(renderer, 0, sy, (float)viewportW, sy);
        }
    }

    // Highlight the current selection.
    if (st.sel.active)
    {
        int minC, minR, maxC, maxR;
        st.sel.normalized(minC, minR, maxC, maxR);
        SDL_FRect r;
        r.x = (minC * BLOCK_PX - st.cam.x) * st.cam.zoom;
        r.y = (minR * BLOCK_PX - st.cam.y) * st.cam.zoom;
        r.w = (maxC - minC + 1) * destSize;
        r.h = (maxR - minR + 1) * destSize;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 80, 160, 255, 70);
        SDL_RenderFillRectF(renderer, &r);
        SDL_SetRenderDrawColor(renderer, 80, 160, 255, 220);
        SDL_RenderDrawRectF(renderer, &r);
    }

    // Paste preview: ghost outline of the clipboard at the cursor position.
    if (st.pasting && st.clip.hasData)
    {
        SDL_FRect r;
        r.x = (st.pasteCol * BLOCK_PX - st.cam.x) * st.cam.zoom;
        r.y = (st.pasteRow * BLOCK_PX - st.cam.y) * st.cam.zoom;
        r.w = st.clip.width * destSize;
        r.h = st.clip.height * destSize;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 200, 80, 60);
        SDL_RenderFillRectF(renderer, &r);
        SDL_SetRenderDrawColor(renderer, 255, 200, 80, 200);
        SDL_RenderDrawRectF(renderer, &r);
    }
}

static void clearSelection(EditorState &st, int fillValue)
{
    if (!st.sel.active) return;
    int minC, minR, maxC, maxR;
    st.sel.normalized(minC, minR, maxC, maxR);
    for (int row = minR; row <= maxR; row++)
        for (int col = minC; col <= maxC; col++)
            if (cellValid(col, row))
                map_bnums[mapIndex(col, row)] = fillValue;
    st.dirty = true;
}

static void updateWindowTitle(SDL_Window* window, const EditorState &st)
{
    std::string name = st.currentPath.empty() ? "untitled.map" : st.currentPath.filename().string();
    std::string title = "RickEditor -- " + name + (st.dirty ? " *" : "");
    SDL_SetWindowTitle(window, title.c_str());
}

// Renders the Open/Save As modal. Returns true the frame the user
// confirms a path (placed in outPath).
static bool renderFileDialog(FileDialog &fd, fs::path &outPath, const EditorState &st)
{
    if (!fd.show) return false;
    fd.initCurrentDir();
    const char* title = fd.purpose == DialogPurpose::PickXrickBinary ? "Select xrick binary"
                       : fd.purpose == DialogPurpose::PickXrickBinaryForConnections ? "Select xrick binary"
                       : fd.purpose == DialogPurpose::ImportTileImage ? "Import tile image"
                       : fd.purpose == DialogPurpose::BatchImportTileImage ? "Batch import tile image"
                       : fd.purpose == DialogPurpose::ImportSpriteImage ? "Import sprite image"
                       : fd.purpose == DialogPurpose::BatchImportSpriteImage ? "Batch import sprite image"
                       : fd.purpose == DialogPurpose::ExportTileBankPNG ? "Export tile bank as PNG"
                       : fd.purpose == DialogPurpose::ExportSpritePNG ? "Export sprites as PNG"
                       : fd.purpose == DialogPurpose::ImportBank0Image ? "Import tile image (bank 0)"
                       : fd.purpose == DialogPurpose::BatchImportBank0Image ? "Batch import tile image (bank 0)"
                       : fd.saveMode ? "Save As" : "Open Map";
    ImGui::OpenPopup(title);

    bool confirmed = false;
    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    bool stillOpen = true;
    if (ImGui::BeginPopupModal(title, &stillOpen, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextDisabled("%s", fd.currentDir().string().c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("##filelist", ImVec2(0, 300), true))
        {
            if (fd.currentDir().has_parent_path() && fd.currentDir() != fd.currentDir().root_path())
            {
                if (ImGui::Selectable("[..]"))
                {
                    fd.currentDir() = fd.currentDir().parent_path();
                    saveConfig(st, fd);
                }
            }

            std::error_code ec;
            std::vector<fs::directory_entry> dirs, files;
            for (auto &e : fs::directory_iterator(fd.currentDir(), ec))
            {
                if (ec) break;
                if (e.is_directory()) dirs.push_back(e);
                else if (fd.extFilter.empty()) files.push_back(e);
                else
                {
                    std::string ext = e.path().extension().string();
                    if (std::find(fd.extFilter.begin(), fd.extFilter.end(), ext) != fd.extFilter.end())
                        files.push_back(e);
                }
            }
            std::sort(dirs.begin(), dirs.end(), [](auto&a, auto&b){ return a.path().filename() < b.path().filename(); });
            std::sort(files.begin(), files.end(), [](auto&a, auto&b){ return a.path().filename() < b.path().filename(); });

            for (auto &d : dirs)
            {
                std::string label = "[" + d.path().filename().string() + "]";
                if (ImGui::Selectable(label.c_str()))
                {
                    fd.currentDir() = d.path();
                    saveConfig(st, fd);
                }
            }
            for (auto &fentry : files)
            {
                std::string name = fentry.path().filename().string();
                bool selected = (name == fd.filename);
                if (ImGui::Selectable(name.c_str(), selected))
                {
                    std::snprintf(fd.filename, sizeof(fd.filename), "%s", name.c_str());
                }
            }
        }
        ImGui::EndChild();

        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##filename", fd.filename, sizeof(fd.filename));
        if (!fd.error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", fd.error.c_str());

        const char* confirmLabel = fd.purpose == DialogPurpose::PickXrickBinary ? "Select"
                                   : fd.purpose == DialogPurpose::PickXrickBinaryForConnections ? "Select"
                                   : fd.purpose == DialogPurpose::ImportTileImage ? "Import"
                                   : fd.purpose == DialogPurpose::BatchImportTileImage ? "Import"
                                   : fd.purpose == DialogPurpose::ImportSpriteImage ? "Import"
                                   : fd.purpose == DialogPurpose::BatchImportSpriteImage ? "Import"
                                   : fd.purpose == DialogPurpose::ExportTileBankPNG ? "Export"
                                   : fd.purpose == DialogPurpose::ExportSpritePNG ? "Export"
                                   : fd.purpose == DialogPurpose::ImportBank0Image ? "Import"
                                   : fd.purpose == DialogPurpose::BatchImportBank0Image ? "Import"
                                   : fd.saveMode ? "Save" : "Open";
        if (ImGui::Button(confirmLabel, ImVec2(120, 0)))
        {
            std::string name = fd.filename;
            if (name.empty())
            {
                fd.error = "Enter a file name.";
            }
            else
            {
                fs::path p = fd.currentDir() / name;
                if (fd.saveMode && p.extension() != ".map")
                    p += ".map";
                if (!fd.saveMode && !fs::exists(p))
                {
                    fd.error = "File not found.";
                }
                else
                {
                    outPath = p;
                    confirmed = true;
                    fd.show = false;
                    fd.error.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            fd.show = false;
            fd.error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!stillOpen) fd.show = false;
    return confirmed;
}

static void requestOpen(FileDialog &fd)
{
    fd.show = true; fd.saveMode = false; fd.purpose = DialogPurpose::OpenMap;
    fd.extFilter = {".map"}; fd.filename[0] = '\0'; fd.error.clear();
}
static void requestSaveAs(FileDialog &fd, const fs::path &currentPath)
{
    fd.show = true; fd.saveMode = true; fd.purpose = DialogPurpose::SaveMap;
    fd.extFilter = {".map"}; fd.filename[0] = '\0';
    if (!currentPath.empty())
        std::snprintf(fd.filename, sizeof(fd.filename), "%s", currentPath.filename().string().c_str());
    fd.error.clear();
}
static void doSave(EditorState &st, FileDialog &fd, const ConnectionsData &conn, const MarksData &sprites, const EflgData &eflg, const std::array<ImapText, SCREEN_IMAPTEXT_COUNT> &texts)
{
    if (st.currentPath.empty()) { requestSaveAs(fd, st.currentPath); return; }
    std::string err;
    if (saveMapFileWithSprites(st.currentPath, conn, sprites, eflg, texts, err)) { st.dirty = false; st.addRecentFile(st.currentPath); saveConfig(st, fd); }
}

// Rebuilds the tile atlas AND the block atlas for one bank after a tile
// graphic changed (tiles_data was edited directly in place -- see the
// Tile Editor window). The block atlas is a baked-in render-to-texture
// snapshot (build_block_atlas), not a live view of the tile atlas, so it
// needs redoing too or edited tiles wouldn't show up on the map/palette.
static void rebuildBankAtlases(SDL_Renderer *renderer, SDL_Texture *tileAtlas[], SDL_Texture *blockAtlas[], int bank)
{
    SDL_DestroyTexture(blockAtlas[bank]);
    SDL_DestroyTexture(tileAtlas[bank]);
    tileAtlas[bank] = build_tile_atlas(renderer, bank);
    blockAtlas[bank] = build_block_atlas(renderer, tileAtlas[bank]);
}

// Rebuilds just the block atlas for one bank -- for when map_blocks
// changed (Block Editor) but the tile graphics themselves didn't, so
// there's no need to redecode+re-upload the tile atlas too.
static void rebuildBlockAtlasOnly(SDL_Renderer *renderer, SDL_Texture *tileAtlas[], SDL_Texture *blockAtlas[], int bank)
{
    SDL_DestroyTexture(blockAtlas[bank]);
    blockAtlas[bank] = build_block_atlas(renderer, tileAtlas[bank]);
}

// Color-codes a tile thumbnail's border by its hazard flags (map_eflg_c)
// -- a quick-glance cue anywhere a tile is drawn (Tile Editor's grid and
// preview, Block Editor's 4x4 cell grid and tile picker), so you don't
// have to open the Tile Editor to notice you just placed a lethal tile
// into a block. Only one color shows even if several flags are set --
// checked in priority order, first match wins:
//   Solid -> gray, Lethal -> red, Climb -> green, SuperPad -> yellow,
//   Fgnd -> white, WayUp -> blue with a gray hatch overlay (blue is also
//   the plain default border when none of these are set -- Vert/Bit01
//   don't get their own color here). WayUp keeps the default blue
//   underneath and adds dashes on top rather than a flat color, since a
//   real xrick level uses WayUp constantly on ordinary-looking
//   platforms and a solid color there would be visually as loud as
//   Lethal.
static void drawTileHazardBorder(ImDrawList *dl, ImVec2 rmin, ImVec2 rmax, uint8_t flags)
{
    const ImU32 gray   = ImGui::ColorConvertFloat4ToU32(HAZARD_COLOR_SOLID);
    const ImU32 red    = ImGui::ColorConvertFloat4ToU32(HAZARD_COLOR_LETHAL);
    const ImU32 green  = ImGui::ColorConvertFloat4ToU32(HAZARD_COLOR_CLIMB);
    const ImU32 blue   = ImGui::ColorConvertFloat4ToU32(HAZARD_COLOR_WAYUP);
    const ImU32 white  = ImGui::ColorConvertFloat4ToU32(HAZARD_COLOR_FGND);
    const ImU32 yellow = ImGui::ColorConvertFloat4ToU32(HAZARD_COLOR_SPAD);
    const float thickness = 2.0f;

    ImU32 color = blue;
    bool hatch = false;
    if (flags & EFLG_SOLID) color = gray;
    else if (flags & EFLG_LETHAL) color = red;
    else if (flags & EFLG_CLIMB) color = green;
    else if (flags & EFLG_SPAD) color = yellow;
    else if (flags & EFLG_FGND) color = white;
    else if (flags & EFLG_WAYUP) hatch = true; // stays blue, gets the hatch below

    dl->AddRect(rmin, rmax, color, 0.0f, 0, thickness);

    if (hatch)
    {
        const float dash = 4.0f, gap = 3.0f;
        for (float x = rmin.x; x < rmax.x; x += dash + gap)
            dl->AddLine(ImVec2(x, rmin.y), ImVec2(std::min(x + dash, rmax.x), rmin.y), gray, thickness);
        for (float x = rmin.x; x < rmax.x; x += dash + gap)
            dl->AddLine(ImVec2(x, rmax.y), ImVec2(std::min(x + dash, rmax.x), rmax.y), gray, thickness);
        for (float y = rmin.y; y < rmax.y; y += dash + gap)
            dl->AddLine(ImVec2(rmin.x, y), ImVec2(rmin.x, std::min(y + dash, rmax.y)), gray, thickness);
        for (float y = rmin.y; y < rmax.y; y += dash + gap)
            dl->AddLine(ImVec2(rmax.x, y), ImVec2(rmax.x, std::min(y + dash, rmax.y)), gray, thickness);
    }
}

// Rebuilds the sprite atlas texture in place -- for when sprites_data
// changed (Sprite Editor). Same "destroy and recreate" pattern as the
// tile/block atlas rebuild helpers above; takes the texture by
// reference since, unlike tileAtlas/blockAtlas, there's only one
// sprite atlas (not one per bank) so main() holds it as a plain
// variable rather than an array.
static void rebuildSpriteAtlas(SDL_Renderer *renderer, SDL_Texture *&spriteAtlas)
{
    SDL_DestroyTexture(spriteAtlas);
    spriteAtlas = build_sprite_atlas(renderer);
}

// Exports a tile bank as a PNG file. The image is a 128x128 atlas
// (16x16 tiles of 8px each), same layout as the SDL texture atlas.
static bool exportTileBankAsPNG(const fs::path &path, int bank)
{
    std::vector<Uint32> pixels(TILE_ATLAS_PX * TILE_ATLAS_PX, 0xFF000000u);
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
    // stb_image_write expects top-to-bottom; our atlas is already in
    // that order. Pixel format is ABGR in memory (0xAABBGGRR) which
    // stbi_write_png interprets as RGBA when comp=4, matching our
    // decode_tile() output layout.
    return stbi_write_png(path.string().c_str(), TILE_ATLAS_PX, TILE_ATLAS_PX, 4,
                          pixels.data(), TILE_ATLAS_PX * sizeof(Uint32)) != 0;
}

// Exports all sprites as a PNG file. The image is a sprite sheet
// (16 sprites per row, SPRITE_ATLAS_ROWS rows).
static bool exportSpritesAsPNG(const fs::path &path)
{
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
    return stbi_write_png(path.string().c_str(), SPRITE_ATLAS_W, SPRITE_ATLAS_H, 4,
                          pixels.data(), SPRITE_ATLAS_W * sizeof(Uint32)) != 0;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow(
        "RickEditor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1400, 900, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor: crisp pixel-art rendering

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    // Tile/block atlases for every available bank (0, 1, 2).
    SDL_Texture* tileAtlas[TILES_NBR_BANKS];
    SDL_Texture* blockAtlas[TILES_NBR_BANKS];
    for (int b = 0; b < TILES_NBR_BANKS; b++)
    {
        tileAtlas[b] = build_tile_atlas(renderer, b);
        blockAtlas[b] = build_block_atlas(renderer, tileAtlas[b]);
    }
    SDL_Texture* spriteAtlas = build_sprite_atlas(renderer);

    EditorState st;
    TileEditorState tileEditor;
    PixelEditorState pixelEditor;
    BlockEditorState blockEditor;
    SpriteEditorState spriteEditor;
    AssetsEditorState assetsEditor;
    assetsEditor.loadDefaults();
    TextEditorState textEditor;
    FileDialog fileDialog;
    ConnectionsData connections = defaultConnections();
    loadConfig(st, fileDialog);
    MarksData sprites = defaultMarks();
    EflgData eflg = defaultEflg();
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> mapTexts = defaultImapTexts();

    // Center the camera on map start position 1 at startup
    {
        int vw, vh; SDL_GetRendererOutputSize(renderer, &vw, &vh);
        const MapStartInfo &ms0 = connections.mapStarts[0];
        float worldX = (float)ms0.x;
        float worldY = (float)((ms0.row + 17) * TILE_PX); // +17 = SCRTOP offset, same as marker rendering
        st.cam.x = worldX - vw / (2.0f * st.cam.zoom);
        st.cam.y = worldY - vh / (2.0f * st.cam.zoom);
    }

    std::string patchResultMessage;
    bool patchResultOk = false;
    updateWindowTitle(window, st);
    bool running = true;

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE
                && event.window.windowID == SDL_GetWindowID(window)) running = false;

            bool mouseWantedByUI = io.WantCaptureMouse;
            bool kbWantedByUI = io.WantTextInput;

            if (!mouseWantedByUI)
            {
                if (event.type == SDL_MOUSEWHEEL)
                {
                    // Scrolls the map instead of zooming it -- zoom is
                    // still reachable via +/-, the top bar buttons, or
                    // Ctrl+wheel for people used to the old behavior.
                    const Uint8* ks = SDL_GetKeyboardState(nullptr);
                    bool ctrl = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
                    if (ctrl)
                    {
                        int mx, my; SDL_GetMouseState(&mx, &my);
                        float factor = event.wheel.y > 0 ? 1.15f : 1.0f / 1.15f;
                        zoomAt(st, (float)mx, (float)my, factor);
                    }
                    else
                    {
                        float step = 60.0f / st.cam.zoom;
                        st.cam.y -= event.wheel.y * step;
                        st.cam.x += event.wheel.x * step;
                    }
                }
                else if (event.type == SDL_MOUSEBUTTONDOWN)
                {
                    int col, row;
                    screenToCell(st, (float)event.button.x, (float)event.button.y, col, row);

                    if (event.button.button == SDL_BUTTON_MIDDLE)
                    {
                        st.panning = true; // panning works in every canvas mode
                        continue;
                    }

                    // Map start placement mode: click = hero position.
                    if (st.placeStartMode >= 0 && event.button.button == SDL_BUTTON_LEFT)
                    {
                        int tileCol = std::clamp(screenToTileCol(st, (float)event.button.x), 0, 31);
                        int tileRow = screenToTileRow(st, (float)event.button.y);
                        int roundedTileRow = (tileRow / 4) * 4;
                        int owner = submapForAbsRow(connections, roundedTileRow);
                        if (owner >= 0)
                        {
                            MapStartInfo &ms = connections.mapStarts[st.placeStartMode];
                            ms.x = tileCol * TILE_PX;
                            ms.y = 139;
                            ms.row = roundedTileRow - 16;
                            ms.submap = owner;
                            st.dirty = true;
                        }
                        st.placeStartMode = -1;
                        continue;
                    }

                    if (st.canvasMode == CanvasMode::Submap)
                        continue; // deliberately inert -- clicking the map does nothing

                    if (st.canvasMode == CanvasMode::Sprite)
                    {
                        int tileCol = std::clamp(screenToTileCol(st, (float)event.button.x), 0, 31);
                        int tileRow = screenToTileRow(st, (float)event.button.y);
                        if (event.button.button == SDL_BUTTON_LEFT)
                        {
                            int owner = submapForAbsRow(connections, tileRow);
                            if (owner >= 0)
                            {
                                // The real engine only respects rows that are a multiple
                                // of 8 LOCAL tile-rows from the submap's start (masks the
                                // raw byte with & 0xf8) -- fold the clicked row's remainder
                                // into fineY so it still lands exactly where clicked once
                                // patched into a real binary, instead of the real game
                                // silently rounding it down (appearing shifted UP by up to
                                // a block). See xrick_marks.h's snapMarkRowToBase().
                                int base = submapStartRow(connections.submaps[owner]);
                                int fineY = 0;
                                int snappedRow = snapMarkRowToBase(tileRow, base, fineY);
                                // Type-3 entities (id >= 0x18, e.g. traps) start asleep and
                                // never wake up without a TRIG* flag (see xrick_marks.h /
                                // e_them_t3_action in the real engine) -- default freshly
                                // placed ones to react to Rick walking into the trigger box,
                                // since that's the overwhelmingly common case. Other entity
                                // types (walkers, etc.) don't consult these flags, so 0 is fine.
                                int defaultFlags = (st.selectedEnt >= 0x18) ? ENT_FLG_TRIGRICK : 0;
                                if (st.selectedEnt == 39)
                                    defaultFlags |= ENT_FLG_LETHALI;
                                if (st.selectedEnt == 25 || st.selectedEnt == 26 || st.selectedEnt == 32)
                                    defaultFlags |= ENT_FLG_LETHALI;
                                if (st.selectedEnt == 53 || st.selectedEnt == 66)
                                    defaultFlags |= ENT_FLG_ONCE | ENT_FLG_LETHALI;
                                if (st.selectedEnt == 69)
                                    defaultFlags |= ENT_FLG_STOPRICK;
                                sprites.marks[owner].push_back(MarkEntry{snappedRow, tileCol, fineY, st.selectedEnt, defaultFlags, tileCol, 0});
                                st.dirty = true;
                            }
                        }
                        else if (event.button.button == SDL_BUTTON_RIGHT)
                        {
                            int fs, fi;
                            if (findMarkNear(sprites, tileCol, tileRow, fs, fi))
                            {
                                sprites.marks[fs].erase(sprites.marks[fs].begin() + fi);
                                st.dirty = true;
                            }
                        }
                        continue;
                    }

                    // CanvasMode::Block -- paint/select/eyedrop tiles.
                    if (event.button.button == SDL_BUTTON_LEFT)
                    {
                        if (st.pasting)
                        {
                            for (int r = 0; r < st.clip.height; r++)
                                for (int c = 0; c < st.clip.width; c++)
                                {
                                    int tc = col + c, tr = row + r;
                                    if (cellValid(tc, tr))
                                        map_bnums[mapIndex(tc, tr)] = st.clip.blocks[r * st.clip.width + c];
                                }

                            // Remove sprites in the paste target area.
                            int tileTargetMinC = col * 4, tileTargetMinR = row * 4;
                            int tileTargetMaxC = tileTargetMinC + st.clip.width * 4 - 1;
                            int tileTargetMaxR = tileTargetMinR + st.clip.height * 4 - 1;
                            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
                            {
                                auto &v = sprites.marks[s];
                                for (int i = (int)v.size() - 1; i >= 0; i--)
                                {
                                    int effRow = markEffectiveRow(v[i]);
                                    if (v[i].col >= tileTargetMinC && v[i].col <= tileTargetMaxC &&
                                        effRow >= tileTargetMinR && effRow <= tileTargetMaxR)
                                        v.erase(v.begin() + i);
                                }
                            }

                            // Paste sprites at the target position.
                            for (auto &cs : st.clip.sprites)
                            {
                                int newCol = col * 4 + cs.relTileCol;
                                int newRowAbs = row * 4 + cs.relRowAbs;
                                int newSubmap = submapForAbsRow(connections, newRowAbs);
                                if (newSubmap < 0 || newCol < 0 || newCol > 31) continue;

                                MarkEntry nm = cs.mark;
                                nm.col = newCol;
                                int base = submapStartRow(connections.submaps[newSubmap]);
                                int newFineY, newTrigRowOff;
                                nm.rowAbs = snapMarkRowToBase(newRowAbs, base, nm.fineY, nm.trigRowOffset, newFineY, newTrigRowOff);
                                nm.fineY = newFineY;
                                nm.trigRowOffset = newTrigRowOff;
                                sprites.marks[newSubmap].push_back(nm);
                            }

                            st.dirty = true;
                            st.pasting = false;
                            if (cellValid(col, row))
                                st.selectedBlock = map_bnums[mapIndex(col, row)];
                        }
                        else
                        {
                        const Uint8* ks = SDL_GetKeyboardState(nullptr);
                        bool shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
                        if (shift)
                        {
                            st.sel.active = true;
                            st.sel.dragging = true;
                            st.sel.c0 = st.sel.c1 = col;
                            st.sel.r0 = st.sel.r1 = row;
                        }
                        else
                        {
                            st.sel.active = false;
                            st.painting = true;
                            st.lastPaintCol = st.lastPaintRow = -1;
                            if (cellValid(col, row)) { map_bnums[mapIndex(col, row)] = st.selectedBlock; st.dirty = true; }
                            st.lastPaintCol = col; st.lastPaintRow = row;
                }
            }
        }
                    else if (event.button.button == SDL_BUTTON_RIGHT)
                    {
                        if (st.pasting) { st.pasting = false; }
                        else
                        {
                        // Right button = eyedropper: pick the block under
                        // the cursor into the palette selection.
                        st.picking = true;
                        if (cellValid(col, row)) st.selectedBlock = map_bnums[mapIndex(col, row)];
                        }
                    }
                }
                else if (event.type == SDL_MOUSEBUTTONUP)
                {
                    if (event.button.button == SDL_BUTTON_LEFT) { st.painting = false; st.sel.dragging = false; }
                    if (event.button.button == SDL_BUTTON_RIGHT) st.picking = false;
                    if (event.button.button == SDL_BUTTON_MIDDLE) st.panning = false;
                }
                else if (event.type == SDL_MOUSEMOTION)
                {
                    int col, row;
                    screenToCell(st, (float)event.motion.x, (float)event.motion.y, col, row);
                    if (st.panning)
                    {
                        st.cam.x -= event.motion.xrel / st.cam.zoom;
                        st.cam.y -= event.motion.yrel / st.cam.zoom;
                    }
                    if (st.sel.dragging)
                    {
                        st.sel.c1 = col; st.sel.r1 = row;
                    }
                    if (st.painting && (col != st.lastPaintCol || row != st.lastPaintRow))
                    {
                        if (cellValid(col, row)) { map_bnums[mapIndex(col, row)] = st.selectedBlock; st.dirty = true; }
                        st.lastPaintCol = col; st.lastPaintRow = row;
                    }
                    if (st.picking && cellValid(col, row))
                    {
                        st.selectedBlock = map_bnums[mapIndex(col, row)];
                    }
                    if (st.pasting)
                    {
                        st.pasteCol = col; st.pasteRow = row;
                    }
                }
            }

            if (!kbWantedByUI && event.type == SDL_KEYDOWN)
            {
                bool ctrl = (event.key.keysym.mod & KMOD_CTRL) != 0;
                if (ctrl && event.key.keysym.sym == SDLK_c && st.sel.active && st.canvasMode == CanvasMode::Block)
                {
                    int minC, minR, maxC, maxR;
                    st.sel.normalized(minC, minR, maxC, maxR);
                    st.clip.width = maxC - minC + 1;
                    st.clip.height = maxR - minR + 1;
                    st.clip.blocks.resize(st.clip.width * st.clip.height);
                    for (int r = 0; r < st.clip.height; r++)
                        for (int c = 0; c < st.clip.width; c++)
                            st.clip.blocks[r * st.clip.width + c] = map_bnums[mapIndex(minC + c, minR + r)];

                    // Capture sprites within the block selection (tile coords).
                    // Use rowAbs (coarse) for the relative offset so that
                    // snapMarkRowToBase doesn't double-count fineY.
                    int tileMinC = minC * 4, tileMaxC = maxC * 4 + 3;
                    int tileMinR = minR * 4, tileMaxR = maxR * 4 + 3;
                    st.clip.sprites.clear();
                    for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
                        for (auto &m : sprites.marks[s])
                        {
                            int effRow = markEffectiveRow(m);
                            if (m.col >= tileMinC && m.col <= tileMaxC && effRow >= tileMinR && effRow <= tileMaxR)
                                st.clip.sprites.push_back({m, m.col - tileMinC, m.rowAbs - tileMinR});
                        }

                    st.clip.hasData = true;
                }
                else if (ctrl && event.key.keysym.sym == SDLK_v && st.clip.hasData && st.canvasMode == CanvasMode::Block)
                    st.pasting = !st.pasting;
                else switch (event.key.keysym.sym)
                {
                    case SDLK_ESCAPE:
                        st.sel.active = false;
                        st.pasting = false;
                        st.placeStartMode = -1;
                        tileEditor.swapMode = false;
                        tileEditor.copyMode = false;
                        break;
                    case SDLK_DELETE:
                    case SDLK_BACKSPACE:
                        clearSelection(st, 0);
                        break;
                    case SDLK_f:
                        clearSelection(st, st.selectedBlock);
                        break;
                    case SDLK_s:
                        if (!event.key.repeat)
                            st.canvasMode = (st.canvasMode == CanvasMode::Sprite) ? CanvasMode::Block : CanvasMode::Sprite;
                        break;
                    case SDLK_g:
                        if (!event.key.repeat) st.showGrid = !st.showGrid;
                        break;
                    case SDLK_1: st.bank = 1; break;
                    case SDLK_2: st.bank = 2; break;
                    case SDLK_PLUS: case SDLK_KP_PLUS: case SDLK_EQUALS:
                    {
                        int vw, vh; SDL_GetRendererOutputSize(renderer, &vw, &vh);
                        zoomAt(st, vw / 2.0f, vh / 2.0f, 1.25f);
                        break;
                    }
                    case SDLK_MINUS: case SDLK_KP_MINUS:
                    {
                        int vw, vh; SDL_GetRendererOutputSize(renderer, &vw, &vh);
                        zoomAt(st, vw / 2.0f, vh / 2.0f, 1.0f / 1.25f);
                        break;
                    }
                    // Fast scroll: discrete jump of FAST_SCROLL_CELLS cells
                    // per key press (repeats automatically while held,
                    // thanks to SDL's native key-repeat events). Mirrors
                    // the original editor's Left/Right = +-16 rows
                    // convention, since the level is essentially a tall
                    // vertical strip 8 blocks wide.
                    case SDLK_LEFT: case SDLK_PAGEUP:
                        st.cam.y -= FAST_SCROLL_CELLS * (float)BLOCK_PX;
                        break;
                    case SDLK_RIGHT: case SDLK_PAGEDOWN:
                        st.cam.y += FAST_SCROLL_CELLS * (float)BLOCK_PX;
                        break;
                }
            }
        }

        // Continuous keyboard movement (Up/Down only -- fine, normal-speed
        // scroll). Left/Right/PageUp/PageDown use a discrete jump instead,
        // handled above in the KEYDOWN switch.
        if (!io.WantCaptureKeyboard)
        {
            const Uint8* ks = SDL_GetKeyboardState(nullptr);
            float speed = 480.0f / st.cam.zoom * io.DeltaTime;
            if (ks[SDL_SCANCODE_UP])       st.cam.y -= speed;
            if (ks[SDL_SCANCODE_DOWN])     st.cam.y += speed;
        }

        int viewportW, viewportH;
        SDL_GetRendererOutputSize(renderer, &viewportW, &viewportH);
        clampCamera(st, viewportW, viewportH);

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // --- Main menu bar (the only File access point, as requested --
        // no separate dockable window for it) ---
        // --- Main menu bar (File dropdown only -- needs a real ImGui
        // menu bar for BeginMenu()/MenuItem() to work at all; kept
        // separate from the toolbar below since it's the one thing here
        // that can't wrap to a second line by itself and realistically
        // never needs to). ---
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open...", "Ctrl+O")) requestOpen(fileDialog);
                if (!st.recentFiles.empty())
                {
                    if (ImGui::BeginMenu("Recent"))
                    {
                        for (int i = 0; i < (int)st.recentFiles.size(); i++)
                        {
                            std::string label = st.recentFiles[i].filename().string();
                            if (ImGui::MenuItem(label.c_str()))
                            {
                                std::string err;
                                if (loadMapFileWithSprites(st.recentFiles[i], connections, sprites, eflg, mapTexts, err))
                                {
                                    st.currentPath = st.recentFiles[i]; st.dirty = false; st.sel.active = false;
                                    rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 0);
                                    rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 1);
                                    rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 2);
                                    rebuildSpriteAtlas(renderer, spriteAtlas);
                                    int vw, vh; SDL_GetRendererOutputSize(renderer, &vw, &vh);
                                    const MapStartInfo &ms0 = connections.mapStarts[0];
                                    st.cam.x = (float)ms0.x - vw / (2.0f * st.cam.zoom);
                                    st.cam.y = (float)((ms0.row + 17) * TILE_PX) - vh / (2.0f * st.cam.zoom);
                                    st.addRecentFile(st.currentPath);
                                    saveConfig(st, fileDialog);
                                }
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
                if (ImGui::MenuItem("Save", "Ctrl+S")) doSave(st, fileDialog, connections, sprites, eflg, mapTexts);
                if (ImGui::MenuItem("Save As...")) requestSaveAs(fileDialog, st.currentPath);
                ImGui::Separator();
                if (ImGui::MenuItem("Patch xrick binary..."))
                {
                    fileDialog.show = true;
                    fileDialog.saveMode = false;
                    fileDialog.purpose = DialogPurpose::PickXrickBinary;
                    fileDialog.extFilter.clear(); // xrick executables have no fixed extension
                    fileDialog.filename[0] = '\0';
                    fileDialog.error.clear();
                }
                if (ImGui::MenuItem("Import from xrick binary..."))
                {
                    fileDialog.show = true;
                    fileDialog.saveMode = false;
                    fileDialog.purpose = DialogPurpose::PickXrickBinaryForConnections;
                    fileDialog.extFilter.clear();
                    fileDialog.filename[0] = '\0';
                    fileDialog.error.clear();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                bool canCopy = st.sel.active && st.canvasMode == CanvasMode::Block;
                bool canPaste = st.clip.hasData && st.canvasMode == CanvasMode::Block;
                bool canFill = st.sel.active && st.canvasMode == CanvasMode::Block;
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, canCopy))
                {
                    int minC, minR, maxC, maxR;
                    st.sel.normalized(minC, minR, maxC, maxR);
                    st.clip.width = maxC - minC + 1;
                    st.clip.height = maxR - minR + 1;
                    st.clip.blocks.resize(st.clip.width * st.clip.height);
                    for (int r = 0; r < st.clip.height; r++)
                        for (int c = 0; c < st.clip.width; c++)
                            st.clip.blocks[r * st.clip.width + c] = map_bnums[mapIndex(minC + c, minR + r)];

                    int tileMinC = minC * 4, tileMaxC = maxC * 4 + 3;
                    int tileMinR = minR * 4, tileMaxR = maxR * 4 + 3;
                    st.clip.sprites.clear();
                    for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
                        for (auto &m : sprites.marks[s])
                        {
                            int effRow = markEffectiveRow(m);
                            if (m.col >= tileMinC && m.col <= tileMaxC && effRow >= tileMinR && effRow <= tileMaxR)
                                st.clip.sprites.push_back({m, m.col - tileMinC, m.rowAbs - tileMinR});
                        }

                    st.clip.hasData = true;
                }
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste))
                    st.pasting = !st.pasting;
                if (ImGui::MenuItem("Fill with selection", "F", false, canFill))
                    clearSelection(st, st.selectedBlock);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem("Trigger boxes", nullptr, &st.showTriggerBoxes);
                ImGui::MenuItem("Map start positions", nullptr, &st.showMapStartPositions);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Tools"))
            {
                ImGui::MenuItem("Tile Editor", nullptr, &tileEditor.open);
                ImGui::MenuItem("Block Editor", nullptr, &blockEditor.open);
                ImGui::MenuItem("Sprite Editor", nullptr, &spriteEditor.open);
                ImGui::MenuItem("Assets Editor", nullptr, &assetsEditor.open);
                ImGui::MenuItem("Text Editor", nullptr, &textEditor.open);
                ImGui::EndMenu();
            }
            if (!st.currentPath.empty())
                ImGui::Text("  %s%s", st.currentPath.filename().string().c_str(), st.dirty ? " *" : "");
            ImGui::EndMainMenuBar();
        }
        const float menuBarH = ImGui::GetFrameHeight();

        // --- Toolbar (everything that used to live in the menu bar
        // besides File itself) -- a plain window, not a real ImGui menu
        // bar, specifically so it CAN wrap to a second line: menu bars
        // are hard-fixed to one line with no wrapping support at all, and
        // this row has grown enough editor toggles over time that it can
        // overflow a narrower window/screen. Wrapping works by calling
        // SameLine() then immediately checking whether that pushed the
        // cursor past the right edge -- if so, NewLine() overrides it and
        // drops to a fresh line. That's width-agnostic (unlike "assume
        // the next item is about the same width as this one", which the
        // tile/block grids elsewhere use safely only because every item
        // in THOSE rows is a uniform-size thumbnail); this row mixes
        // radio buttons, checkboxes, text, and small buttons of very
        // different widths, so the general version is worth the couple
        // extra lines of code. Real vertical-bar separators
        // (`ImGui::Separator()`) only render that way inside an actual
        // menu bar, so a light "|" glyph stands in for them here -- it's
        // just another wrap-checked item like anything else in the row. ---
        float toolbarH;
        {
            ImGui::SetNextWindowPos(ImVec2(0, menuBarH));
            ImGui::SetNextWindowSize(ImVec2((float)viewportW, 0)); // 0 height = auto-fit to content (1 or 2 lines)
            ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                          | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            float toolbarRight = ImGui::GetWindowPos().x + (float)viewportW - ImGui::GetStyle().WindowPadding.x;
            auto wrap = [&]()
            {
                ImGui::SameLine();
                if (ImGui::GetCursorScreenPos().x > toolbarRight) ImGui::NewLine();
            };
            auto divider = [&]()
            {
                ImGui::SameLine(0, 12);
                ImGui::TextDisabled("|");
                wrap();
            };

            struct ModeOpt { CanvasMode mode; const char *label; const char *tip; };
            static const ModeOpt modeOpts[] = {
                {CanvasMode::Submap, "Submap", "Look around and inspect Screen Connections, without editing anything"},
                {CanvasMode::Block,  "Block",  "Paint/pick tiles (block palette)"},
                {CanvasMode::Sprite, "Sprite", "Place/remove sprites (Sprite Tools) -- S"},
            };
            for (auto &opt : modeOpts)
            {
                bool active = st.canvasMode == opt.mode;
                if (ImGui::RadioButton(opt.label, active)) st.canvasMode = opt.mode;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opt.tip);
                wrap();
            }

            divider();
            ImGui::Checkbox("Grid", &st.showGrid);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the block grid (one square = one block) -- G");
            wrap();

            divider();
            ImGui::TextDisabled("Bank");
            for (int b = FIRST_USABLE_BANK; b <= LAST_USABLE_BANK; b++)
            {
                ImGui::SameLine();
                char label[16]; std::snprintf(label, sizeof label, "%d##tb", b);
                if (ImGui::RadioButton(label, st.bank == b)) st.bank = b;
            }
            wrap();

            divider();
            for (int i = 0; i < 4; i++)
            {
                ImGui::PushID(9000 + i);
                if (ImGui::SmallButton(std::to_string(i + 1).c_str()))
                {
                    int vw, vh; SDL_GetRendererOutputSize(renderer, &vw, &vh);
                    const MapStartInfo &ms0 = connections.mapStarts[i];
                    st.cam.x = (float)ms0.x - vw / (2.0f * st.cam.zoom);
                    st.cam.y = (float)((ms0.row + 17) * TILE_PX) - vh / (2.0f * st.cam.zoom);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scroll to map start %d", i + 1);
                wrap();
                ImGui::PopID();
            }

            divider();
            if (ImGui::SmallButton("-")) zoomAt(st, viewportW / 2.0f, viewportH / 2.0f, 1.0f / 1.25f);
            wrap();
            ImGui::Text("%.0f%%", st.cam.zoom * 100.0f);
            wrap();
            if (ImGui::SmallButton("+")) zoomAt(st, viewportW / 2.0f, viewportH / 2.0f, 1.25f);
            wrap();
            if (ImGui::SmallButton("Reset zoom")) { st.cam.zoom = 2.0f; }
            wrap();

            if (st.pasting)
            {
                divider();
                ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Paste mode: click to place, Esc to cancel");
                wrap();
            }

            if (st.placeStartMode >= 0)
            {
                divider();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Placing Map %d start: click on map, Esc to cancel", st.placeStartMode + 1);
                wrap();
            }

            divider();
            {
                int mx, my; SDL_GetMouseState(&mx, &my);
                int col, row;
                screenToCell(st, (float)mx, (float)my, col, row);
                int tileCol = std::clamp(screenToTileCol(st, (float)mx), 0, 31);
                int tileRow = screenToTileRow(st, (float)my);
                int topTileRow = (int)std::floor(st.cam.y / TILE_PX);
                int curSubmap = submapForAbsRow(connections, topTileRow);
                if (cellValid(col, row))
                    ImGui::Text("col %d, row %d (index %d) -- tile col %d, tile row %d -- submap %d", col, row, mapIndex(col, row), tileCol, tileRow, curSubmap);
                else
                    ImGui::Text("out of map -- tile col %d, tile row %d -- submap %d", tileCol, tileRow, curSubmap);
            }

            toolbarH = ImGui::GetWindowHeight();
            ImGui::End();
        }

        // Shared docked-panel geometry for the 3 mutually-exclusive tool
        // windows below (Block Palette / Screen Connections / Sprite
        // Tools -- exactly one is ever visible, gated by canvas mode).
        // Fixed to the right edge, full height under the menu bar, not
        // movable or resizable -- there's nothing to arrange, so letting
        // them float/overlap the canvas was just friction.
        const float toolPanelW = 430.0f; // wide enough for 8 block thumbnails/row with room for the scrollbar
        const float topBarH = menuBarH + toolbarH; // 1 or 2 toolbar lines, depending on whether it wrapped this frame
        const ImVec2 toolPanelPos((float)viewportW - toolPanelW, topBarH);
        const ImVec2 toolPanelSize(toolPanelW, (float)viewportH - topBarH);
        const ImGuiWindowFlags toolPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

        fs::path chosenPath;
        if (renderFileDialog(fileDialog, chosenPath, st))
        {
            std::string err;
            switch (fileDialog.purpose)
            {
                case DialogPurpose::SaveMap:
                    if (saveMapFileWithSprites(chosenPath, connections, sprites, eflg, mapTexts, err)) { st.currentPath = chosenPath; st.dirty = false; st.addRecentFile(chosenPath); saveConfig(st, fileDialog); }
                    else fileDialog.error = err;
                    break;
                case DialogPurpose::OpenMap:
                    if (loadMapFileWithSprites(chosenPath, connections, sprites, eflg, mapTexts, err))
                    {
                        st.currentPath = chosenPath; st.dirty = false; st.sel.active = false;
                        st.addRecentFile(chosenPath);
                        saveConfig(st, fileDialog);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 0);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 1);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 2);
                        rebuildSpriteAtlas(renderer, spriteAtlas);
                        // Center on map start position 1
                        int vw, vh; SDL_GetRendererOutputSize(renderer, &vw, &vh);
                        const MapStartInfo &ms0 = connections.mapStarts[0];
                        st.cam.x = (float)ms0.x - vw / (2.0f * st.cam.zoom);
                        st.cam.y = (float)((ms0.row + 17) * TILE_PX) - vh / (2.0f * st.cam.zoom);
                    }
                    else fileDialog.error = err;
                    break;
                case DialogPurpose::PickXrickBinary:
                {
                    // Encode ASCII text screens before patching
                    std::vector<std::vector<uint8_t>> textEncoded(3);
                    std::vector<std::pair<const char*, size_t>> textSlots;
                    for (int i = 0; i < 3; i++)
                    {
                        textEncoded[i] = encodeAsciiTextScreen(assetsEditor.asciiScreens[i]);
                        textSlots.push_back({ASCII_TEXT_SCREEN_SYMBOLS[i], ASCII_TEXT_SCREEN_SIZES[i]});
                    }
                    PatchResult r = patchXrickBinaryWithSprites(chosenPath, connections, sprites, eflg, mapTexts,
                        textSlots, textEncoded);
                    patchResultMessage = r.message;
                    patchResultOk = r.ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::PickXrickBinaryForConnections:
                {
                    std::string cerr;
                    bool ok = loadXrickConnections(chosenPath, connections, cerr);
                    if (ok) ok = loadXrickMarks(chosenPath, connections, sprites, cerr);
                    if (ok) ok = loadEflgFromXrickBinary(chosenPath, eflg, cerr);
                    if (ok) ok = loadTilesFromXrickBinary(chosenPath, cerr);
                    if (ok) ok = loadBlocksFromXrickBinary(chosenPath, cerr);
                    if (ok) ok = loadSpritesFromXrickBinary(chosenPath, cerr);
                    if (ok) ok = loadScreenTextsFromXrickBinary(chosenPath, mapTexts, cerr);
                    if (ok) loadAssetsFromXrickBinary(chosenPath, assetsEditor, cerr);
                    if (ok)
                    {
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 0);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 1);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 2);
                        rebuildSpriteAtlas(renderer, spriteAtlas);
                    }
                    patchResultMessage = ok
                        ? "Imported " + std::to_string(MAP_NBR_SUBMAPS) + " submaps (connections + sprites + tile "
                          "hazard flags + tile graphics + block composition + sprite graphics + intro text) from "
                          + chosenPath.filename().string()
                          + ". See the \"Screen Connections\", \"Sprite Tools\", Block Palette, Tile Editor, "
                            "Block Editor, Sprite Editor, Text Editor, and Assets Editor windows."
                        : cerr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ImportTileImage:
                {
                    std::string ierr;
                    bool ok = tileEditor.selectedTile >= 0
                        && importTileFromImage(chosenPath, tiles_data[tileEditor.bank][tileEditor.selectedTile], ierr);
                    if (ok)
                    {
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, tileEditor.bank);
                        st.dirty = true; // tile graphics are part of the .map file now (RKM6) -- flag for save
                        patchResultMessage = "Imported " + chosenPath.filename().string() + " into tile "
                            + std::to_string(tileEditor.selectedTile) + " (bank " + std::to_string(tileEditor.bank) + ").";
                    }
                    else patchResultMessage = ierr.empty() ? "No tile selected." : ierr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::BatchImportTileImage:
                {
                    std::string ierr;
                    BatchImportResult br;
                    bool ok = importTilesBatchFromImage(chosenPath, tileEditor.bank, tileEditor.batchStartTile, br, ierr);
                    if (ok)
                    {
                        if (br.imported > 0)
                        {
                            rebuildBankAtlases(renderer, tileAtlas, blockAtlas, tileEditor.bank);
                            st.dirty = true;
                        }
                        std::string msg = "Batch import from " + chosenPath.filename().string() + ": detected a "
                            + std::to_string(br.cols) + "x" + std::to_string(br.rows) + " grid of tiles (bank "
                            + std::to_string(tileEditor.bank) + ").\n";
                        if (br.imported > 0)
                            msg += "Imported " + std::to_string(br.imported) + " tile(s), into "
                                + std::to_string(br.startTile) + "-" + std::to_string(br.endTile) + ".\n";
                        else
                            msg += "No tiles imported.\n";
                        if (br.skippedOverflow > 0)
                            msg += std::to_string(br.skippedOverflow) + " tile(s) skipped: ran past tile 255 "
                                "(start tile " + std::to_string(br.startTile) + " + " + std::to_string(br.cols * br.rows)
                                + " image tiles overflows the bank).\n";
                        if (br.skippedUniform > 0)
                            msg += std::to_string(br.skippedUniform) + " tile(s) skipped: empty cell (all pixels "
                                "the same color) -- left whatever was already at that tile.\n";
                        if (br.leftoverPixelsX > 0 || br.leftoverPixelsY > 0)
                            msg += "Image size isn't a multiple of 8 -- ignored " + std::to_string(br.leftoverPixelsX)
                                + "px on the right and " + std::to_string(br.leftoverPixelsY) + "px at the bottom.";
                        patchResultMessage = msg;
                    }
                    else patchResultMessage = ierr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ImportSpriteImage:
                {
                    std::string ierr;
                    bool ok = spriteEditor.selectedSprite >= 0
                        && importSpriteFromImage(chosenPath, sprites_data[spriteEditor.selectedSprite], ierr);
                    if (ok)
                    {
                        rebuildSpriteAtlas(renderer, spriteAtlas);
                        st.dirty = true;
                        patchResultMessage = "Imported " + chosenPath.filename().string() + " into sprite "
                            + std::to_string(spriteEditor.selectedSprite) + ".";
                    }
                    else patchResultMessage = ierr.empty() ? "No sprite selected." : ierr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::BatchImportSpriteImage:
                {
                    std::string ierr;
                    SpriteBatchImportResult br;
                    bool ok = importSpritesBatchFromImage(chosenPath, spriteEditor.batchStartSprite, br, ierr);
                    if (ok)
                    {
                        if (br.imported > 0)
                        {
                            rebuildSpriteAtlas(renderer, spriteAtlas);
                            st.dirty = true;
                        }
                        std::string msg = "Batch import from " + chosenPath.filename().string() + ": detected a "
                            + std::to_string(br.cols) + "x" + std::to_string(br.rows) + " grid of sprites.\n";
                        if (br.imported > 0)
                            msg += "Imported " + std::to_string(br.imported) + " sprite(s), into "
                                + std::to_string(br.startSprite) + "-" + std::to_string(br.endSprite) + ".\n";
                        else
                            msg += "No sprites imported.\n";
                        if (br.skippedOverflow > 0)
                            msg += std::to_string(br.skippedOverflow) + " sprite(s) skipped: ran past sprite "
                                + std::to_string(SPRITES_NBR_SPRITES - 1) + " (start sprite "
                                + std::to_string(br.startSprite) + " + " + std::to_string(br.cols * br.rows)
                                + " image sprites overflows the table).\n";
                        if (br.skippedUniform > 0)
                            msg += std::to_string(br.skippedUniform) + " sprite(s) skipped: empty cell (all pixels "
                                "the same color, or fully transparent) -- left whatever was already at that sprite.\n";
                        if (br.leftoverPixelsX > 0 || br.leftoverPixelsY > 0)
                            msg += "Image size isn't a multiple of " + std::to_string(SPRITE_W) + "x"
                                + std::to_string(SPRITE_H) + " -- ignored " + std::to_string(br.leftoverPixelsX)
                                + "px on the right and " + std::to_string(br.leftoverPixelsY) + "px at the bottom.";
                        patchResultMessage = msg;
                    }
                    else patchResultMessage = ierr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ExportTileBankPNG:
                {
                    bool ok = exportTileBankAsPNG(chosenPath, 0);
                    patchResultMessage = ok
                        ? "Exported bank 0 tiles to " + chosenPath.filename().string() + "."
                        : "Failed to write " + chosenPath.filename().string() + ".";
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ExportSpritePNG:
                {
                    bool ok = exportSpritesAsPNG(chosenPath);
                    patchResultMessage = ok
                        ? "Exported sprites to " + chosenPath.filename().string() + "."
                        : "Failed to write " + chosenPath.filename().string() + ".";
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ImportBank0Image:
                {
                    std::string ierr;
                    bool ok = assetsEditor.selectedTile >= 0
                        && importTileFromImage(chosenPath, tiles_data[0][assetsEditor.selectedTile], ierr);
                    if (ok)
                    {
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 0);
                        st.dirty = true;
                        patchResultMessage = "Imported " + chosenPath.filename().string() + " into tile "
                            + std::to_string(assetsEditor.selectedTile) + " (bank 0).";
                    }
                    else patchResultMessage = ierr.empty() ? "No tile selected." : ierr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::BatchImportBank0Image:
                {
                    std::string ierr;
                    BatchImportResult br;
                    bool ok = importTilesBatchFromImage(chosenPath, 0, assetsEditor.batchStartTile, br, ierr);
                    if (ok)
                    {
                        if (br.imported > 0)
                        {
                            rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 0);
                            st.dirty = true;
                        }
                        std::string msg = "Batch import from " + chosenPath.filename().string() + ": detected a "
                            + std::to_string(br.cols) + "x" + std::to_string(br.rows) + " grid of tiles (bank 0).\n";
                        if (br.imported > 0)
                            msg += "Imported " + std::to_string(br.imported) + " tile(s), into "
                                + std::to_string(br.startTile) + "-" + std::to_string(br.endTile) + ".\n";
                        else
                            msg += "No tiles imported.\n";
                        if (br.skippedOverflow > 0)
                            msg += std::to_string(br.skippedOverflow) + " tile(s) skipped: ran past tile 255 "
                                "(start tile " + std::to_string(br.startTile) + " + " + std::to_string(br.cols * br.rows)
                                + " image tiles overflows the bank).\n";
                        if (br.skippedUniform > 0)
                            msg += std::to_string(br.skippedUniform) + " tile(s) skipped: empty cell (all pixels "
                                "the same color) -- left whatever was already at that tile.\n";
                        if (br.leftoverPixelsX > 0 || br.leftoverPixelsY > 0)
                            msg += "Image size isn't a multiple of 8 -- ignored " + std::to_string(br.leftoverPixelsX)
                                + "px on the right and " + std::to_string(br.leftoverPixelsY) + "px at the bottom.";
                        patchResultMessage = msg;
                    }
                    else patchResultMessage = ierr;
                    patchResultOk = ok;
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ExportPicPNG:
                {
                    int pi = assetsEditor.exportPicIdx;
                    if (pi < 0 || pi >= PIC_COUNT || assetsEditor.pics.pics[pi].empty())
                    {
                        patchResultMessage = "No picture data to export.";
                        patchResultOk = false;
                    }
                    else
                    {
                        const BitmapPic &pic = assetsEditor.pics.pics[pi];
                        bool ok = stbi_write_png(chosenPath.string().c_str(), pic.w, pic.h, 4,
                                                  pic.pixels.data(), pic.w * sizeof(uint32_t));
                        patchResultMessage = ok
                            ? "Exported " + std::string(PIC_LABELS[pi]) + " as " + chosenPath.filename().string()
                            : "Failed to write PNG.";
                        patchResultOk = ok;
                    }
                    ImGui::OpenPopup("Result");
                    break;
                }
                case DialogPurpose::ImportPicImage:
                {
                    int pi = assetsEditor.importPicIdx;
                    if (pi < 0 || pi >= PIC_COUNT)
                    {
                        patchResultMessage = "No picture slot selected.";
                        patchResultOk = false;
                    }
                    else
                    {
                        std::string ierr;
                        bool ok = importPicFromImage(chosenPath, assetsEditor.pics.pics[pi],
                                                     PIC_W[pi], PIC_H[pi], ierr);
                        if (ok)
                        {
                            assetsEditor.pics.loaded[pi] = true;
                            st.dirty = true;
                            patchResultMessage = "Imported " + chosenPath.filename().string()
                                + " into " + std::string(PIC_LABELS[pi]) + " (resampled to "
                                + std::to_string(PIC_W[pi]) + "x" + std::to_string(PIC_H[pi]) + ").";
                        }
                        else patchResultMessage = ierr.empty() ? "Import failed." : ierr;
                        patchResultOk = ok;
                    }
                    ImGui::OpenPopup("Result");
                    break;
                }
            }
        }

        if (ImGui::BeginPopupModal("Result", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(420.0f);
            if (patchResultOk)
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", patchResultMessage.c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", patchResultMessage.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // --- Block palette -- only relevant (and shown) in Block mode ---
        if (st.canvasMode == CanvasMode::Block)
        {
            ImGui::SetNextWindowPos(toolPanelPos);
            ImGui::SetNextWindowSize(toolPanelSize);
            ImGui::Begin("Block Palette", nullptr, toolPanelFlags);
            ImGui::Text("Selected block: %d", st.selectedBlock);
            ImGui::Separator();

            ImTextureID atlasTexId = (ImTextureID)(intptr_t)blockAtlas[st.bank];
            float thumb = 40.0f;
            // Robust wrapping: decide whether to continue the row AFTER
            // placing each button, based on where it actually landed,
            // instead of pre-computing a fixed "items per row" from the
            // window width (thumb+padding estimates never quite match
            // ImageButton's real frame padding/border, so the guessed
            // count was consistently off by one -- block 7, 14, 21...
            // silently fell just past the edge and needed a horizontal
            // scroll to reach). Same pattern already used for the
            // sprite quick-pick chips and the flags checkboxes.
            ImGuiStyle &blkStyle = ImGui::GetStyle();
            float blkWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            for (int b = 0; b < 0x100; b++)
            {
                ImGui::PushID(b);
                float u0 = (float)(b % ATLAS_BLOCKS_PER_ROW) / ATLAS_BLOCKS_PER_ROW;
                float v0 = (float)(b / ATLAS_BLOCKS_PER_ROW) / ATLAS_BLOCKS_PER_ROW;
                float u1 = u0 + 1.0f / ATLAS_BLOCKS_PER_ROW;
                float v1 = v0 + 1.0f / ATLAS_BLOCKS_PER_ROW;

                bool selected = (b == st.selectedBlock);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                if (ImGui::ImageButton("##blk", atlasTexId, ImVec2(thumb, thumb), ImVec2(u0, v0), ImVec2(u1, v1)))
                    st.selectedBlock = b;
                if (selected) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Block %d", b);
                float nextRight = ImGui::GetItemRectMax().x + blkStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                if (b != 255 && nextRight < blkWindowRight)
                    ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::End();
        }

        // --- Tile Editor -- standalone window, independent of canvas mode.
        // Edits tile GRAPHICS (tiles_data, via image import) and per-tile
        // hazard flags (eflg), for one bank at a time. Not persisted to
        // .map or patched into an xrick binary yet -- graphics edits live
        // only in this session for now, same as any other in-memory state
        // until that's wired up. ---
        if (tileEditor.open)
        {
            ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
            ImGui::Begin("Tile Editor", &tileEditor.open);

            ImGui::Text("Tile bank:");
            ImGui::SameLine();
            {
                char label0[24]; std::snprintf(label0, sizeof label0, "0 (font/decor)##tebank0");
                if (ImGui::RadioButton(label0, tileEditor.bank == 0)) tileEditor.bank = 0;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bank 0 isn't unused -- it's the font (see the Text "
                                                            "Editor) AND the between-levels cutscene decor (see "
                                                            "below). Hidden from Block Palette/Block Editor since "
                                                            "no block ever uses it in-game.");
            ImGui::SameLine();
            for (int b = FIRST_USABLE_BANK; b <= LAST_USABLE_BANK; b++)
            {
                ImGui::SameLine();
                char label[16]; std::snprintf(label, sizeof label, "%d##tebank", b);
                if (ImGui::RadioButton(label, tileEditor.bank == b)) tileEditor.bank = b;
            }
            ImGui::Separator();

            // Cutscene decor shortcuts: bank 0, tiles laid out in a 6x6
            // block per map (see screens_text.h) -- jump the bank/batch-
            // start fields above to the right spot with one click,
            // rather than needing to know/type the magic tile numbers.
            if (ImGui::CollapsingHeader("Cutscene decor (bank 0)"))
            {
                ImGui::Indent();
                ImGui::TextWrapped("Background scenery behind Rick on the between-levels intro screen -- a "
                                    "6x6 block of tiles (48x48px) per map, bank 0. Same bank as the font (Text "
                                    "Editor), different tile range -- they don't overlap.");
                ImTextureID decorTexId = (ImTextureID)(intptr_t)tileAtlas[0];
                for (int m = 0; m < SCREEN_IMAPTEXT_COUNT; m++)
                {
                    ImGui::PushID(1000 + m);
                    int start = SCREEN_IMAP_DECOR_START_TILE[m];
                    ImGui::BeginGroup();
                    for (int r = 0; r < SCREEN_IMAP_DECOR_ROWS; r++)
                    {
                        for (int c = 0; c < SCREEN_IMAP_DECOR_COLS; c++)
                        {
                            int t = start + r * SCREEN_IMAP_DECOR_COLS + c;
                            float u0 = (float)(t % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                            float v0 = (float)(t / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                            float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                            float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;
                            ImGui::Image(decorTexId, ImVec2(12, 12), ImVec2(u0, v0), ImVec2(u1, v1));
                            if (c != SCREEN_IMAP_DECOR_COLS - 1) ImGui::SameLine(0, 0);
                        }
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("%s (tiles %d-%d)", SCREEN_IMAPTEXT_LABELS[m], start,
                                start + SCREEN_IMAP_DECOR_COLS * SCREEN_IMAP_DECOR_ROWS - 1);
                    if (ImGui::SmallButton("Batch import into this decor..."))
                    {
                        tileEditor.bank = 0;
                        tileEditor.batchStartTile = start;
                        fileDialog.show = true;
                        fileDialog.saveMode = false;
                        fileDialog.purpose = DialogPurpose::BatchImportTileImage;
                        fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                        fileDialog.filename[0] = '\0';
                        fileDialog.error.clear();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets bank to 0 and batch start tile to %d, then "
                                                                   "opens the image picker -- pick a 48x48 (or any "
                                                                   "size, resampled to a 6x6 tile grid) image to "
                                                                   "replace this whole decor at once.", start);
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
                ImGui::Unindent();
            }
            ImGui::Separator();

            // Batch import: doesn't depend on the tile grid selection
            // below (you're picking a *destination range*, not editing
            // one already-selected tile), so it lives here, above the
            // grid/detail split.
            if (ImGui::CollapsingHeader("Batch import..."))
            {
                ImGui::Indent();
                ImGui::TextWrapped("Slices one image into consecutive 8x8 tiles, left-to-right then "
                                    "top-to-bottom, starting at the tile number below (bank %d). No "
                                    "resampling -- each tile must be an exact 8x8 pixel cell of the image.",
                                    tileEditor.bank);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Start tile", &tileEditor.batchStartTile);
                tileEditor.batchStartTile = std::clamp(tileEditor.batchStartTile, 0, 255);
                if (ImGui::Button("Choose image..."))
                {
                    fileDialog.show = true;
                    fileDialog.saveMode = false;
                    fileDialog.purpose = DialogPurpose::BatchImportTileImage;
                    fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                    fileDialog.filename[0] = '\0';
                    fileDialog.error.clear();
                }
                ImGui::Unindent();
            }
            ImGui::Separator();

            // Left: scrollable grid of all 256 tiles for the selected
            // bank. Right: detail/edit panel for whichever tile is
            // currently selected. Same width split as the old Tools/map
            // layout, just local to this window instead of the whole app.
            float detailW = 210.0f;
            ImGui::BeginChild("##tileGrid", ImVec2(-detailW, 0), true);
            {
                ImTextureID tileTexId = (ImTextureID)(intptr_t)tileAtlas[tileEditor.bank];
                float thumb = 32.0f;
                ImGuiStyle &teStyle = ImGui::GetStyle();
                float teWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                for (int t = 0; t < 0x100; t++)
                {
                    ImGui::PushID(t);
                    float u0 = (float)(t % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                    float v0 = (float)(t / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                    float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                    float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;

                    bool selected = (t == tileEditor.selectedTile);
                    bool multiSel = tileEditor.multiSelected.count(t) > 0;
                    bool stylePushed = false;
                    if (selected) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f)); stylePushed = true; }
                    else if (multiSel) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.35f, 0.70f, 1.0f)); stylePushed = true; }
                    else if (tileEditor.swapMode) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.45f, 0.10f, 1.0f)); stylePushed = true; }
                    else if (tileEditor.copyMode) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.65f, 0.30f, 1.0f)); stylePushed = true; }
                    if (ImGui::ImageButton("##tile", tileTexId, ImVec2(thumb, thumb), ImVec2(u0, v0), ImVec2(u1, v1)))
                    {
                        ImGuiIO &io = ImGui::GetIO();
                        if (tileEditor.swapMode && tileEditor.selectedTile >= 0 && t != tileEditor.selectedTile)
                        {
                            int a = tileEditor.selectedTile, b = t;
                            int bank = tileEditor.bank;
                            // Swap tile graphics
                            tile_t tmp;
                            std::memcpy(tmp, tiles_data[bank][a], sizeof(tile_t));
                            std::memcpy(tiles_data[bank][a], tiles_data[bank][b], sizeof(tile_t));
                            std::memcpy(tiles_data[bank][b], tmp, sizeof(tile_t));
                            // Swap hazard flags (banks 1+ only; bank 0 has no eflg)
                            if (bank >= 1)
                            {
                                uint8_t tmpF = eflg.bank[bank - 1][a];
                                eflg.bank[bank - 1][a] = eflg.bank[bank - 1][b];
                                eflg.bank[bank - 1][b] = tmpF;
                            }
                            // Update all blocks that reference these tiles
                            for (int bl = 0; bl < 0x100; bl++)
                                for (int s = 0; s < 16; s++)
                                {
                                    if (map_blocks[bl][s] == a) map_blocks[bl][s] = b;
                                    else if (map_blocks[bl][s] == b) map_blocks[bl][s] = a;
                                }
                            rebuildBankAtlases(renderer, tileAtlas, blockAtlas, bank);
                            tileEditor.swapMode = false;
                            tileEditor.selectedTile = b;
                        }
                        else if (tileEditor.copyMode && tileEditor.hasCopiedTile)
                        {
                            int bank = tileEditor.bank;
                            std::memcpy(tiles_data[bank][t], tileEditor.copiedTile, sizeof(tile_t));
                            rebuildBankAtlases(renderer, tileAtlas, blockAtlas, bank);
                            tileEditor.copyMode = false;
                            tileEditor.selectedTile = t;
                        }
                        else if (io.KeyShift && tileEditor.anchorTile >= 0)
                        {
                            // Shift+click: range select from anchor to clicked tile
                            int a = tileEditor.anchorTile;
                            int lo = std::min(a, t), hi = std::max(a, t);
                            tileEditor.multiSelected.clear();
                            for (int i = lo; i <= hi; i++)
                                tileEditor.multiSelected.insert(i);
                            tileEditor.selectedTile = t;
                        }
                        else if (io.KeyCtrl)
                        {
                            // Ctrl+click: toggle multi-selection
                            auto it = tileEditor.multiSelected.find(t);
                            if (it != tileEditor.multiSelected.end())
                                tileEditor.multiSelected.erase(it);
                            else
                                tileEditor.multiSelected.insert(t);
                            tileEditor.anchorTile = t;
                        }
                        else
                        {
                            tileEditor.selectedTile = t;
                            tileEditor.batchStartTile = t;
                            tileEditor.multiSelected.clear();
                            tileEditor.anchorTile = t;
                        }
                    }
                    if (stylePushed) ImGui::PopStyleColor();
                    drawTileHazardBorder(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), eflg.bank[tileEditor.bank - 1][t]);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile %d", t);
                    float nextRight = ImGui::GetItemRectMax().x + teStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                    if (t != 255 && nextRight < teWindowRight)
                        ImGui::SameLine();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##tileDetail", ImVec2(detailW, 0));
            {
                if (!tileEditor.multiSelected.empty())
                {
                    ImGui::Text("%d tiles selected (bank %d)", (int)tileEditor.multiSelected.size(), tileEditor.bank);
                    ImGui::Spacing();
                    if (ImGui::SmallButton("Clear selection"))
                        tileEditor.multiSelected.clear();
                    ImGui::Spacing();
                    if (ImGui::SmallButton("Delete selected"))
                        tileEditor.confirmDelete = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear all pixels of %d selected tiles (cannot be undone)", (int)tileEditor.multiSelected.size());
                    if (tileEditor.confirmDelete)
                    {
                        ImGui::OpenPopup("Delete tiles?");
                        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
                        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                        if (ImGui::BeginPopupModal("Delete tiles?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                        {
                            ImGui::Text("This will clear all pixels of %d tiles in bank %d.\nThis cannot be undone.",
                                        (int)tileEditor.multiSelected.size(), tileEditor.bank);
                            ImGui::Separator();
                            if (ImGui::Button("Delete", ImVec2(120, 0)))
                            {
                                for (int t : tileEditor.multiSelected)
                                    for (int y = 0; y < 8; y++)
                                        tiles_data[tileEditor.bank][t][y] = 0;
                                rebuildBankAtlases(renderer, tileAtlas, blockAtlas, tileEditor.bank);
                                st.dirty = true;
                                tileEditor.multiSelected.clear();
                                tileEditor.selectedTile = -1;
                                tileEditor.confirmDelete = false;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel", ImVec2(120, 0)))
                            {
                                tileEditor.confirmDelete = false;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                    }
                }
                else if (tileEditor.selectedTile < 0)
                {
                    ImGui::TextWrapped("Click a tile on the left to select it.");
                }
                else
                {
                    ImGui::Text("Tile %d (bank %d)", tileEditor.selectedTile, tileEditor.bank);
                    ImGui::Spacing();

                    ImTextureID tileTexId = (ImTextureID)(intptr_t)tileAtlas[tileEditor.bank];
                    float u0 = (float)(tileEditor.selectedTile % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                    float v0 = (float)(tileEditor.selectedTile / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                    float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                    float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;
                    float big = 128.0f;
                    ImGui::Image(tileTexId, ImVec2(big, big), ImVec2(u0, v0), ImVec2(u1, v1));
                    drawTileHazardBorder(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), eflg.bank[tileEditor.bank - 1][tileEditor.selectedTile]);

                    ImGui::Spacing();
                    if (ImGui::Button("Import from image...", ImVec2(-1, 0)))
                    {
                        fileDialog.show = true;
                        fileDialog.saveMode = false;
                        fileDialog.purpose = DialogPurpose::ImportTileImage;
                        fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                        fileDialog.filename[0] = '\0';
                        fileDialog.error.clear();
                    }
                    ImGui::TextWrapped("Any image size works -- it's resampled to 8x8 and each pixel "
                                        "is matched to the closest of the 16 game colors.");

                    ImGui::Spacing();
                    if (ImGui::SmallButton(tileEditor.swapMode ? "Cancel swap" : "Swap with..."))
                    {
                        tileEditor.swapMode = !tileEditor.swapMode;
                        tileEditor.copyMode = false;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap this tile's graphics, hazard flags and block references with another tile");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tileEditor.copyMode ? "Cancel copy" : "Copy to..."))
                    {
                        if (!tileEditor.copyMode)
                        {
                            std::memcpy(tileEditor.copiedTile, tiles_data[tileEditor.bank][tileEditor.selectedTile], sizeof(tile_t));
                            tileEditor.hasCopiedTile = true;
                        }
                        tileEditor.copyMode = !tileEditor.copyMode;
                        tileEditor.swapMode = false;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy this tile's graphics and hazard flags,\nthen click another tile to paste");
                    if (ImGui::SmallButton("Delete"))
                    {
                        for (int y = 0; y < 8; y++)
                            tiles_data[tileEditor.bank][tileEditor.selectedTile][y] = 0;
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, tileEditor.bank);
                        st.dirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set all pixels of this tile to black (color 0)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Edit pixels"))
                    {
                        pixelEditor.open = true;
                        pixelEditor.target = PixelEditorState::Tile;
                        pixelEditor.bank = tileEditor.bank;
                        pixelEditor.index = tileEditor.selectedTile;
                        pixelEditor.color = 1;
                        std::memcpy(pixelEditor.backupTile, tiles_data[tileEditor.bank][tileEditor.selectedTile], sizeof(tile_t));
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open pixel editor for this tile (8x8, 16 colors)");

                    ImGui::Separator();
                    ImGui::Text("Hazard flags (this tile, bank %d):", tileEditor.bank);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("map_eflg_c -- what actually makes a TILE solid, "
                                                                   "climbable, lethal, etc. (see xrick_eflg.h). "
                                                                   "Edits this tile's byte directly.");
                    uint8_t &flags = eflg.bank[tileEditor.bank - 1][tileEditor.selectedTile];
                    auto flagBit = [&](const char *label, int bit, const char *tip, const ImVec4 *color = nullptr)
                    {
                        bool on = (flags & bit) != 0;
                        if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
                        if (ImGui::Checkbox(label, &on))
                        {
                            flags = on ? (flags | bit) : (flags & ~bit);
                            st.dirty = true;
                        }
                        if (color) ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                    };
                    // Solid/Lethal/Climb/WayUp get the same color here as
                    // they do on a tile's border (drawTileHazardBorder())
                    // -- Vert/Bit01 don't have a dedicated border color
                    // of their own, so their labels stay the normal
                    // text color.
                    flagBit("Solid", EFLG_SOLID, "SOLID -- can't walk/fall through", &HAZARD_COLOR_SOLID);
                    flagBit("Lethal", EFLG_LETHAL, "LETHAL -- kills an entity that touches it (this is the corpse-transform trigger)", &HAZARD_COLOR_LETHAL);
                    flagBit("Climb", EFLG_CLIMB, "CLIMB -- entities can climb here", &HAZARD_COLOR_CLIMB);
                    flagBit("Vert", EFLG_VERT, "VERT -- vertical move only (usually paired with Climb)");
                    flagBit("WayUp", EFLG_WAYUP, "WAYUP -- solid except when moving up through it (jump-through platform)", &HAZARD_COLOR_WAYUP);
                    flagBit("SuperPad", EFLG_SPAD, "SPAD -- solid, but bounces entities skyward", &HAZARD_COLOR_SPAD);
                    flagBit("Fgnd", EFLG_FGND, "FGND -- foreground, drawn in front of / hides entities", &HAZARD_COLOR_FGND);
                    flagBit("Bit01", EFLG_01, "Undocumented bit in the original source -- exposed raw");

                    ImGui::Spacing();
                    {
                        int runCount = (int)eflgRunsFromTable(eflg.bank[tileEditor.bank - 1]).size();
                        ImGui::TextDisabled("%d of 8 tile ranges used in bank %d", runCount, tileEditor.bank);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The real format only has room for 8 contiguous "
                                                                       "[start-end] ranges per bank, each sharing one "
                                                                       "set of flags -- editing tiles whose neighbors "
                                                                       "have different flags can split a range, and "
                                                                       "\"Patch xrick binary...\"/Save will refuse if "
                                                                       "that pushes a bank past 8.");
                    }
                }
            }
            ImGui::EndChild();

            ImGui::End();
        }

        // --- Pixel Editor -- standalone window for editing tile/sprite pixels
        if (pixelEditor.open)
        {
            const char *title = pixelEditor.target == PixelEditorState::Tile
                ? "Pixel Editor (tile)" : "Pixel Editor (sprite)";

            int pixW = pixelEditor.target == PixelEditorState::Tile ? 8 : 32;
            int pixH = pixelEditor.target == PixelEditorState::Tile ? 8 : 21;
            float cellPx = 14.0f;
            float paletteW = 120.0f;
            float gridW = pixW * cellPx;
            float gridH = pixH * cellPx;
            float childExtra = ImGui::GetStyle().WindowPadding.x + ImGui::GetStyle().FramePadding.x * 2;
            float childW = gridW + childExtra;
            float childH = gridH + childExtra;
            float buttonsH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
            float titleH = ImGui::GetFrameHeight();
            float winW = childW + paletteW + ImGui::GetStyle().WindowPadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
            float winH = childH + buttonsH + titleH + ImGui::GetStyle().WindowPadding.y * 2 + ImGui::GetStyle().ItemSpacing.y;
            ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
            ImGui::Begin(title, &pixelEditor.open);
            ImGui::BeginChild("##pixGrid", ImVec2(childW, childH), true);
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 origin = ImGui::GetCursorScreenPos();
                for (int y = 0; y < pixH; y++)
                {
                    for (int x = 0; x < pixW; x++)
                    {
                        ImGui::PushID(y * pixW + x);
                        ImVec2 p0(origin.x + x * cellPx, origin.y + y * cellPx);
                        ImVec2 p1(p0.x + cellPx, p0.y + cellPx);
                        int cidx;
                        if (pixelEditor.target == PixelEditorState::Tile)
                            cidx = getTilePixel(tiles_data[pixelEditor.bank][pixelEditor.index], x, y);
                        else
                            cidx = getSpritePixel(sprites_data[pixelEditor.index], x, y);
                        ImVec4 col(RED[cidx] / 255.0f, GREEN[cidx] / 255.0f, BLUE[cidx] / 255.0f, 1.0f);
                        dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(col));
                        dl->AddRect(p0, p1, IM_COL32(60, 60, 60, 255));
                        // Invisible button for click detection
                        ImGui::SetCursorScreenPos(p0);
                        ImGui::InvisibleButton("##px", ImVec2(cellPx, cellPx));
                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
                        {
                            if (pixelEditor.target == PixelEditorState::Tile)
                                setTilePixel(tiles_data[pixelEditor.bank][pixelEditor.index], x, y, pixelEditor.color);
                            else
                                setSpritePixel(sprites_data[pixelEditor.index], x, y, pixelEditor.color);
                            st.dirty = true;
                            if (pixelEditor.target == PixelEditorState::Tile)
                                rebuildBankAtlases(renderer, tileAtlas, blockAtlas, pixelEditor.bank);
                            else
                                rebuildSpriteAtlas(renderer, spriteAtlas);
                        }
                        else if (ImGui::IsItemClicked(0))
                        {
                            if (pixelEditor.target == PixelEditorState::Tile)
                                setTilePixel(tiles_data[pixelEditor.bank][pixelEditor.index], x, y, pixelEditor.color);
                            else
                                setSpritePixel(sprites_data[pixelEditor.index], x, y, pixelEditor.color);
                            st.dirty = true;
                            if (pixelEditor.target == PixelEditorState::Tile)
                                rebuildBankAtlases(renderer, tileAtlas, blockAtlas, pixelEditor.bank);
                            else
                                rebuildSpriteAtlas(renderer, spriteAtlas);
                        }
                        else if (ImGui::IsItemClicked(1))
                        {
                            if (pixelEditor.target == PixelEditorState::Tile)
                                pixelEditor.color = getTilePixel(tiles_data[pixelEditor.bank][pixelEditor.index], x, y);
                            else
                                pixelEditor.color = getSpritePixel(sprites_data[pixelEditor.index], x, y);
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##pixPalette", ImVec2(paletteW, gridH));
            {
                ImGui::Text("Color: %d", pixelEditor.color);
                float swatch = 20.0f;
                for (int i = 0; i < 16; i++)
                {
                    if (i % 4 != 0) ImGui::SameLine(0, 2);
                    ImVec4 col(RED[i] / 255.0f, GREEN[i] / 255.0f, BLUE[i] / 255.0f, 1.0f);
                    if (pixelEditor.color == i)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x * 1.3f, col.y * 1.3f, col.z * 1.3f, 1.0f));
                    else
                        ImGui::PushStyleColor(ImGuiCol_Button, col);
                    char id[8]; snprintf(id, sizeof(id), "##c%d", i);
                    if (ImGui::Button(id, ImVec2(swatch, swatch)))
                        pixelEditor.color = i;
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
                ImVec4 curCol(RED[pixelEditor.color] / 255.0f, GREEN[pixelEditor.color] / 255.0f, BLUE[pixelEditor.color] / 255.0f, 1.0f);
                ImGui::ColorButton("##cur", curCol, 0, ImVec2(paletteW - 10, 30));
                ImGui::Spacing();
                ImGui::TextDisabled("Left: paint");
                ImGui::TextDisabled("Right: pick color");
            }
            ImGui::EndChild();

            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(80, 0)))
            {
                pixelEditor.open = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0)))
            {
                if (pixelEditor.target == PixelEditorState::Tile)
                    std::memcpy(tiles_data[pixelEditor.bank][pixelEditor.index], pixelEditor.backupTile, sizeof(tile_t));
                else
                    std::memcpy(sprites_data[pixelEditor.index], pixelEditor.backupSprite, sizeof(sprite_t));
                if (pixelEditor.target == PixelEditorState::Tile)
                    rebuildBankAtlases(renderer, tileAtlas, blockAtlas, pixelEditor.bank);
                else
                    rebuildSpriteAtlas(renderer, spriteAtlas);
                st.dirty = true;
                pixelEditor.open = false;
            }

            ImGui::End();
        }

        // --- Block Editor -- standalone window, independent of canvas
        // mode. Edits which tile goes in each of a block's 16 cells
        // (map_blocks) -- shared across both tile banks (a block's
        // layout doesn't depend on which bank's graphics render it), so
        // there's only one bank selector here and it's preview-only. ---
        if (blockEditor.open)
        {
            ImGui::SetNextWindowSize(ImVec2(700, 560), ImGuiCond_FirstUseEver);
            ImGui::Begin("Block Editor", &blockEditor.open);

            ImGui::Text("Preview bank:");
            ImGui::SameLine();
            for (int b = FIRST_USABLE_BANK; b <= LAST_USABLE_BANK; b++)
            {
                if (b > FIRST_USABLE_BANK) ImGui::SameLine();
                char label[16]; std::snprintf(label, sizeof label, "%d##bebank", b);
                if (ImGui::RadioButton(label, blockEditor.bank == b)) blockEditor.bank = b;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Block composition is shared by both banks -- this only "
                                                            "picks which bank's tile graphics preview here");
            ImGui::Separator();

            ImGui::Separator();
            if (blockEditor.swapMode)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Swap mode: click a block on the left to swap "
                                                                     "with block %d.", blockEditor.selectedBlock);
            if (blockEditor.copyMode)
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Copy mode: click a block on the left to "
                                                                    "overwrite it with block %d.", blockEditor.selectedBlock);

            float detailW = 280.0f;
            ImGui::BeginChild("##blockGrid", ImVec2(-detailW, 0), true);
            {
                ImTextureID blkTexId = (ImTextureID)(intptr_t)blockAtlas[blockEditor.bank];
                float thumb = 40.0f;
                ImGuiStyle &beStyle = ImGui::GetStyle();
                float beWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                for (int b = 0; b < 0x100; b++)
                {
                    ImGui::PushID(b);
                    float u0 = (float)(b % ATLAS_BLOCKS_PER_ROW) / ATLAS_BLOCKS_PER_ROW;
                    float v0 = (float)(b / ATLAS_BLOCKS_PER_ROW) / ATLAS_BLOCKS_PER_ROW;
                    float u1 = u0 + 1.0f / ATLAS_BLOCKS_PER_ROW;
                    float v1 = v0 + 1.0f / ATLAS_BLOCKS_PER_ROW;

                    // Whether a style color was pushed for THIS button is
                    // captured up front and used as-is for the matching
                    // pop below -- never re-derived from swapMode/copyMode
                    // after the click handler runs, since that handler can
                    // itself flip those flags off (e.g. a successful swap
                    // disarms swapMode). Re-checking post-click would pop
                    // based on a value that changed since the push,
                    // silently unbalancing ImGui's style color stack --
                    // exactly what was crashing the swap feature before.
                    bool selected = (b == blockEditor.selectedBlock);
                    bool pushedColor = true;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                    else if (blockEditor.swapMode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.45f, 0.10f, 1.0f));
                    else if (blockEditor.copyMode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.20f, 1.0f));
                    else pushedColor = false;

                    if (ImGui::ImageButton("##blk", blkTexId, ImVec2(thumb, thumb), ImVec2(u0, v0), ImVec2(u1, v1)))
                    {
                        if (blockEditor.swapMode)
                        {
                            if (b != blockEditor.selectedBlock)
                            {
                                int a = blockEditor.selectedBlock;
                                std::swap(map_blocks[a], map_blocks[b]);
                                for (int i = 0; i < MAP_COUNT; i++)
                                {
                                    if (map_bnums[i] == a) map_bnums[i] = b;
                                    else if (map_bnums[i] == b) map_bnums[i] = a;
                                }
                                rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 1);
                                rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 2);
                                st.dirty = true;
                            }
                            blockEditor.swapMode = false;
                        }
                        else if (blockEditor.copyMode)
                        {
                            if (b != blockEditor.selectedBlock)
                            {
                                std::memcpy(map_blocks[b], map_blocks[blockEditor.selectedBlock], sizeof(map_blocks[b]));
                                rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 1);
                                rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 2);
                                st.dirty = true;
                            }
                            blockEditor.selectedBlock = b;
                            blockEditor.selectedCell = 0;
                            blockEditor.copyMode = false;
                        }
                        else
                        {
                            blockEditor.selectedBlock = b;
                            blockEditor.selectedCell = 0;
                        }
                    }
                    if (pushedColor) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                    {
                        if (blockEditor.swapMode) ImGui::SetTooltip("Swap with block %d", b);
                        else if (blockEditor.copyMode) ImGui::SetTooltip("Copy into block %d", b);
                        else ImGui::SetTooltip("Block %d", b);
                    }
                    float nextRight = ImGui::GetItemRectMax().x + beStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                    if (b != 255 && nextRight < beWindowRight)
                        ImGui::SameLine();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##blockDetail", ImVec2(detailW, 0));
            {
                if (blockEditor.selectedBlock < 0)
                {
                    ImGui::TextWrapped("Click a block on the left to select it.");
                }
                else
                {
                    ImGui::Text("Block %d (bank %d preview)", blockEditor.selectedBlock, blockEditor.bank);
                    ImGui::TextWrapped("Click a cell below, then click a tile to place it there.");
                    ImGui::Spacing();

                    // 4x4 editable grid: cell i is column i%4, row i/4
                    // (same layout as build_block_atlas()/drawblock()).
                    ImTextureID tileTexId = (ImTextureID)(intptr_t)tileAtlas[blockEditor.bank];
                    float cell = 48.0f;
                    int *cells = map_blocks[blockEditor.selectedBlock];
                    bool blockChanged = false;
                    for (int i = 0; i < 16; i++)
                    {
                        ImGui::PushID(i);
                        int t = std::clamp(cells[i], 0, 255);
                        float u0 = (float)(t % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                        float v0 = (float)(t / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                        float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                        float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;

                        bool selected = (i == blockEditor.selectedCell);
                        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                        if (ImGui::ImageButton("##cell", tileTexId, ImVec2(cell, cell), ImVec2(u0, v0), ImVec2(u1, v1)))
                            blockEditor.selectedCell = i;
                    if (selected || tileEditor.swapMode) ImGui::PopStyleColor();
                        drawTileHazardBorder(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), eflg.bank[blockEditor.bank - 1][t]);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cell %d (tile %d)", i, t);
                        if (i % 4 != 3) ImGui::SameLine();
                        ImGui::PopID();
                    }

                    ImGui::Spacing();
                    ImGui::Text("Selected cell: %d (tile %d)", blockEditor.selectedCell, std::clamp(cells[blockEditor.selectedCell], 0, 255));
                    // These 3 buttons can together exceed this panel's
                    // fixed width (280px, or narrower still if the user
                    // shrinks the window) -- SameLine() alone doesn't
                    // wrap, it just lets a button run off the edge,
                    // unclickable. Same wrap-if-needed idiom as the top
                    // toolbar: SameLine() to tentatively continue the
                    // row, then NewLine() overrides it if that pushed
                    // past the panel's right edge.
                    float detailRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                    auto wrapDetail = [&]()
                    {
                        ImGui::SameLine();
                        if (ImGui::GetCursorScreenPos().x > detailRight) ImGui::NewLine();
                    };
                    if (ImGui::SmallButton("Clear block (all tile 0)"))
                    {
                        for (int i = 0; i < 16; i++) cells[i] = 0;
                        blockChanged = true;
                    }
                    wrapDetail();
                    if (ImGui::SmallButton(blockEditor.swapMode ? "Cancel swap" : "Swap with..."))
                    {
                        blockEditor.swapMode = !blockEditor.swapMode;
                        if (blockEditor.swapMode) blockEditor.copyMode = false; // mutually exclusive armed modes
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click, then pick another block on the left to "
                                                                   "swap the two blocks' tile compositions");
                    wrapDetail();
                    if (ImGui::SmallButton(blockEditor.copyMode ? "Cancel copy" : "Copy to..."))
                    {
                        blockEditor.copyMode = !blockEditor.copyMode;
                        if (blockEditor.copyMode) blockEditor.swapMode = false;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click, then pick another block on the left to "
                                                                   "overwrite it with this block's tile composition "
                                                                   "(this block itself is left unchanged)");

                    ImGui::Separator();
                    ImGui::TextWrapped("Tile picker (bank %d) -- click to place into the selected cell:", blockEditor.bank);
                    ImGui::BeginChild("##blockTilePicker", ImVec2(0, 0), true);
                    {
                        float thumb2 = 28.0f;
                        ImGuiStyle &tpStyle = ImGui::GetStyle();
                        float tpWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                        for (int t = 0; t < 0x100; t++)
                        {
                            ImGui::PushID(t);
                            float u0 = (float)(t % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                            float v0 = (float)(t / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                            float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                            float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;

                            bool isCurrent = (t == std::clamp(cells[blockEditor.selectedCell], 0, 255));
                            if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                            if (ImGui::ImageButton("##pick", tileTexId, ImVec2(thumb2, thumb2), ImVec2(u0, v0), ImVec2(u1, v1)))
                            {
                                cells[blockEditor.selectedCell] = t;
                                blockChanged = true;
                                // Advance to the next cell so composing a
                                // block is a quick left-to-right, top-to-
                                // bottom stamping motion -- stop at the
                                // last cell instead of wrapping, so a
                                // deliberate out-of-order fixup (click an
                                // earlier cell, place one tile) doesn't
                                // get silently overridden by another
                                // auto-advance on the very next click.
                                if (blockEditor.selectedCell < 15) blockEditor.selectedCell++;
                            }
                            if (isCurrent) ImGui::PopStyleColor();
                            drawTileHazardBorder(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), eflg.bank[blockEditor.bank - 1][t]);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(tileEditor.swapMode ? "Swap with tile %d" : "Tile %d", t);
                            float nextRight = ImGui::GetItemRectMax().x + tpStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                            if (t != 255 && nextRight < tpWindowRight)
                                ImGui::SameLine();
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();

                    if (blockChanged)
                    {
                        st.dirty = true; // map_blocks is part of the .map file now (RKM7) -- flag for save
                        rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 1);
                        rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 2);
                    }
                }
            }
            ImGui::EndChild();

            ImGui::End();
        }

        // --- Sprite Editor -- standalone window, independent of canvas
        // mode. Edits sprite GRAPHICS (sprites_data, via image import,
        // single or batch), same pattern as the Tile Editor above but
        // with no bank split (one flat 213-entry table) and no hazard
        // flags (sprites don't have map_eflg_c-style per-graphic
        // flags -- their behavior comes from the marks/triggers system,
        // a different thing entirely, already covered by "Sprite Tools").
        // See src/sprite_import.h. ---
        if (spriteEditor.open)
        {
            ImGui::SetNextWindowSize(ImVec2(600, 560), ImGuiCond_FirstUseEver);
            ImGui::Begin("Sprite Editor", &spriteEditor.open);

            ImGui::TextDisabled("%d sprites, 32x21 pixels each, indices 1-15 opaque + 0 transparent",
                                 SPRITES_NBR_SPRITES);
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Batch import..."))
            {
                ImGui::Indent();
                ImGui::TextWrapped("Slices one image into consecutive %dx%d sprites, left-to-right then "
                                    "top-to-bottom, starting at the sprite number below. No resampling -- "
                                    "each sprite must be an exact %dx%d pixel cell of the image. Pixels with "
                                    "alpha below %d become transparent; opaque pixels are matched to the "
                                    "closest of the 15 non-transparent palette colors.",
                                    SPRITE_W, SPRITE_H, SPRITE_W, SPRITE_H, SPRITE_ALPHA_THRESHOLD);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Start sprite", &spriteEditor.batchStartSprite);
                spriteEditor.batchStartSprite = std::clamp(spriteEditor.batchStartSprite, 0, SPRITES_NBR_SPRITES - 1);
                if (ImGui::Button("Choose image..."))
                {
                    fileDialog.show = true;
                    fileDialog.saveMode = false;
                    fileDialog.purpose = DialogPurpose::BatchImportSpriteImage;
                    fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                    fileDialog.filename[0] = '\0';
                    fileDialog.error.clear();
                }
                ImGui::Unindent();
            }
            ImGui::Separator();

            float detailW = 210.0f;
            ImGui::BeginChild("##spriteGrid", ImVec2(-detailW, 0), true);
            {
                ImTextureID spriteTexId = (ImTextureID)(intptr_t)spriteAtlas;
                float thumb = 32.0f, thumbH = thumb * SPRITE_H / SPRITE_W;
                ImGuiStyle &seStyle = ImGui::GetStyle();
                float seWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                for (int s = 0; s < SPRITES_NBR_SPRITES; s++)
                {
                    ImGui::PushID(s);
                    float u0, v0, u1, v1;
                    sprite_uv(s, u0, v0, u1, v1);

                    bool selected = (s == spriteEditor.selectedSprite);
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                    if (ImGui::ImageButton("##spr", spriteTexId, ImVec2(thumb, thumbH), ImVec2(u0, v0), ImVec2(u1, v1)))
                    {
                        spriteEditor.selectedSprite = s;
                        spriteEditor.batchStartSprite = s; // still freely editable in the Batch import field above
                    }
                    if (selected) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sprite %d", s);
                    float nextRight = ImGui::GetItemRectMax().x + seStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                    if (s != SPRITES_NBR_SPRITES - 1 && nextRight < seWindowRight)
                        ImGui::SameLine();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##spriteDetail", ImVec2(detailW, 0));
            {
                if (spriteEditor.selectedSprite < 0)
                {
                    ImGui::TextWrapped("Click a sprite on the left to select it.");
                }
                else
                {
                    ImGui::Text("Sprite %d", spriteEditor.selectedSprite);
                    ImGui::Spacing();

                    ImTextureID spriteTexId = (ImTextureID)(intptr_t)spriteAtlas;
                    float u0, v0, u1, v1;
                    sprite_uv(spriteEditor.selectedSprite, u0, v0, u1, v1);
                    float bigW = 128.0f, bigH = bigW * SPRITE_H / SPRITE_W;
                    // Checkerboard behind the preview so transparency is
                    // visible instead of blending into the window
                    // background (which is the same dark gray as index 0
                    // would otherwise look like, for tiles -- but sprites
                    // have real alpha, so a plain background can't show it).
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImVec2 p1 = ImVec2(p0.x + bigW, p0.y + bigH);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    const float cb = 8.0f;
                    for (float y = p0.y; y < p1.y; y += cb)
                        for (float x = p0.x; x < p1.x; x += cb)
                        {
                            bool dark = (((int)((x - p0.x) / cb) + (int)((y - p0.y) / cb)) % 2) == 0;
                            dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cb, p1.x), std::min(y + cb, p1.y)),
                                               dark ? IM_COL32(60, 60, 60, 255) : IM_COL32(90, 90, 90, 255));
                        }
                    ImGui::Image(spriteTexId, ImVec2(bigW, bigH), ImVec2(u0, v0), ImVec2(u1, v1));

                    ImGui::Spacing();
                    if (ImGui::Button("Import from image...", ImVec2(-1, 0)))
                    {
                        fileDialog.show = true;
                        fileDialog.saveMode = false;
                        fileDialog.purpose = DialogPurpose::ImportSpriteImage;
                        fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                        fileDialog.filename[0] = '\0';
                        fileDialog.error.clear();
                    }
                    if (ImGui::SmallButton("Edit pixels"))
                    {
                        pixelEditor.open = true;
                        pixelEditor.target = PixelEditorState::Sprite;
                        pixelEditor.index = spriteEditor.selectedSprite;
                        pixelEditor.color = 1;
                        std::memcpy(pixelEditor.backupSprite, sprites_data[spriteEditor.selectedSprite], sizeof(sprite_t));
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open pixel editor for this sprite (32x21, 16 colors)");
                    ImGui::TextWrapped("Any image size works -- it's resampled to %dx%d. Alpha below %d "
                                        "becomes transparent; opaque pixels are matched to the closest of "
                                        "the 15 non-transparent palette colors.", SPRITE_W, SPRITE_H, SPRITE_ALPHA_THRESHOLD);
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            if (ImGui::Button("Export all sprites as PNG..."))
            {
                fileDialog.show = true;
                fileDialog.saveMode = true;
                fileDialog.purpose = DialogPurpose::ExportSpritePNG;
                fileDialog.extFilter = {".png"};
                std::snprintf(fileDialog.filename, sizeof fileDialog.filename, "sprites.png");
                fileDialog.error.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export all %d sprites as a PNG sprite sheet (%dx%d)",
                                                            SPRITES_NBR_SPRITES, SPRITE_ATLAS_W, SPRITE_ATLAS_H);

            ImGui::End();
        }

        // --- Assets Editor -- standalone window for editing screen
        // graphics beyond tiles/sprites/blocks: bank 0 tile grid,
        // logo tile streams, ASCII text screens, and bitmap pictures. ---
        if (assetsEditor.open)
        {
            ImGui::SetNextWindowSize(ImVec2(640, 600), ImGuiCond_FirstUseEver);
            ImGui::Begin("Assets Editor", &assetsEditor.open);

            ImGui::TextDisabled("Screen graphics: bank 0 tiles, logo titles, text screens, and bitmap pictures");
            ImGui::Separator();

            // --- Bank 0 tile grid ---
            if (ImGui::CollapsingHeader("Bank 0 Tiles (font / cutscene decor)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Indent();
                ImGui::TextDisabled("256 tiles, 8x8 pixels each. Bank 0 is the font used by all text screens "
                                     "and the cutscene decor backgrounds.");
                if (ImGui::CollapsingHeader("Batch import..."))
                {
                    ImGui::Indent();
                    ImGui::TextWrapped("Slices one image into consecutive 8x8 tiles, left-to-right then "
                                        "top-to-bottom, starting at the tile number below.");
                    ImGui::SetNextItemWidth(100);
                    ImGui::InputInt("Start tile", &assetsEditor.batchStartTile);
                    assetsEditor.batchStartTile = std::clamp(assetsEditor.batchStartTile, 0, 255);
                    if (ImGui::Button("Choose image..."))
                    {
                        fileDialog.show = true;
                        fileDialog.saveMode = false;
                        fileDialog.purpose = DialogPurpose::BatchImportBank0Image;
                        fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                        fileDialog.filename[0] = '\0';
                        fileDialog.error.clear();
                    }
                    ImGui::Unindent();
                }
                // Tile grid + detail
                float detailW = 160.0f;
                ImGui::BeginChild("##bank0Grid", ImVec2(-detailW, 180), true);
                {
                    ImTextureID tileTexId = (ImTextureID)(intptr_t)tileAtlas[0];
                    float thumb = 20.0f;
                    ImGuiStyle &b0Style = ImGui::GetStyle();
                    float b0WindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                    for (int t = 0; t < 0x100; t++)
                    {
                        ImGui::PushID(5000 + t);
                        float u0 = (float)(t % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                        float v0 = (float)(t / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                        float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                        float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;
                        bool selected = (t == assetsEditor.selectedTile);
                        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                        if (ImGui::ImageButton("##b0t", tileTexId, ImVec2(thumb, thumb), ImVec2(u0, v0), ImVec2(u1, v1)))
                        {
                            assetsEditor.selectedTile = t;
                            assetsEditor.batchStartTile = t;
                        }
                        if (selected) ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile %d (0x%02X)", t, t);
                        float nextRight = ImGui::GetItemRectMax().x + b0Style.ItemSpacing.x + ImGui::GetItemRectSize().x;
                        if (t != 0xFF && nextRight < b0WindowRight) ImGui::SameLine();
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("##bank0Detail", ImVec2(detailW, 180));
                {
                    if (assetsEditor.selectedTile < 0)
                        ImGui::TextWrapped("Click a tile to select.");
                    else
                    {
                        ImGui::Text("Tile %d", assetsEditor.selectedTile);
                        ImTextureID tileTexId = (ImTextureID)(intptr_t)tileAtlas[0];
                        float u0 = (float)(assetsEditor.selectedTile % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                        float v0 = (float)(assetsEditor.selectedTile / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                        float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                        float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;
                        ImGui::Image(tileTexId, ImVec2(96, 96), ImVec2(u0, v0), ImVec2(u1, v1));
                        if (ImGui::SmallButton("Import..."))
                        {
                            fileDialog.show = true; fileDialog.saveMode = false;
                            fileDialog.purpose = DialogPurpose::ImportBank0Image;
                            fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                            fileDialog.filename[0] = '\0'; fileDialog.error.clear();
                        }
                        if (ImGui::SmallButton("Edit pixels"))
                        {
                            pixelEditor.open = true; pixelEditor.target = PixelEditorState::Tile;
                            pixelEditor.bank = 0; pixelEditor.index = assetsEditor.selectedTile;
                            pixelEditor.color = 1;
                            std::memcpy(pixelEditor.backupTile, tiles_data[0][assetsEditor.selectedTile], sizeof(tile_t));
                        }
                        if (ImGui::SmallButton("Delete"))
                        {
                            for (int i = 0; i < 8; i++) tiles_data[0][assetsEditor.selectedTile][i] = 0;
                            rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 0);
                            st.dirty = true;
                        }
                    }
                }
                ImGui::EndChild();
                if (ImGui::Button("Export bank 0 tiles as PNG..."))
                {
                    fileDialog.show = true; fileDialog.saveMode = true;
                    fileDialog.purpose = DialogPurpose::ExportTileBankPNG;
                    fileDialog.extFilter = {".png"};
                    std::snprintf(fileDialog.filename, sizeof fileDialog.filename, "tiles_bank0.png");
                    fileDialog.error.clear();
                }
                ImGui::Unindent();
            }

            // --- ASCII Text Screens ---
            if (ImGui::CollapsingHeader("Text Screens (copyright, game over, pause)"))
            {
                ImGui::Indent();
                ImGui::TextWrapped("Same encoding as the intro texts: '@' = space, 0xFF = newline, "
                                    "0xFE = end. Each screen is a fixed-size slot in the binary.");
                for (int i = 0; i < 3; i++)
                {
                    ImGui::PushID(7000 + i);
                    AsciiTextScreen &ats = assetsEditor.asciiScreens[i];
                    if (ImGui::TreeNode(ASCII_TEXT_SCREEN_LABELS[i]))
                    {
                        char buf[512];
                        size_t n = std::min(ats.text.size(), sizeof(buf) - 1);
                        std::memcpy(buf, ats.text.data(), n);
                        buf[n] = '\0';
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputTextMultiline("##txt", buf, sizeof(buf), ImVec2(0, 80)))
                            ats.text = buf;
                        ImGui::TextDisabled("Slot size: %d bytes", (int)ASCII_TEXT_SCREEN_SIZES[i]);
                        if (ImGui::SmallButton("Reset to default"))
                        {
                            if (i == 0) ats = defaultImainCDC();
                            else if (i == 1) ats = defaultGameoverTxt();
                            else ats = defaultPausedTxt();
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::Unindent();
            }

            // --- Bitmap Pictures (pic_congrats, pic_haf, pic_splash) ---
            if (ImGui::CollapsingHeader("Bitmap Pictures (title screen, backgrounds)"))
            {
                ImGui::Indent();
                ImGui::TextWrapped("4-bit-per-pixel indexed images using the fixed 16-color ST palette. "
                                    "Loaded from the xrick binary -- use \"Import from xrick binary\" "
                                    "in the File menu to load them.");
                for (int i = 0; i < PIC_COUNT; i++)
                {
                    ImGui::PushID(8000 + i);
                    const BitmapPic &pic = assetsEditor.pics.pics[i];
                    bool loaded = assetsEditor.pics.loaded[i];
                    if (ImGui::TreeNode(PIC_LABELS[i]))
                    {
                        if (!loaded)
                        {
                            ImGui::TextDisabled("Not loaded from xrick binary.");
                            ImGui::Text("Dimensions: %dx%d", PIC_W[i], PIC_H[i]);
                            if (ImGui::Button("Import from image..."))
                            {
                                assetsEditor.importPicIdx = i;
                                fileDialog.show = true; fileDialog.saveMode = false;
                                fileDialog.purpose = DialogPurpose::ImportPicImage;
                                fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                                fileDialog.filename[0] = '\0';
                                fileDialog.error.clear();
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import any image -- resampled to %dx%d", PIC_W[i], PIC_H[i]);
                        }
                        else
                        {
                            ImGui::Text("%dx%d pixels", pic.w, pic.h);
                            // Preview: create a temporary SDL texture
                            static SDL_Texture *picPreviewTex[3] = {};
                            static int picPreviewW[3] = {}, picPreviewH[3] = {};
                            if (!picPreviewTex[i] || picPreviewW[i] != pic.w || picPreviewH[i] != pic.h)
                            {
                                if (picPreviewTex[i]) SDL_DestroyTexture(picPreviewTex[i]);
                                picPreviewTex[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                                     SDL_TEXTUREACCESS_STATIC, pic.w, pic.h);
                                SDL_SetTextureBlendMode(picPreviewTex[i], SDL_BLENDMODE_NONE);
                                SDL_UpdateTexture(picPreviewTex[i], nullptr, pic.pixels.data(), pic.w * sizeof(Uint32));
                                picPreviewW[i] = pic.w; picPreviewH[i] = pic.h;
                            }
                            float scale = std::min(400.0f / (float)pic.w, 1.0f);
                            float dw = pic.w * scale, dh = pic.h * scale;
                            ImGui::Image((ImTextureID)(intptr_t)picPreviewTex[i], ImVec2(dw, dh));
                            if (ImGui::Button("Import from image..."))
                            {
                                assetsEditor.importPicIdx = i;
                                fileDialog.show = true; fileDialog.saveMode = false;
                                fileDialog.purpose = DialogPurpose::ImportPicImage;
                                fileDialog.extFilter = {".png", ".bmp", ".tga", ".jpg", ".jpeg", ".gif", ".psd"};
                                fileDialog.filename[0] = '\0';
                                fileDialog.error.clear();
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import any image -- resampled to %dx%d", pic.w, pic.h);
                            if (ImGui::Button("Export as PNG..."))
                            {
                                assetsEditor.exportPicIdx = i;
                                fileDialog.show = true; fileDialog.saveMode = true;
                                fileDialog.purpose = DialogPurpose::ExportPicPNG;
                                fileDialog.extFilter = {".png"};
                                std::snprintf(fileDialog.filename, sizeof fileDialog.filename, "%s.png",
                                              PIC_SYMBOLS[i]);
                                fileDialog.error.clear();
                            }
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::Unindent();
            }

            ImGui::End();
        }

        // --- Text Editor -- standalone window, independent of canvas
        // mode. Edits the 5 between-levels intro texts (mapTexts, in
        // memory only -- see screens_text.h/dat_screens.c). Not tied to
        // any bank/atlas rebuild since it doesn't touch tiles_data --
        // characters ARE tile indices into bank 0 (the "unused padding"
        // bank everywhere else in this editor is in fact the font), so
        // the live preview below reads tileAtlas[0] directly and needs
        // no atlas rebuild of its own. ---
        if (textEditor.open)
        {
            ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
            ImGui::Begin("Text Editor", &textEditor.open);
            ImGui::TextDisabled("Between-levels intro text -- each character is a tile from bank 0 (the font). "
                                 "Max 30 characters/line (screen width); type real spaces, they're stored as "
                                 "'@' internally same as the original data.");
            ImGui::Separator();

            float listW = 220.0f;
            ImGui::BeginChild("##textList", ImVec2(listW, 0), true);
            for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
            {
                if (ImGui::Selectable(SCREEN_IMAPTEXT_LABELS[i], textEditor.selected == i))
                    textEditor.selected = i;
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##textDetail", ImVec2(0, 0));
            {
                ImapText &txt = mapTexts[textEditor.selected];

                if (ImGui::SmallButton("Reset to stock"))
                {
                    txt = parseImapText(screen_imaptext[textEditor.selected]);
                    st.dirty = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Discards edits, reloads the original game text for this entry");
                ImGui::SameLine();
                if (ImGui::SmallButton("Add line") && txt.rows.size() < 40)
                {
                    txt.rows.push_back(ImapTextRow{});
                    st.dirty = true;
                }

                ImGui::Separator();
                ImGui::Text("Lines:");

                int removeRow = -1, moveUp = -1, moveDown = -1;
                for (int i = 0; i < (int)txt.rows.size(); i++)
                {
                    ImGui::PushID(i);
                    ImapTextRow &row = txt.rows[i];

                    char buf[32];
                    size_t n = std::min(row.text.size(), (size_t)30);
                    std::memcpy(buf, row.text.data(), n);
                    buf[n] = '\0';
                    ImGui::SetNextItemWidth(220);
                    if (ImGui::InputText("##line", buf, sizeof(buf)))
                    {
                        row.text = buf;
                        st.dirty = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%2d/30", (int)row.text.size());
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Blank line after", &row.blankLineAfter))
                        st.dirty = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds an extra empty row of vertical spacing "
                                                                   "after this line (visual only)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Up") && i > 0) moveUp = i;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Down") && i < (int)txt.rows.size() - 1) moveDown = i;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X") && txt.rows.size() > 1) removeRow = i;

                    ImGui::PopID();
                }
                if (removeRow >= 0) { txt.rows.erase(txt.rows.begin() + removeRow); st.dirty = true; }
                if (moveUp >= 0) { std::swap(txt.rows[moveUp], txt.rows[moveUp - 1]); st.dirty = true; }
                if (moveDown >= 0) { std::swap(txt.rows[moveDown], txt.rows[moveDown + 1]); st.dirty = true; }

                ImGui::Separator();
                ImGui::Text("Preview (bank 0 font tiles, as rendered in-game):");
                ImGui::BeginChild("##textPreview", ImVec2(0, 0), true);
                {
                    ImTextureID fontTexId = (ImTextureID)(intptr_t)tileAtlas[0];
                    float glyph = 16.0f; // 8px tile x2 for visibility
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImVec2 origin = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(origin, ImVec2(origin.x + 30 * glyph, origin.y + (float)txt.rows.size() * glyph
                                                      + (float)std::count_if(txt.rows.begin(), txt.rows.end(),
                                                            [](const ImapTextRow &r){ return r.blankLineAfter; }) * glyph),
                                       IM_COL32(20, 20, 30, 255));
                    for (const ImapTextRow &row : txt.rows)
                    {
                        for (char ch : row.text)
                        {
                            uint8_t tileIdx = (uint8_t)(ch == ' ' ? '@' : ch);
                            float u0 = (float)(tileIdx % ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                            float v0 = (float)(tileIdx / ATLAS_TILES_PER_ROW) / ATLAS_TILES_PER_ROW;
                            float u1 = u0 + 1.0f / ATLAS_TILES_PER_ROW;
                            float v1 = v0 + 1.0f / ATLAS_TILES_PER_ROW;
                            ImGui::Image(fontTexId, ImVec2(glyph, glyph), ImVec2(u0, v0), ImVec2(u1, v1));
                            ImGui::SameLine(0, 0);
                        }
                        ImGui::NewLine();
                        if (row.blankLineAfter)
                            ImGui::Dummy(ImVec2(1, glyph));
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();

            ImGui::End();
        }

        // --- Screen Connections (links between submaps) -- only relevant (and shown) in Submap mode ---
        if (st.canvasMode == CanvasMode::Submap)
        {
        ImGui::SetNextWindowPos(toolPanelPos);
        ImGui::SetNextWindowSize(toolPanelSize);
        ImGui::Begin("Screen Connections", nullptr, toolPanelFlags);
        {
            int totalSlots = 0;
            for (int i = 0; i < MAP_NBR_SUBMAPS; i++) totalSlots += (int)connections.exits[i].size() + 1;
            bool full = totalSlots >= MAP_NBR_CONNECT;
            ImGui::TextColored(full ? ImVec4(1.0f, 0.6f, 0.3f, 1.0f) : ImVec4(0.7f, 0.9f, 0.7f, 1.0f),
                                "%d / %d exit slots used", totalSlots, MAP_NBR_CONNECT);
            ImGui::TextWrapped("Rows are absolute TILE rows -- four times finer than the main map's "
                                "block grid (\"Cell under cursor\" in Tools), so e.g. row 60 here lines "
                                "up with block row 15 there. Confirmed from the original xrick source. "
                                "A link is one row on this submap connected to one row on the target "
                                "(direction just says which way through it counts as a trigger).");
            ImGui::Separator();

            // --- Per-map start positions (xrick's map_maps) ---
            if (ImGui::CollapsingHeader("Map Start Positions"))
            {
                ImGui::Indent();
                ImGui::TextWrapped("Each of the 4 maps (levels) has a start position that controls "
                                   "where Rick appears initially: pixel coordinates (x, y), the "
                                   "first displayed tile-row in game, and the starting submap "
                                   "index. Rick's actual screen row depends on both the scroll "
                                   "position and his pixel Y offset. These values come from "
                                   "xrick's map_maps[] array (dat_maps.c).");
                ImGui::Separator();
                static const char *mapNames[] = {"Map 1", "Map 2", "Map 3", "Map 4"};
                for (int m = 0; m < MAP_NBR_MAPS; m++)
                {
                    ImGui::PushID(2000 + m);
                    MapStartInfo &ms = connections.mapStarts[m];
                    ImGui::SetNextItemWidth(120);
                    ImGui::DragInt("X", &ms.x, 1.0f, 0, 32767);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixel X position where Rick spawns");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120);
                    ImGui::DragInt("Y", &ms.y, 1.0f, 0, 32767);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pixel Y position where Rick spawns");
                    ImGui::SameLine();
                    ImGui::TextDisabled("  %s", mapNames[m]);

                    int sm = ms.submap;
                    int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(connections.submaps[sm]) : 0;
                    int rawRow = ms.row - base;
                    int firstVisible = ms.row + 8;
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::DragInt("Row", &firstVisible, 1.0f, 8, MAP_TILE_ROWS - 1))
                        ms.row = firstVisible - 8;
                    if (ImGui::IsItemHovered())
                    {
                        int heroFeetRow = firstVisible + (ms.y + entDataTable[1].h) / 8 - 8;
                        ImGui::SetTooltip("First tile-row displayed in game.\n"
                                          "Rick's feet appear at tile row %d.\n"
                                          "Raw map_frow: %d.", heroFeetRow, rawRow);
                    }
                    if (rawRow % 4 != 0)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "!");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Raw map_frow %d is not a multiple of 4!\n"
                                              "map_expand() will load misaligned tiles,\n"
                                              "causing sprite/tile desync in-game.", rawRow);
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120);
                    ImGui::DragInt("Submap", &ms.submap, 1.0f, 0, MAP_NBR_SUBMAPS - 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starting submap index (0-%d)", MAP_NBR_SUBMAPS - 1);
                    bool placing = (st.placeStartMode == m);
                    if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                    if (ImGui::SmallButton("Place on map"))
                        st.placeStartMode = placing ? -1 : m;
                    if (placing) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(placing ? "Click on the map to place %s start (Esc to cancel)"
                                                   : "Click, then click on the map to place %s start position", mapNames[m]);

                    ImGui::PopID();
                    if (m < MAP_NBR_MAPS - 1) ImGui::Separator();
                }
                ImGui::Unindent();
            }
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Diagnostics"))
            {
                ImGui::Indent();
                ImGui::TextWrapped("Checks for a known stock-data quirk: a submap whose block data starts "
                                    "mid-row instead of at a clean block boundary (confirmed harmless "
                                    "in-game -- the real engine reads blocks as a flat, row-agnostic "
                                    "sliding window -- but it means this editor's own block-grid rendering "
                                    "doesn't line up with where that submap's content actually starts, "
                                    "which can look like everything past it is shifted). See EDITEUR.md.");
                if (ImGui::SmallButton("Check for / fix misaligned block run"))
                {
                    auto fix = fixMisalignedBlockRun(connections);
                    patchResultMessage = fix.message;
                    patchResultOk = fix.applied;
                    st.dirty = st.dirty || fix.applied;
                    ImGui::OpenPopup("Result");
                }
                ImGui::Unindent();
            }
            ImGui::Separator();

            int submapToDelete = -1;
            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
            {
                ImGui::PushID(s);
                char header[96];
                // "###submapN" pins the ID to the submap index so deleting
                // a link (which changes the count in the label) doesn't
                // reset this header to closed -- same fix as Sprite Tools.
                std::snprintf(header, sizeof header, "Submap %d -- starts at row %d / block %d (%d links)###submap%d",
                              s, submapStartRow(connections.submaps[s]), submapStartRow(connections.submaps[s]) / 4,
                              (int)connections.exits[s].size(), s);
                if (ImGui::CollapsingHeader(header))
                {
                    ImGui::Indent();

                    // Start row: editable directly (this IS the real
                    // stored value, `bnum`, just shown/edited in tile-row
                    // units). There is deliberately no matching "end row"
                    // field: the real format has none -- a submap has no
                    // stored height at all, it's an open-ended scrolling
                    // window into the shared block canvas (maps.c slides
                    // `map_frow` indefinitely as Rick moves; the only
                    // thing that actually stops a submap is one of its own
                    // Screen Connections exits sending Rick elsewhere).
                    // Existing marks/exits stay anchored to their own
                    // absolute row when this changes (they don't move),
                    // and repackMarks()/repackConnections() already catch
                    // -- with a clear error -- any of them ending up too
                    // far from the new start to still fit the format.
                    int startRow = submapStartRow(connections.submaps[s]);
                    ImGui::SetNextItemWidth(70);
                    if (ImGui::DragInt("Start row", &startRow, 1.0f, 0, MAP_TILE_ROWS - 1))
                        connections.submaps[s].bnum = startRow * 2;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Where this submap starts reading the shared block canvas "
                                           "from. Moving this does NOT move its blocks (paint those "
                                           "separately in Block mode) -- it just changes which part of "
                                           "the canvas this submap's window points at. There's no "
                                           "separate \"end\" to set: a submap has no stored height in "
                                           "this format, see the note below.");

                    // Purely observational (not stored anywhere): the
                    // lowest/highest row this submap's own marks and
                    // exits actually use right now, as a practical sense
                    // of how far it's been built out.
                    {
                        int lo = INT32_MAX, hi = INT32_MIN;
                        for (auto &m : sprites.marks[s]) { lo = std::min(lo, m.rowAbs); hi = std::max(hi, m.rowAbs); }
                        for (auto &c : connections.exits[s]) { lo = std::min(lo, c.rowAbs); hi = std::max(hi, c.rowAbs); }
                        if (lo <= hi)
                            ImGui::TextDisabled("Its own sprites/exits currently span rows %d-%d (%d rows) -- "
                                                 "not a stored value, just what's placed so far.", lo, hi, hi - lo + 1);
                        else
                            ImGui::TextDisabled("No sprites or exits placed in it yet.");
                    }

                    int bank = connections.submaps[s].page; // 0 = tile bank 1, 1 = tile bank 2
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::Combo("Tile bank", &bank, "1\0002\0\0")) connections.submaps[s].page = bank;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete submap"))
                        ImGui::OpenPopup("confirm_delete_submap");
                    if (ImGui::BeginPopup("confirm_delete_submap"))
                    {
                        ImGui::Text("This format has no way to shrink the fixed 47-submap array, so\n"
                                     "\"delete\" means: clear submap %d's own links and disconnect any\n"
                                     "other submap's link that pointed to it, making it unreachable.\n"
                                     "Its tiles/sprites stay in the data, just cut off from the level.", s);
                        if (ImGui::Button("Confirm")) { submapToDelete = s; ImGui::CloseCurrentPopup(); }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }

                    auto &exits = connections.exits[s];
                    int removeIdx = -1;
                    for (int e = 0; e < (int)exits.size(); e++)
                    {
                        ImGui::PushID(e);
                        ConnectEntry &c = exits[e];

                        int base = submapStartRow(connections.submaps[s]);
                        bool outOfRange = (c.rowAbs - base < 0 || c.rowAbs - base > 255);

                        const char* dirLabels[] = { "Down", "Up" };
                        ImGui::SetNextItemWidth(70);
                        ImGui::Combo("##dir", &c.dir, dirLabels, 2);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("row");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55);
                        if (outOfRange) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
                        ImGui::DragInt("##rowabs", &c.rowAbs, 1.0f, 0, MAP_TILE_ROWS - 1);
                        if (ImGui::IsItemHovered() && !outOfRange) ImGui::SetTooltip("Block row ~%d", c.rowAbs / 4);
                        if (outOfRange) { ImGui::PopStyleColor(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Too far from this submap's own rows to fit the original format"); }
                        ImGui::SameLine();
                        ImGui::TextUnformatted("->");
                        ImGui::SameLine();
                        int target = c.targetSubmap == SUBMAP_END_OF_LEVEL ? MAP_NBR_SUBMAPS : c.targetSubmap;
                        ImGui::SetNextItemWidth(60);
                        if (ImGui::DragInt("##target", &target, 1.0f, 0, MAP_NBR_SUBMAPS))
                            c.targetSubmap = std::clamp(target, 0, MAP_NBR_SUBMAPS);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d = end of this world", MAP_NBR_SUBMAPS);
                        if (c.targetSubmap >= MAP_NBR_SUBMAPS) c.targetSubmap = SUBMAP_END_OF_LEVEL;

                        if (c.targetSubmap != SUBMAP_END_OF_LEVEL)
                        {
                            ImGui::SameLine();
                            ImGui::TextUnformatted("row");
                            ImGui::SameLine();
                            int tbase = submapStartRow(connections.submaps[c.targetSubmap]);
                            bool targetOutOfRange = (c.targetRowAbs - tbase < 0 || c.targetRowAbs - tbase > 255);
                            ImGui::SetNextItemWidth(55);
                            if (targetOutOfRange) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
                            ImGui::DragInt("##targetrowabs", &c.targetRowAbs, 1.0f, 0, MAP_TILE_ROWS - 1);
                            if (ImGui::IsItemHovered() && !targetOutOfRange) ImGui::SetTooltip("Block row ~%d", c.targetRowAbs / 4);
                            if (targetOutOfRange) { ImGui::PopStyleColor(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Too far from submap %d's own rows to fit the original format", c.targetSubmap); }
                        }
                        else
                        {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(end of world)");
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X")) removeIdx = e;
                        ImGui::PopID();
                    }
                    if (removeIdx >= 0) exits.erase(exits.begin() + removeIdx);

                    if (ImGui::SmallButton(full ? "+ Add link (table full)" : "+ Add link"))
                    {
                        if (!full)
                        {
                            int base = submapStartRow(connections.submaps[s]);
                            exits.push_back(ConnectEntry{0, base, SUBMAP_END_OF_LEVEL, 0});
                        }
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            if (submapToDelete >= 0)
            {
                int redirected = disconnectSubmap(connections, submapToDelete);
                patchResultMessage = "Submap " + std::to_string(submapToDelete) + " disconnected"
                                    + (redirected > 0 ? " (" + std::to_string(redirected) + " incoming link(s) redirected to end-of-world)." : ".");
                patchResultOk = true;
                ImGui::OpenPopup("Result");
            }
        }
        ImGui::End();
        }

        // --- Sprite Tools -- only relevant (and shown) in Sprite mode ---
        if (st.canvasMode == CanvasMode::Sprite)
        {
        ImGui::SetNextWindowPos(toolPanelPos);
        ImGui::SetNextWindowSize(toolPanelSize);
        ImGui::Begin("Sprite Tools", nullptr, toolPanelFlags);
        ImGui::Checkbox("Show sprites on map", &st.showSprites);
        ImGui::TextWrapped("Left click: place the selected entity. Right click: remove the nearest sprite.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(100);
        ImGui::DragInt("Entity type", &st.selectedEnt, 1.0f, 0, 255);
        {
            // Quick-pick chips for all entity types that have a valid sprite.
            // Some groups of related traps are pulled out of numeric order
            // and shown side by side so they're easy to compare:
            //   dogs (53 slow walker, 66 fast walker),
            //     grouped together right after ent 38, with fire trap 64
            //     following the group
            //   grille traps (45, 49, 52)
            //   moving stones / falling rocks (24, 28, 73, 31, 33, 36, 38)
            //   arrow trap 32 sits right after ent 26
            //   fireball ball (42) and egyptian treasure (34, 35) always go last
            const int dogEnts[] = {53, 66};
            const int afterDogsEnts[] = {64};
            const int grilleEnts[] = {45, 49, 52};
            const int stoneEnts[] = {24, 28, 73, 31, 33, 36, 38};
            const int lastEnts[] = {42, 34, 35};
            const int after23Ents[] = {47};  // sound trigger sits next to ent 23
            const int after26Ents[] = {32};  // vertical arrow trap sits after ent 26
            std::vector<int> all;
            for (int e = 0; e < (int)entDataTable.size(); e++)
            {
                bool grouped = false;
                for (int g : dogEnts) if (g == e) grouped = true;
                for (int g : afterDogsEnts) if (g == e) grouped = true;
                for (int g : grilleEnts) if (g == e) grouped = true;
                for (int g : stoneEnts) if (g == e) grouped = true;
                for (int g : lastEnts) if (g == e) grouped = true;
                for (int g : after23Ents) if (g == e) grouped = true;
                for (int g : after26Ents) if (g == e) grouped = true;
                if (grouped) continue;
                if (entDataTable[e].h > 0 && entDataTable[e].spr < SPRITES_NBR_SPRITES)
                    all.push_back(e);
            }
            // Insert ent 47 (sound trigger) right after ent 23.
            auto it23 = std::find(all.begin(), all.end(), 23);
            auto pos47 = it23 != all.end() ? it23 + 1 : all.end();
            for (int g : after23Ents)
                if (entDataTable[g].h > 0 && entDataTable[g].spr < SPRITES_NBR_SPRITES)
                    all.insert(pos47, g);
            // Insert ent 32 (vertical arrow trap) right after ent 26.
            auto it26 = std::find(all.begin(), all.end(), 26);
            auto pos32 = it26 != all.end() ? it26 + 1 : all.end();
            for (int g : after26Ents)
                if (entDataTable[g].h > 0 && entDataTable[g].spr < SPRITES_NBR_SPRITES)
                    all.insert(pos32, g);
            // Insert the dog group (and fire trap 64 after it) right after ent 38.
            auto it38 = std::find(all.begin(), all.end(), 38);
            auto posDogs = it38 != all.end() ? it38 + 1 : all.end();
            std::vector<int> dogBlock;
            for (int d : dogEnts)
                if (entDataTable[d].h > 0 && entDataTable[d].spr < SPRITES_NBR_SPRITES)
                    dogBlock.push_back(d);
            for (int d : afterDogsEnts)
                if (entDataTable[d].h > 0 && entDataTable[d].spr < SPRITES_NBR_SPRITES)
                    dogBlock.push_back(d);
            all.insert(posDogs, dogBlock.begin(), dogBlock.end());
            for (int g : grilleEnts)
                if (entDataTable[g].h > 0 && entDataTable[g].spr < SPRITES_NBR_SPRITES)
                    all.push_back(g);
            for (int se : stoneEnts)
                if (entDataTable[se].h > 0 && entDataTable[se].spr < SPRITES_NBR_SPRITES)
                    all.push_back(se);
            for (int le : lastEnts)
                if (entDataTable[le].h > 0 && entDataTable[le].spr < SPRITES_NBR_SPRITES)
                    all.push_back(le);
            // Mark which ones are already used on the map.
            std::set<int> used;
            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
                for (auto &m : sprites.marks[s])
                    used.insert(m.ent);
            ImGui::TextDisabled("Entity types:");
            ImTextureID spriteTexIdQP = (ImTextureID)(intptr_t)spriteAtlas;
            ImGuiStyle &qpStyle = ImGui::GetStyle();
            float qpWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            for (size_t qi = 0; qi < all.size(); qi++)
            {
                int e = all[qi];
                ImGui::PushID(e);
                int spr = entDataTable[e].spr;
                bool selected = (e == st.selectedEnt);
                bool isUsed = used.count(e) > 0;
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                else if (isUsed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                float u0, v0, u1, v1;
                sprite_uv(spr, u0, v0, u1, v1);
                if (ImGui::ImageButton("##qp", spriteTexIdQP, ImVec2(24, 21), ImVec2(u0, v0), ImVec2(u1, v1)))
                    st.selectedEnt = e;
                if (selected || isUsed) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Entity %d%s", e, isUsed ? " (on map)" : "");
                float lastItemRight = ImGui::GetItemRectMax().x;
                float nextItemRight = lastItemRight + qpStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                if (qi + 1 < all.size() && nextItemRight < qpWindowRight)
                    ImGui::SameLine();
                ImGui::PopID();
            }
            // Entity info blurb for the currently selected type.
            const char *info = entInfoText(st.selectedEnt);
            if (info && info[0])
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.18f, 0.24f, 1.0f));
                ImGui::BeginChild("##entInfo", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 4 + ImGui::GetStyle().WindowPadding.y * 2), true);
                ImGui::TextDisabled("Type %d:", st.selectedEnt);
                ImGui::TextWrapped("%s", info);
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
        }
        ImGui::Separator();

        int totalMarkSlots = 0;
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++) totalMarkSlots += (int)sprites.marks[i].size() + 1;
        bool marksFull = totalMarkSlots >= MAP_NBR_MARKS;
        ImGui::TextColored(marksFull ? ImVec4(1.0f, 0.6f, 0.3f, 1.0f) : ImVec4(0.7f, 0.9f, 0.7f, 1.0f),
                            "%d / %d sprite slots used", totalMarkSlots, MAP_NBR_MARKS);
        ImGui::TextWrapped("Rows are absolute TILE rows, like Screen Connections (four times finer "
                            "than the main map's block grid). Column is a tile position "
                            "(0-31) shared across the whole map width.");
        ImGui::Separator();

        for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
        {
            if (sprites.marks[s].empty()) continue;
            ImGui::PushID(1000 + s);
            // The real engine can only have 3 "walker/climber" entities
            // (type 1a/1b/2, ent id 4-15) alive at once -- ent_creat2()
            // in ents.c only has slots 9-11 (3 slots) for them, shared
            // across ALL of 1a/1b/2 together. A 4th one nearby silently
            // fails to spawn (no error, it just never appears) instead
            // of, say, queueing or replacing one. This is a real engine
            // limit, not an editor bug -- flagged here since it's easy
            // to not notice until testing in-game.
            int walkerClimberCount = 0;
            for (auto &m : sprites.marks[s])
                if (m.ent >= 4 && m.ent <= 15) walkerClimberCount++;
            bool tooManyWalkers = walkerClimberCount > 3;
            char header[128];
            // The label text includes the sprite count, which changes on
            // deletion -- if that text were also the widget's ID (ImGui's
            // default), deleting a sprite would change the ID and reset
            // the header to closed. The "###submapN" suffix pins the ID
            // to the submap index alone, so the open/closed state survives
            // edits to the visible count.
            if (tooManyWalkers)
                std::snprintf(header, sizeof header, "Submap %d (%d sprites) -- %d walkers/climbers!###submap%d",
                              s, (int)sprites.marks[s].size(), walkerClimberCount, s);
            else
                std::snprintf(header, sizeof header, "Submap %d (%d sprites)###submap%d", s, (int)sprites.marks[s].size(), s);
            if (tooManyWalkers) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
            bool headerOpen = ImGui::CollapsingHeader(header);
            if (tooManyWalkers) ImGui::PopStyleColor();
            if (headerOpen)
            {
                ImGui::Indent();
                if (tooManyWalkers)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
                    ImGui::TextWrapped(
                        "This submap has %d walker/climber entities (ent 4-15), but the real "
                        "engine can only keep 3 of that kind active at once (shared slot pool, "
                        "see ents.c). Extra ones silently fail to spawn -- move some to a "
                        "different submap, or remove/replace some.", walkerClimberCount);
                    ImGui::PopStyleColor();
                }
                auto &list = sprites.marks[s];
                int removeIdx = -1;
                for (int i = 0; i < (int)list.size(); i++)
                {
                    ImGui::PushID(i);
                    MarkEntry &m = list[i];
                    ImGui::SetNextItemWidth(50);
                    ImGui::DragInt("##ent", &m.ent, 1.0f, 0, 255);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Entity type");
                    ImGui::SameLine();
                    ImGui::TextUnformatted("row");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(50);
                    int rowBase = submapStartRow(connections.submaps[s]);
                    if (ImGui::DragInt("##markrow", &m.rowAbs, 8.0f, rowBase, MAP_TILE_ROWS - 1))
                    {
                        // The real engine only respects rows that are a multiple of 8
                        // LOCAL tile-rows from the submap's start -- see
                        // xrick_marks.h's snapMarkRowToBase(). Fold any remainder
                        // (e.g. from typing an exact number via ctrl-click) into
                        // fineY *and* trigRowOffset together, so both the sprite's
                        // own row and its trigger's row are preserved -- a plain
                        // drag (already on-grid, moving by a multiple of 8) leaves
                        // both untouched.
                        int newFineY, newTrigRowOffset;
                        m.rowAbs = snapMarkRowToBase(m.rowAbs, rowBase, m.fineY, m.trigRowOffset, newFineY, newTrigRowOffset);
                        m.fineY = newFineY;
                        m.trigRowOffset = newTrigRowOffset;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Block row ~%d -- steps by 8 tile-rows: that's the "
                                           "coarsest grid the real engine allows for this field "
                                           "(it masks the raw byte). Use the fineY field to its "
                                           "right for anything finer.", m.rowAbs / 4);
                    ImGui::SameLine();
                    ImGui::TextUnformatted("+");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(32);
                    ImGui::DragInt("##fineY", &m.fineY, 0.2f, 0, 7);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fine row offset (0-7, same weight as a whole row -- not pixels)");
                    ImGui::SameLine();
                    ImGui::TextUnformatted("col");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(40);
                    ImGui::DragInt("##markcol", &m.col, 1.0f, 0, 31);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) removeIdx = i;

                    // Trigger point and behavior flags: BOTH only mean
                    // something for specific entity-type ranges (see
                    // "Enemy types and behavior flags" in EDITEUR.md).
                    // Showing controls that the game silently ignores was
                    // actively misleading -- e.g. a type-1b entity in the
                    // stock data happens to carry flags=0xf0 (all four
                    // TRIG* bits) that the game NEVER reads for that type
                    // (e_them_t1_action2 in e_them.c only consults
                    // latency/offsx/step_count), so it looked like those
                    // flags might explain its behavior when they're just
                    // inert leftover bytes. Scope each control to the
                    // entity types that actually consult it instead.
                    bool isType3 = m.ent >= 0x18;
                    bool isType1a = (m.ent == 4 || m.ent == 7 || m.ent == 10 || m.ent == 13);
                    bool isWalkerOrClimber = m.ent >= 4 && m.ent <= 15;

                    if (isType3)
                    {
                        ImGui::TextDisabled("  trigger ->");
                        ImGui::SameLine();
                        ImGui::TextUnformatted("col");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(40);
                        ImGui::DragInt("##trigcol", &m.trigCol, 1.0f, 0, 31);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("+row");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(32);
                        ImGui::DragInt("##trigrowoff", &m.trigRowOffset, 0.2f, 0, 7);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Where this trap's trigger box sits and reacts to "
                                               "Rick/a bullet/a bomb, per the flags below -- not its "
                                               "own drawn position.");
                    }
                    else if (isType1a)
                    {
                        ImGui::TextDisabled("  patrol distance ->");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(40);
                        ImGui::DragInt("##trigcol", &m.trigCol, 1.0f, 0, 31);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Walks right from spawn for this many tiles,\n"
                                               "then u-turns. Green zone shows the range.");
                    }
                    // type 1b/2 (chaser/climber) and box/bonus types (16-23)
                    // don't consult the trigger point at all -- nothing shown.

                    if (isType3)
                    {
                        // Behavior flags (mark_t.flags). Only meaningful for
                        // "type 3" entities (ent id >= 0x18): those start
                        // asleep and need at least one TRIG* bit to ever wake
                        // up. See xrick_marks.h header comment for the source.
                        ImGui::TextDisabled("  flags ->");
                        ImGui::SameLine();
                        ImGuiStyle &flagStyle = ImGui::GetStyle();
                        float flagWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                        auto flagBox = [&](const char *label, int bit, const char *tip)
                        {
                            bool on = (m.flags & bit) != 0;
                            if (ImGui::Checkbox(label, &on))
                                m.flags = on ? (m.flags | bit) : (m.flags & ~bit);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                            float nextRight = ImGui::GetItemRectMax().x + flagStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                            if (nextRight < flagWindowRight)
                                ImGui::SameLine();
                        };
                        flagBox("Rick", ENT_FLG_TRIGRICK, "TRIGRICK -- wakes up when Rick walks into the trigger box");
                        flagBox("Cane", ENT_FLG_TRIGSTOP, "CANE -- wakes up when Rick does his cane move (FIRE+direction) inside the trigger box");
                        flagBox("Bullet", ENT_FLG_TRIGBULLET, "TRIGBULLET -- wakes up when a bullet hits the trigger box");
                        flagBox("Bomb", ENT_FLG_TRIGBOMB, "TRIGBOMB -- wakes up when a bomb hits the trigger box");
                        flagBox("Once", ENT_FLG_ONCE, "ONCE -- plays once and stays gone, instead of looping/respawning");
                        flagBox("LethalWake", ENT_FLG_LETHALI, "LETHALI -- lethal to Rick as soon as it wakes up");
                        flagBox("LethalLoop", ENT_FLG_LETHALR, "LETHALR -- lethal to Rick when it restarts a loop");
                        flagBox("StopsRick", ENT_FLG_STOPRICK, "STOPRICK -- this entity physically stops Rick (e.g. a solid block), goes to the special slot 0");
                        ImGui::NewLine();
                    }
                    else if (isWalkerOrClimber)
                    {
                        // The one meaningful exception to "flags are unused by this
                        // type": setting ALL FOUR trigger bits at once (0xF0) makes
                        // this entity borrow its own `sni` as a sprite index the
                        // instant it lands, keeping its normal look while airborne --
                        // the "type 1 morphs into type 2 on landing" trick confirmed
                        // in ents.c (see ENT_FLG_MORPH_TO_TYPE2 in xrick_marks.h). Any
                        // other combination of these bits is still inert for this
                        // entity type, so expose only this one meaningful state as a
                        // single toggle, not 8 checkboxes implying fine control that
                        // doesn't actually exist for it.
                        bool morphs = (m.flags == ENT_FLG_MORPH_TO_TYPE2);
                        ImGui::TextDisabled("  flags ->");
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Type 2 on landing", &morphs))
                            m.flags = morphs ? ENT_FLG_MORPH_TO_TYPE2 : 0;
                        if (ImGui::IsItemHovered())
                        {
                            int sni = (m.ent >= 0 && m.ent < (int)entDataTable.size()) ? entDataTable[m.ent].sni : 0;
                            ImGui::SetTooltip("Keeps this entity's own sprite while falling, then "
                                               "switches to sprite #%d (its own sni field) the instant "
                                               "it touches the ground -- confirmed in ents.c, e.g. the "
                                               "falling guy on submap 3. Sets/clears all four TRIG* bits "
                                               "together (flags = 0xF0); any other combination of them "
                                               "has no effect for this entity type.", sni);
                        }
                        if (m.flags != 0 && m.flags != ENT_FLG_MORPH_TO_TYPE2)
                            ImGui::TextDisabled("  (flags = 0x%02X -- not the 0xF0 combo, so this has no effect for this entity type)", m.flags);
                    }
                    ImGui::PopID();
                }
                if (removeIdx >= 0) list.erase(list.begin() + removeIdx);
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        ImGui::End();
        }

        // Submap boundary labels: a thin line + "Submap N" text at the
        // top row of each submap, drawn straight on the map so it's
        // visible in every canvas mode -- same background draw list as
        // the sprite overlay below, so it stays under every window too.
        {
            ImDrawList *dl = ImGui::GetBackgroundDrawList();
            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
            {
                float wy = (float)submapStartRow(connections.submaps[s]) * (float)TILE_PX;
                float sy = (wy - st.cam.y) * st.cam.zoom;
                if (sy < -20 || sy > viewportH + 20) continue;
                float sx0 = (0.0f - st.cam.x) * st.cam.zoom;
                float sx1 = (32.0f * (float)TILE_PX - st.cam.x) * st.cam.zoom; // MAP_COLS(8 blocks)*4 tiles/block = 32 tile columns
                dl->AddLine(ImVec2(sx0, sy), ImVec2(sx1, sy), IM_COL32(140, 190, 255, 130), 1.5f);
                char lbl[16]; std::snprintf(lbl, sizeof lbl, "Submap %d", s);
                dl->AddText(ImVec2(sx0 + 4, sy + 2), IM_COL32(140, 190, 255, 230), lbl);
            }
        }

        // Tile-row labels on both sides of the map, just below each block
        // boundary (tile rows that are multiples of 4), in submap mode --
        // always visible, whether or not the grid overlay is on. Drawn on
        // the background draw list, like the submap lines above.
        if (st.canvasMode == CanvasMode::Submap)
        {
            ImDrawList *dl = ImGui::GetBackgroundDrawList();
            float mapLeft = (0.0f - st.cam.x) * st.cam.zoom;
            float mapRight = ((float)MAP_COLS * BLOCK_PX - st.cam.x) * st.cam.zoom;
            int firstR = std::max(0, (int)std::floor(st.cam.y / BLOCK_PX));
            int lastR = std::min(MAP_ROWS - 1, (int)std::floor((st.cam.y + viewportH / st.cam.zoom) / BLOCK_PX));
            for (int r = firstR; r <= lastR; r++)
            {
                float sy = (r * (float)BLOCK_PX - st.cam.y) * st.cam.zoom;
                if (sy < -20 || sy > viewportH + 20) continue;
                char lbl[8]; std::snprintf(lbl, sizeof lbl, "%d", r * 4);
                ImU32 col = IM_COL32(208, 216, 232, 220);
                dl->AddText(ImVec2(mapLeft - 36, sy + 3), col, lbl);
                dl->AddText(ImVec2(mapRight + 5, sy + 3), col, lbl);
            }
        }

        // Sprite overlay on the map canvas: real sprite art when available
        // (ent_entdata's spr field, decoded from sprites_data -- see
        // sprites_render.h), a colored marker fallback otherwise. Drawn on
        // top of the tiles but BELOW every ImGui window: the background
        // draw list renders first in ImGui's draw pass (right after the
        // SDL-rendered map, before any window), unlike the foreground
        // draw list which renders last and would otherwise paint sprites
        // over the palette/tools windows.
        if (st.showSprites)
        {
            ImDrawList *dl = ImGui::GetBackgroundDrawList();
            ImTextureID spriteTexId = (ImTextureID)(intptr_t)spriteAtlas;
            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
            {
                for (auto &m : sprites.marks[s])
                {
                    // Own position: (col, row+fineY) -- fineY is added at
                    // the SAME weight as a whole row unit before the
                    // engine's pixel conversion, not a small pixel nudge
                    // (confirmed in ents.c). See markEffectiveRow().
                    float wx = m.col * (float)TILE_PX;
                    float wy = markEffectiveRow(m) * (float)TILE_PX;
                    float sx = (wx - st.cam.x) * st.cam.zoom;
                    float sy = (wy - st.cam.y) * st.cam.zoom;
                    if (sx < -60 || sy < -60 || sx > viewportW + 60 || sy > viewportH + 60) continue;

                    int spr = (m.ent >= 0 && m.ent < (int)entDataTable.size()) ? entDataTable[m.ent].spr : 0;
                    if (spr > 0 && spr < SPRITES_NBR_SPRITES)
                    {
                        float u0, v0, u1, v1;
                        sprite_uv(spr, u0, v0, u1, v1);
                        float w = SPRITE_W * st.cam.zoom, h = SPRITE_H * st.cam.zoom;
                        dl->AddImage(spriteTexId, ImVec2(sx, sy), ImVec2(sx + w, sy + h), ImVec2(u0, v0), ImVec2(u1, v1));
                    }
                    else
                    {
                        float r = std::clamp(4.0f * st.cam.zoom, 3.0f, 10.0f);
                        dl->AddCircleFilled(ImVec2(sx, sy), r, IM_COL32(255, 210, 60, 230));
                        dl->AddCircle(ImVec2(sx, sy), r, IM_COL32(60, 40, 0, 255), 0, 1.5f);
                    }
                    if (st.cam.zoom >= 1.5f)
                    {
                        char lbl[8]; std::snprintf(lbl, sizeof lbl, "%d", m.ent);
                        dl->AddText(ImVec2(sx + 2, sy - 12), IM_COL32(255, 255, 0, 255), lbl);
                    }

                    // Trigger point (decoded from the old `lt` byte -- see
                    // MarkEntry's comment): some entity types (e.g. arrow
                    // traps) react to Rick's position there rather than at
                    // their own (col,row). Only drawn when it actually
                    // differs from the entity's own spot, to avoid clutter
                    // for entities that don't use it.
                    int trigRow = markTriggerRow(m);
                    if (m.trigCol != m.col || trigRow != m.rowAbs)
                    {
                        float twx = m.trigCol * (float)TILE_PX + TILE_PX / 2.0f;
                        float twy = trigRow * (float)TILE_PX + TILE_PX / 2.0f;
                        float tsx = (twx - st.cam.x) * st.cam.zoom;
                        float tsy = (twy - st.cam.y) * st.cam.zoom;
                        ImU32 trigCol = IM_COL32(255, 60, 60, 220);
                        dl->AddLine(ImVec2(sx, sy), ImVec2(tsx, tsy), IM_COL32(255, 60, 60, 120), 1.5f);
                        float cr = std::clamp(3.0f * st.cam.zoom, 3.0f, 8.0f);
                        dl->AddLine(ImVec2(tsx - cr, tsy - cr), ImVec2(tsx + cr, tsy + cr), trigCol, 2.0f);
                        dl->AddLine(ImVec2(tsx - cr, tsy + cr), ImVec2(tsx + cr, tsy - cr), trigCol, 2.0f);
                    }
                    // Full trigger box for grille traps and arrow traps.
                    // Shows the actual rectangular zone the engine tests
                    // against in u_trigbox(), so the user can see whether
                    // Rick's walking path overlaps with it.
                    // Uses the 3-bit-truncated trigRowOffset (the engine
                    // reads lt & 0x07, not the full stored value) so the
                    // box matches what xrick actually does at runtime.
                    if (st.showTriggerBoxes)
                    {
                        int tw, th;
                        entTriggerSize(m.ent, tw, th);
                        if (tw > 0 && th > 0)
                        {
                            int engineTrigRow = m.rowAbs + (m.trigRowOffset & 7);
                            float bx0 = m.trigCol * (float)TILE_PX;
                            float by0 = engineTrigRow * (float)TILE_PX;
                            float bx1 = bx0 + tw * TILE_PX;
                            float by1 = by0 + th * TILE_PX;
                            float sx0 = (bx0 - st.cam.x) * st.cam.zoom;
                            float sy0 = (by0 - st.cam.y) * st.cam.zoom;
                            float sx1 = (bx1 - st.cam.x) * st.cam.zoom;
                            float sy1 = (by1 - st.cam.y) * st.cam.zoom;
                            dl->AddRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                        IM_COL32(255, 60, 60, 100), 0.0f, 0, 2.0f);
                            dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                              IM_COL32(255, 60, 60, 25));
                        }
                    }
                    // Patrol zone for type-1a walkers: horizontal range
                    // from spawn to spawn + trigCol tiles.
                    if (st.showTriggerBoxes)
                    {
                        bool isType1a = (m.ent == 4 || m.ent == 7 || m.ent == 10 || m.ent == 13);
                        if (isType1a && m.trigCol > 0)
                        {
                            float px0 = wx + TILE_PX;
                            float py0 = wy;
                            float px1 = wx + (m.trigCol + 2) * (float)TILE_PX;
                            float py1 = wy + SPRITE_H;
                            float dzx0 = (px0 - st.cam.x) * st.cam.zoom;
                            float dzy0 = (py0 - st.cam.y) * st.cam.zoom;
                            float dzx1 = (px1 - st.cam.x) * st.cam.zoom;
                            float dzy1 = (py1 - st.cam.y) * st.cam.zoom;
                            dl->AddRect(ImVec2(dzx0, dzy0), ImVec2(dzx1, dzy1),
                                        IM_COL32(80, 200, 80, 120), 0.0f, 0, 1.5f);
                            dl->AddRectFilled(ImVec2(dzx0, dzy0), ImVec2(dzx1, dzy1),
                                              IM_COL32(80, 200, 80, 20));
                        }
                    }
                }
            }
        }

        // Map start position markers above sprites (foreground draw list).
        if (st.showMapStartPositions)
        {
            ImDrawList *dl = ImGui::GetBackgroundDrawList();
            ImTextureID tileTexId = (ImTextureID)(intptr_t)tileAtlas[0];
            for (int m = 0; m < MAP_NBR_MAPS; m++)
            {
                const MapStartInfo &ms = connections.mapStarts[m];
                float lx = (ms.x - st.cam.x) * st.cam.zoom;
                float ly = ((ms.row + 17) * TILE_PX - st.cam.y) * st.cam.zoom;
                if (lx < -200 || lx > viewportW + 200 || ly < -50 || ly > viewportH + 50)
                    continue;
                int markerTileIdx = 0x31 + m;
                int tx = (markerTileIdx % ATLAS_TILES_PER_ROW) * TILE_PX;
                int ty = (markerTileIdx / ATLAS_TILES_PER_ROW) * TILE_PX;
                float u0 = (float)tx / (float)TILE_ATLAS_PX;
                float v0 = (float)ty / (float)TILE_ATLAS_PX;
                float u1 = (float)(tx + TILE_PX) / (float)TILE_ATLAS_PX;
                float v1 = (float)(ty + TILE_PX) / (float)TILE_ATLAS_PX;
                float sz = TILE_PX * st.cam.zoom;
                dl->AddImage(tileTexId, ImVec2(lx, ly), ImVec2(lx + sz, ly + sz),
                             ImVec2(u0, v0), ImVec2(u1, v1));
            }
        }

        // --- Render ---
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        drawMap(renderer, blockAtlas[st.bank], st, connections, viewportW, viewportH);

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    for (int b = 0; b < TILES_NBR_BANKS; b++)
    {
        SDL_DestroyTexture(blockAtlas[b]);
        SDL_DestroyTexture(tileAtlas[b]);
    }
    SDL_DestroyTexture(spriteAtlas);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

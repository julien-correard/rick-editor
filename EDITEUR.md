# RickEditor -- level editor (v4, Dear ImGui)

The UI is in English (program requirement). Source code comments stay in
French, as in the rest of this codebase.

## Editing model

Editing works **at the block level**: the original map format is kept as-is
(`map_bnums[]` references indices into `map_blocks[0x100][16]`, each block =
16 tiles fixed in a 4x4 grid). You pick a block from the palette (256
blocks, as originally defined) and place it on the map. Blocks themselves
are not split into independently editable tiles for now -- to revisit later
alongside sprites, if needed.

Tile bank 0 is hidden from the picker (unused padding data, never shown in
the original editor either) -- only banks 1 and 2 are selectable.

## Controls

The top menu bar (after File and the map name) has the canvas mode
switch, the grid toggle, zoom buttons, a Fill selection button, and the
cursor's current coordinates -- the keyboard shortcuts below are just
quicker ways to reach the same things. There used to be a separate
"Tools" window duplicating most of this; it's gone now that everything
useful in it lives in the top bar instead.

The **Block Palette** window only shows up in Block mode, **Sprite
Tools** only in Sprite mode, and **Screen Connections** only in Submap
mode -- each is irrelevant clutter in the other two modes, so they get
out of the way instead of competing for space.

The map canvas also always shows a thin line + "Submap N" label at each
submap's top boundary, regardless of canvas mode -- a quick visual
anchor for which submap you're looking at without needing to open
Screen Connections.

| Action | Effect |
|---|---|
| **Submap / Block / Sprite** (top bar radio buttons) | Canvas mode: Submap = clicking the map does nothing, just look around and inspect Screen Connections; Block = paint/pick tiles (shows the Block Palette window); Sprite = place/remove sprites (shows the Sprite Tools window). Panning and zoom work in every mode. |
| Left click (hold = paint) -- Block mode | Place the selected block |
| Right click (hold = keep picking) -- Block mode | Eyedropper: selects the block under the cursor into the palette |
| Left/right click -- Sprite mode | Place the selected entity / remove the nearest sprite |
| Shift + drag -- Block mode | Rectangle selection |
| Del / Backspace | Clear the selection (sets it to block 0) |
| `F`, or **Fill selection** (top bar) | Fill the selection with the selected block |
| Mouse wheel, or `+`/`-` (also in the top bar) | Zoom (centered on cursor for the wheel, on screen center for the keys/buttons) |
| Middle-drag, or arrow keys Up/Down | Pan (fine, continuous) -- works in every canvas mode |
| Left/Right, Page Up/Page Down | Fast scroll: jumps 20 cells per key press (repeats while held) |
| `1` / `2` | Tile bank 1 / 2 |
| `S` | Toggle canvas mode between Block and Sprite |
| **Grid** checkbox (top bar), or `G` | Always-on block grid overlay (one square = one block); also auto-shown above a high zoom level regardless |
| Escape | Cancel the current selection |

## File menu (top menu bar only -- no separate dockable window)

- **Open...** / **Save** / **Save As...** -- this editor's own `.map`
  format (see `src/mapfile.h`/`src/xrick_marks.h`): a small header (magic
  + version + element count) followed by the 8152 `map_bnums` values,
  screen connections, sprites, per-tile hazard flags, tile graphics (as
  of "RKM6"), block composition (as of "RKM7"), and sprite graphics (as
  of "RKM8"). Loading refuses any file whose element count doesn't
  match. A `*` next to the file name means there are unsaved changes.
- **Patch xrick binary...** -- injects the level currently open in the
  editor (layout + connections + sprites + tile hazard flags + tile
  graphics + block composition + sprite graphics) directly into a
  compiled xrick executable. See below.
- **Import connections, sprites, tile hazards, tile graphics, blocks &&
  sprite graphics from xrick binary...** -- the reverse direction: pulls
  all of the above straight from a chosen binary's own compiled-in data
  (via its ELF symbols), overwriting what's currently in the editor.

### How the xrick patch works

The real xrick engine stores the level as a global C array called
`map_bnums` (same name, same values, just packed as one byte per entry
instead of the editor's 4-byte `int`, and same for `map_blocks`/
`tiles_data` -- this editor's data actually *comes from* xrick's own
`dat_tilesST.c`, so it lines up exactly). `sprites_data` doesn't need
that widen/pack step -- the editor's in-memory `sprite_t` already
matches the real engine's layout byte-for-byte, same as `tiles_data`.

Given an **unstripped** xrick executable (ELF32, Linux), the patcher:

1. Parses the ELF symbol table to find each relevant symbol by name
   (`map_bnums`, `map_submaps`, `map_connect`, `map_marks`,
   `map_eflg_c`, `tiles_data`, `map_blocks`, `sprites_data`) and
   computes where each lives in the file (works for any matching build
   -- the location isn't hard-coded to one specific binary).
2. Checks each symbol's size matches exactly what the editor currently
   holds; refuses to touch anything if it doesn't look like a compatible
   build.
3. Writes the current state (level layout, connections, sprites, hazard
   flags, tile graphics, block composition, sprite graphics) over those
   bytes, in a **copy** of the file named `<name>_patched` next to the
   original -- the input executable is never modified -- and copies over
   its file permissions (so the output stays executable).

Tested against a real xrick 20-year-old Linux/x86 build: the patched
region matches byte-for-byte and every other byte in the file is left
untouched (see `src/test_xrick_patch.cpp`, for `map_bnums`; the same
`elf32_patch_symbol()` helper is reused for every other symbol above,
including `tiles_data`, `map_blocks`, and `sprites_data`, each with its
own round-trip check -- see the Tile Editor, Block Editor, and Sprite
Editor sections below).

## Files

- `src/editor.cpp` -- main editor (UI, input handling, rendering).
- `src/mapfile.h` -- `.map` save/load (level layout only), isolated from
  the UI so it can be unit-tested on its own.
- `src/xrick_marks.h` -- `.map` save/load for everything past the level
  layout (connections, sprites, hazard flags, tile graphics, block
  composition -- the "RKM4"-"RKM7" formats) plus the corresponding
  xrick-binary patcher, `patchXrickBinaryWithSprites()`.
- `src/xrick_patch.h` -- ELF32 symbol lookup + generic symbol patching
  (`elf32_find_symbol_file_offset`/`elf32_patch_symbol`), reused by every
  patch/import path above; also the `map_bnums`-only patcher and
  `map_blocks_as_bytes()` (int -> byte packing for the patch).
- `src/tile_import.h` -- tile graphic import from an image file (palette
  quantization + encoding, see the Tile Editor section),
  `loadTilesFromXrickBinary()`, and `loadBlocksFromXrickBinary()`
  (despite the name -- grouped here since both are "read tile/block data
  straight from a binary" siblings of the same import flow; a rename to
  something like `tile_block_import.h` would be reasonable if this file
  grows further).
- `src/sprite_import.h` -- sprite graphic import from an image file
  (single + batch, see the Sprite Editor section), mirrors
  `tile_import.h` closely and reuses its `readWholeFile()`/stb_image
  setup via `#include "tile_import.h"`; also
  `loadSpritesFromXrickBinary()`.
- `src/test_mapfile.cpp` -- standalone round-trip test for the `.map`
  format (not part of the CMake build; compile/run manually: `g++
  -std=c++17 -Iinclude -Isrc src/test_mapfile.cpp src/dat_tilesST.c -o
  test_mapfile && ./test_mapfile`).
- `src/test_xrick_patch.cpp` -- standalone test for the xrick patcher,
  takes a real xrick binary path on the command line (not part of the
  CMake build): `g++ -std=c++17 -Iinclude -Isrc src/test_xrick_patch.cpp
  src/dat_tilesST.c -o test_xrick_patch && ./test_xrick_patch
  /path/to/xrick`.
- `src/test_tile_import.cpp` -- standalone round-trip test for image
  import -> tile encoding -> `decode_tile()` (not part of the CMake
  build; compile/run manually: `g++ -std=c++17 -Iinclude -Isrc
  -Ithird_party src/test_tile_import.cpp src/dat_tilesST.c -o
  test_tile_import && ./test_tile_import`).
- `src/test_tile_batch_import.cpp` -- standalone test for batch tile
  import (grid slicing, no-bleed-between-tiles, tile-255 overflow
  handling -- not part of the CMake build; compile/run manually: `g++
  -std=c++17 -Iinclude -Isrc -Ithird_party src/test_tile_batch_import.cpp
  src/dat_tilesST.c -o test_tile_batch_import && ./test_tile_batch_import`).
- `src/test_sprite_import.cpp` -- standalone test for single + batch
  sprite import (round-trip through `decode_sprite()` incl.
  transparency, no-bleed between batch-imported sprites, end-of-table
  overflow -- not part of the CMake build; compile/run manually: `g++
  -std=c++17 -Iinclude -Isrc -Ithird_party src/test_sprite_import.cpp
  src/dat_tilesST.c src/dat_spritesST.c -o test_sprite_import &&
  ./test_sprite_import`).
- `src/test_sprite_persist.cpp` -- standalone round-trip test for the
  RKM8 `.map` format (sprite graphics -- not part of the CMake build;
  compile/run manually: `g++ -std=c++17 -Iinclude -Isrc -Ithird_party
  src/test_sprite_persist.cpp src/dat_tilesST.c src/dat_spritesST.c -o
  test_sprite_persist && ./test_sprite_persist`).
- `src/test_patch_sprites.cpp` -- standalone test for the `sprites_data`
  ELF patch + reload round-trip, takes a synthetic (or real) ELF32
  binary path on the command line (not part of the CMake build): `g++
  -std=c++17 -Iinclude -Isrc -Ithird_party src/test_patch_sprites.cpp
  src/dat_tilesST.c src/dat_spritesST.c -o test_patch_sprites &&
  ./test_patch_sprites /path/to/xrick`.
- `src/test_block_swap.cpp` -- standalone test for the Block Editor's
  block-swap logic (not part of the CMake build; compile/run manually:
  `g++ -std=c++17 -Iinclude -Isrc src/test_block_swap.cpp -o
  test_block_swap && ./test_block_swap`).
- `src/tiles_render.h` -- tile decoding + atlas texture construction
  (tiles and blocks, incl. `build_block_atlas()` which assembles blocks
  from `map_blocks`).
- `third_party/stb/stb_image.h` -- vendored image loader (PNG/BMP/TGA/
  JPG/GIF/PSD/...), used by `tile_import.h` for the Tile Editor's
  "Import from image..." button.
- `src/legacy_main.cpp.txt` -- old faithful SDL1-port version (bug-fixed,
  behavior-preserving), kept for reference, not compiled.

## Still ahead

Sprites and the rest of the engine beyond what's listed above: not
addressed.

## Screen connections (links between submaps)

Rick Dangerous has no horizontal scrolling: the level is a chain of fixed
screens ("submaps" in the engine), and moving to the next one is a
vertical transition -- walking off the top or bottom of the current
screen. Each submap owns a chain of links to other submaps; a link is
`{dir, row, target submap, target row}`.

**Coordinates are absolute**, in the same row numbering as everywhere
else in the editor (matches the coordinates shown in the top bar) --
not the raw engine's per-submap-relative byte values. A link is shown as
one row (on the submap whose card it's listed under) connected to one row
on the target submap; `dir` (Down/Up) just says which way through it
counts as a trigger, direction isn't otherwise called out as
"entrance"/"exit" in the UI. Absolute row `submapStartRow(submap) = bnum
/ 2` marks where a submap's own data begins in the flat map grid.

**Loaded automatically at startup** (compiled into the program from a
stock xrick binary, `src/connections_default.h` -- same idea as the
default map data) and **saved/loaded with the map itself**: `.map` files
now include the full screen-connection graph (`File > Save` / `Open...`).
Older bnums-only `.map` files still open fine; they just leave the
connections as they currently are (default or previously loaded).

Per submap, in the **Screen Connections** window (only shown in Submap
mode -- see Controls above):
- **Start row**: directly editable -- this is the real stored value
  (`bnum`), just shown/edited in tile-row units. There's deliberately no
  matching "end row" to set: the real format has no stored height for a
  submap at all. It's an open-ended scrolling window into the shared
  block canvas (confirmed in `maps.c`: `map_frow` slides indefinitely as
  Rick moves, sliding the read window through `map_bnums` right along
  with it) -- the only thing that actually stops a submap in practice is
  one of its own Screen Connections exits sending Rick elsewhere. Moving
  Start row does NOT move its blocks (paint those separately in Block
  mode) -- it only changes which part of the shared canvas this submap's
  window points at. Existing marks/exits stay anchored to their own
  absolute row (they don't move with it); if that ends up too far from
  the new start to still fit the format, `repackMarks()`/
  `repackConnections()` catch it with a clear error rather than silently
  producing a broken binary.
- A read-only line under it shows the lowest/highest row this submap's
  own sprites and exits actually use *right now* -- not a stored value,
  just a practical sense of how far it's been built out.
- **Tile bank** selector (1 or 2 -- same banks as the Block Palette;
  maps to the engine's internal `page` field).
- **Delete submap**: since the 47-entry `map_submaps` array can't
  shrink, "delete" means clearing this submap's own links and
  redirecting any other submap's link that pointed to it to "end of
  world" -- its tiles/sprites stay in the data, just cut off from the
  level's flow, unreachable in normal play.
- Per link: direction, its own absolute row, target submap (or "end of
  world"), and the target's absolute row. A row field turns red if it's
  too far from its owning submap's own rows to fit back into the
  original byte format (see below) -- hover it for why.
- **+ Add link** / **X** to add or remove one.

**File > Import connections from xrick binary...** is now an optional,
advanced action: it overwrites the current in-memory connections with
whatever's in a chosen binary (e.g. to start from a different xrick
build instead of the compiled-in stock data).

### The fixed-capacity constraint

`map_submaps` (47 entries) and `map_connect` (153 slots total, shared
across every submap) are **fixed-size arrays** in the compiled xrick
binary -- patching can rewrite their content, but not grow them. So:

- Adding a new link only works if there's spare room across the whole
  153-slot table (all submaps combined, including one end-marker each).
  The stock binary checked against is already 100% full (106 links + 47
  markers), so adding one there means removing one elsewhere first.
- A row too far (>255) from its owning submap's own start row can't be
  represented in the original per-submap-relative byte format; the
  editor refuses that edit with a clear message rather than silently
  producing a broken binary.
- "Deleting" a submap disconnects it rather than removing the slot --
  the 47-entry array can't shrink either.

`File > Patch xrick binary...` always re-packs and patches both
`map_bnums` and the connection graph together now.

See `src/xrick_levels.h` (load/repack/patch/.map v2) and
`src/test_xrick_levels.cpp` (round-trip tests against a real binary,
path given as argv[1], not part of the CMake build).

## Sprites (entity placement)

Entities (enemies, traps, pickups...) are placed the same structural way
as screen connections: `map_marks[523]` is a single shared, packed array,
each submap owns a contiguous run (`map_submaps[s].mark` start, `row ==
255` terminator) -- verified against this project's binary the same way
as connections: every submap's run does end with that marker, and the
table is, again, already 100% full (476 sprites + 47 end-markers =
523/523).

A sprite/mark is `{row, ent, flags, xy, lt}` in the real engine. Rows
work exactly like Screen Connections (absolute, `submapStartRow(submap) +
local row`). `xy` packs a tile column (0-31, shared across the whole
32-tile map width) and a few bits of fine vertical offset (`fineY`, see
below). `flags` and `lt` (trigger point) are both decoded now -- see
"Enemy types and behavior flags" further down.

There's no name/graphics table for entity types baked into this binary
(no strings, and decoding `sprites_data`'s actual pixel format wasn't
attempted here) -- entities are exposed as a raw numeric id (4-73 are
used in the stock data). Placed sprites show as a small circular marker
with the id as a label, not real game art.

**Sprite Tools** window:
- **Show sprites on map** -- overlay toggle, independent of editing mode
  (turn it off to declutter the view while working on tiles).
- **Sprite placement mode** -- when on, canvas clicks place/remove
  sprites (left = place the selected entity type, right = remove the
  nearest one) instead of editing tiles; off by default so it doesn't
  interfere with normal block editing.
- Entity type picker, with quick-pick buttons for ids already used
  elsewhere on the currently loaded map.
- Per-submap list of its sprites (row / column / entity type, editable;
  trigger point and behavior flags are editable too -- see "Enemy types
  and behavior flags" below. Freshly placed traps (entity id >= 0x18)
  default to reacting to Rick, since that's the overwhelmingly common
  case; other entity types default to no flags, which is correct for
  simple walkers).

Loaded automatically at startup (`connections_default.h`, same stock
xrick binary as the map and connections) and **saved/loaded with the
map** (`.map` is now format v3, `RKM3`): `File > Save` / `Open...`
include sprites, screen connections, and the level layout together.
Older v1/v2 `.map` files still open fine, leaving whatever's missing
untouched. `File > Patch xrick binary...` now always patches all three
(`map_bnums`, `map_submaps`/`map_connect`, `map_marks`) together.
`File > Import connections && sprites from xrick binary...` overwrites
both from a chosen binary (optional/advanced, e.g. to start from a
different build).

See `src/xrick_marks.h` (load/repack/patch/.map v3) and
`src/test_xrick_marks.cpp` (round-trip tests against a real binary, path
given as argv[1], not part of the CMake build).

## Sprite placement fix (unit bug found using the real xrick source)

You provided the actual xrick source (`rick.zip`) after the sprite
feature above was first built from binary reverse-engineering alone.
Reading it turned up a real bug: `map_frow` -- and by direct extension,
`connect_t.rowout`/`rowin` and `mark_t.row` (both added to/subtracted
from `map_frow` with no unit conversion) -- are in **TILE-row** units,
not block-row units as originally assumed. Confirmed unambiguously by
two comments in `maps.c` itself:

> map_frow is map_map top row within the submap ... We need to /4
> map_frow to convert from tile rows to block rows.

So a submap's own absolute row start is `bnum/2` (tile-rows), not
`bnum/8` (block-rows) -- off by a factor of 4. This was fixed in both
`submapStartRow()` (`xrick_levels.h`, used by Screen Connections) and a
duplicated copy of the same calculation inside `decodeMarksFromRaw()`
(`xrick_marks.h`, sprites) that had been written before the unit was
confirmed and didn't get updated at the same time -- that second, missed
spot was the immediate cause of the sprites looking wrong. Both are now
consistent and share the tile-row unit.

Practical effects of the fix:
- Screen Connections and Sprite Tools rows are now four times finer than
  before, and the *"Cell under cursor"* text in Tools shows the matching
  tile row alongside the block row so you can cross-reference the two.
  Row fields also show a "block row ~N" tooltip.
- Sprite placement (click position -> tile row) and the on-map sprite
  overlay (tile-row -> screen pixel) were updated to the same tile-row
  scale, so placed sprites now line up with the row they're dropped on
  instead of appearing four rows off.
- Regression tests added in `test_xrick_levels.cpp` and
  `test_xrick_marks.cpp` hard-check specific known values (e.g. submap 1,
  bnum=120 -> tile-row start 60, not 15) against the source-derived
  formula, so this can't silently regress again.

Everything else already documented above (absolute rows, capacity
limits, the caveat about `flags`/`lt`/fine-offset semantics) still
applies -- only the row *scale* changed, not the overall model.

## Sprite fixes and real sprite art (using the real xrick source)

You provided the full xrick source (`rick.zip`) after noticing sprites
looked wrong. Reading `ents.c` (`ent_reset()`) turned up two more issues,
on top of the tile-row/block-row fix from before:

**1. Sprites appearing shifted up.** The real Y formula is:

    y = ((xy & 7) + (row & 0xf8) - map_frow) * 8

`fineY` (`xy & 7`) is added to `row` **before** the final ×8, so it
carries the *same weight as a whole row unit* (0-7 rows = 0-56 pixels),
not a small 0-7 pixel nudge. 444 of the 476 stock sprites have a nonzero
`fineY` (mostly 1 or 5) -- the editor was under-adding this offset for
nearly all of them, hence the systematic upward shift (e.g. an enemy that
should sit inside open space landing inside a solid block instead).
Fixed: `markEffectiveRow(mark) = mark.rowAbs + mark.fineY` is now used
everywhere a sprite's real vertical position is needed (rendering,
placement), instead of scaling `fineY` as raw pixels.

**2. Sprite 25 (arrow trap)'s firing position not matching its own
placement -- this is by design, now made visible.** The `lt` byte (which
the editor exposed as an opaque raw value) actually decodes into a
**separate trigger point**:

    trig_x = lt & 0xf8
    trig_y = 3 + 8 * ((row & 0xf8) - map_frow + (lt & 0x07))

Confirmed directly in `ents.c`. Some entity types (arrow traps among
them) react to Rick's position at this *trigger* point rather than at
their own drawn position -- e.g. a wall-mounted trap's firing plate can
legitimately sit in a different column of the room. `MarkEntry` now
exposes this as `trigCol` / `trigRowOffset` (editable in Sprite Tools,
under each sprite's "trigger ->" row) instead of a raw `lt` byte, and the
map overlay draws a small red crosshair with a connecting line from the
entity to its trigger point whenever the two differ, so you can see (and
fix) the relationship directly instead of guessing at a hex byte.
Not every entity type consults this -- for those that don't it's inert,
harmless data.

**3. Real sprite art.** `dat_spritesST.c` (sprite pixel data) and the
relevant part of `dat_ents.c` (entity-type -> sprite-index table) are now
vendored into the project (`src/dat_spritesST.c`,
`src/connections_default.h`'s `entDataTable`), decoded with the same
nibble-per-pixel algorithm as tiles (confirmed against `draw.c`'s
`draw_sprite()`), and built into a texture atlas
(`src/sprites_render.h`). Placed entities now show their real first
animation frame when one is defined for that entity type (`ent_entdata`'s
`spr` field), falling back to the colored dot only for types with no
distinct sprite (`spr == 0`). The quick-pick strip in Sprite Tools shows
the same thumbnails.

Because sprite rows/positions changed shape (`fineY` scale) and `lt`
became two explicit fields, the `.map` format bumped to **v4**
(`RKM4`); v1/v2/v3 files are handled the same way older formats always
have been here -- whatever a given version doesn't include is simply
left as-is when loading.

Regression tests for both fixes (hand-checked against the raw bytes of a
real mark) are in `test_xrick_marks.cpp`.

### Still not attempted
- Sprite *animation* (only the first frame is shown; `ent_sprseq` /
  `ent_mvstep`, which drive frame sequencing, weren't decoded).
- `ent_entdata`'s source table has 76 entries; this project's actual
  xrick binary's `ent_entdata` symbol is sized for 74 -- a discrepancy
  between the provided source and that specific compiled binary that
  doesn't affect anything patched back into the binary (sprite art is a
  purely local editor/rendering aid, never written back), but is worth
  knowing about if the two are compared directly.

## The 8-tile-row placement grid (why a fresh trap could fire "one block higher" than placed)

Found while investigating an arrow trap (`ent` 25) that fired noticeably
above where it was placed. Root cause, confirmed in `ents.c`
(`ent_reset()`): the real engine masks the raw row byte with `& 0xf8`
for BOTH an entity's own y and its trigger's y --

    y      = ((xy & 7)  + (row & 0xf8) - map_frow) * 8
    trig_y = 3 + 8 * ((row & 0xf8) - map_frow + (lt & 0x07))

-- so the shared `row` field only takes effect at a resolution of 8
LOCAL tile-rows (relative to the owning submap's own start row, *not* a
fixed absolute multiple -- submaps don't all start on an 8-row
boundary themselves). Every one of the 476 sprites in the stock binary
does sit on that grid (confirmed by direct inspection), which is exactly
why it went unnoticed at first: the *stock* data was always authored on
it. Any *new or edited* mark whose row wasn't on that grid used to look
completely fine in this editor's own preview (which didn't reproduce the
masking) but got silently rounded **down** by the real game once
patched -- i.e. shifted to an earlier, physically **higher** row on
screen, by up to 7 tile-rows (almost 2 blocks) depending on how far off
the grid it was.

The remainder doesn't have to be lost, though: `fineY` (for the entity's
own position) and `trigRowOffset` (for its trigger) are added back
*after* that masking, unmasked, 0-7 each -- so any absolute row is still
exactly reachable, just split into "coarse row" (multiple of 8, shared
by both the entity and its trigger) + "fine remainder" (0-7, separate
for each). Fixed by folding the split in automatically instead of
requiring it to be done by hand:
- **Placing** a new sprite (canvas click) now decomposes the clicked row
  into that coarse+fine split itself (`snapMarkRowToBase()` in
  `xrick_marks.h`) -- you still land exactly on the row you clicked, the
  editor just distributes it correctly under the hood.
- The **row field** in Sprite Tools' per-sprite list now drags in steps
  of 8 (matching the real grid) instead of 1, and folds any leftover
  from typing an exact number into `fineY` automatically. Its tooltip
  says as much -- use the `fineY` field right next to it for anything
  finer.
- `repackMarks()` (the `.map` save / binary-patch path) now defensively
  re-snaps any row that somehow isn't on the grid, as a last-resort
  safety net, instead of silently writing a value the real game would
  interpret differently than shown.

**Follow-up bug in that same fix:** the first version of this fix only
folded the leftover remainder into `fineY`, correctly preserving the
entity's own drawn position -- but left `trigRowOffset` untouched, so
re-basing the shared coarse `row` silently dragged the *trigger*'s
row along with it, uncompensated. Since both `fineY` and
`trigRowOffset` are added to the exact same coarse `row`, shifting that
shared base without adjusting both is the same class of bug all over
again, just one level deeper -- this is exactly what still made an
arrow trap fire "one block higher" than shown even after re-patching
with the first fix in place. `snapMarkRowToBase()` now takes and
updates both `fineY` and `trigRowOffset` together (falling back to
rounding the coarse row up instead of down, and as an absolute last
resort clamping, on the rare off-grid mark where the two would
otherwise need to land in different 8-row windows) -- both a mark's own
row and its trigger's row now survive a patch unchanged.

Note this only affects the shared coarse `row` -- `trigRowOffset` itself
was already correct and needed no fix; a trap's trigger point
legitimately sitting a few rows above or below its own sprite (as seen
throughout the stock data) is normal, by-design behavior, not this bug.

## Enemy types and behavior flags

Confirmed against the real source (`ents.h`, `ents.c`, `e_them.c`,
`util.c`), not guessed. This explains what an entity's own `.n` (its
numeric type id, i.e. the "entity type" field in Sprite Tools) actually
makes it *do*, and what the `flags` checkboxes and trigger point
(`trigCol`/`trigRowOffset`) do for each case.

**The engine dispatches an entity's behavior purely from its numeric
type id (`ent`)** -- there is no separate "kind" field:

- **`ent` 4, 7, a, d -- type "1a" (patrol walker).** Walks back and
  forth over a fixed horizontal distance then U-turns, falls if the
  ground disappears under it. **Its trigger point's column is reused as
  the patrol distance** (`trig_x >> 1` steps before turning around) --
  moving the trigger point changes how far it walks, it is *not* a
  spatial trigger for this type. `flags` is otherwise unused by this
  type *except* for one specific combination -- see "Morphing into
  type 2 on landing" below.
- **`ent` 5, 8, b, e -- type "1b" (chaser walker).** Same movement code
  as 1a (walks, falls, U-turns), but steers itself to always advance
  towards Rick's current horizontal position instead of patrolling a
  fixed distance. Same trigger-point caveat as 1a: the column doesn't
  behave as a spatial trigger for this type either. Same `flags`
  exception as 1a below.
- **`ent` 6, 9, c, f -- type "2" (climber).** Climbs along ladder-like
  ("climbable") map cells towards Rick's position (vertically first,
  then horizontally, or vice versa), and free-falls when off a
  climbable surface. Trigger point isn't consulted by this type. Same
  `flags` exception as 1a/1b below (in practice not useful here, since
  it makes an entity morph *into* the very sprite family it's already
  in -- but the check in the source isn't type-specific, just slot-pool
  based, so it's technically active for this type too).
- **`ent` >= 0x18 (24) -- type "3" (trap / triggered entity).** Starts
  **asleep** and invisible-to-harm, playing no animation, until woken up
  by one of the `flags` TRIG* checkboxes firing (see below); once awake
  it plays through its `ent_mvstep` move sequence once, then either
  loops back to sleep or disappears depending on the `Once` flag. This
  is the type used for arrow traps, falling rocks, swinging axes, etc.
  **A trap with no TRIG* flag set can never wake up**, which is exactly
  what silently breaks newly-placed traps (see below).

Entity ids below 4 (0-3) never appear in the stock data (likely reserved
for Rick himself / special-cased slots) and ids in between the ranges
above that aren't multiples of the pattern (e.g. graphic-only variants
sharing the same sprite across 3 consecutive ids for 1a/1b/2) simply
follow whichever range they fall in.

### Morphing into type 2 on landing (`ent_entdata`'s bug fix, and the 0xF0 flag trick)

A user-reported example (submap 2, `ent` 5) turned out to be a false
lead -- flags `0xf0` sitting on a plain type-1b walker in the stock
data does nothing for that entity, since type 1b's movement code
(`e_them_t1_action`/`_action2` in `e_them.c`) never reads `.flags` at
all. But a *different*, later report -- with exact source references
(`ents.c`'s entity-creation code and the `ENT_FLG_TRIGGERS` macro) --
pointed at a real, confirmed mechanic that the earlier investigation
missed:

For a type 1a/1b/2 entity (checked in the source as `e >= 0x09` on the
entity's runtime *slot* -- always true for these types, since they're
the only ones that land in the 3-slot walker/climber pool), having
**all four TRIG\* bits set at once** (`flags == 0xF0` exactly --
`ENT_FLG_MORPH_TO_TYPE2` in `xrick_marks.h`) does something entirely
unrelated to their usual "wake up a sleeping trap" meaning: at creation,
`sprbase` (normally set to the entity's own resting sprite) gets
overwritten with its own `sni` (`step_no_i`) field instead, reinterpreted
as a raw sprite index. `sni` is otherwise dead data for these types --
their AI never reads it, only a type-3 trap's sleep/wake sequence does
-- so it's effectively a free byte the original developers repurposed
to stash a *different* sprite to switch to. Critically, `sprite` (what's
actually drawn) is set separately at creation and only gets
recalculated from `sprbase` when `e_them_t1_action2()` detects landing
-- so the entity keeps drawing its own (type 1) sprite while airborne,
then switches the instant it touches the ground. The source itself
flags this as mysterious (`/* FIXME what is this? ... Why? What is the
point? */`) and names the visible in-game result: *"the falling guy on
the right on submap 3: it changes when hitting the ground."*

Confirmed directly against the stock binary: submap 3 has exactly one
mark with `flags == 0xF0` -- a type-1a `ent` 4 -- matching that comment
precisely.

**In Sprite Tools**, a type 1a/1b/2 sprite's flags row is now a single
**"Type 2 on landing"** checkbox instead of exposing all 8 raw bits --
matching how the user asked for this to be simplified. Checking it sets
`flags` to exactly `0xF0`; unchecking clears it to `0`. Its tooltip
shows which sprite index (`sni`) it'll switch to on landing. If a mark
already has some *other* nonzero `flags` value (leftover data, harmless
either way for these types), a note says so rather than silently
overwriting or hiding it.

**Also fixed while investigating this**: `entDataTable` (the compiled-in
sprite/`sni` lookup used for rendering and this feature) had 76 entries;
the real `ENT_NBR_ENTDATA` is `0x4a` = **74** (confirmed in `ents.h`).
The extra 2 came from misreading `dat_ents.c`'s two commented-out dead
rows as live data, which gave entities 67/68 a bogus sprite index and
shifted every entity from 69 on by 2 -- both silently wrong for several
sprites actually placed in the stock level (verified: ids 67-73 are used
by real marks). Fixed by regenerating the table directly from the
source, this time keeping `sni` too (needed for the feature above).

### Behavior flags (`flags` checkboxes in Sprite Tools)

Each sprite's "flags ->" row exposes the real `mark_t.flags` byte as
seven checkboxes:

| Checkbox     | Bit    | Meaning |
|--------------|--------|---------|
| `Rick`       | `0x80` | (type 3 only) wakes up when Rick walks into the trigger box |
| `Stop`       | `0x40` | (type 3 only) wakes up when Rick performs his "stop" move over the trigger box |
| `Bullet`     | `0x20` | (type 3 only) wakes up when a bullet hits the trigger box |
| `Bomb`       | `0x10` | (type 3 only) wakes up when a bomb hits the trigger box |
| `Once`       | `0x01` | plays once and stays gone, instead of looping/respawning |
| `LethalWake` | `0x08` | lethal to Rick as soon as it wakes up (`LETHALI`) |
| `LethalLoop` | `0x04` | lethal to Rick when it restarts a loop (`LETHALR`) |
| `StopsRick`  | `0x02` | this entity physically blocks Rick, like a solid moving block (`STOPRICK`) |

The four TRIG* checkboxes are what `e_them_t3_action` (type 3, traps)
checks every frame while the entity is asleep -- **at least one of them
must be on for a trap to ever fire.** They're meaningless for types
1a/1b/2 (those move on their own and never consult `flags`).

**Newly placed sprites default sensibly:** placing an entity with id
>= 0x18 (a trap) automatically turns on the `Rick` flag and points its
trigger point at the tile you placed it on, since "fires when Rick walks
over it" is by far the most common case. Placing a walker (1a/1b/2)
leaves all flags off, since they're unused. You can still untick `Rick`
and tick a different TRIG* combination (or several at once) for traps
that should instead react to a bullet, a bomb, or Rick's stop move.

### Trigger point (`trigCol` / `trigRowOffset`)

Decoded from the `lt` byte: `trigCol` is a tile column (0-31), and
`trigRowOffset` (0-7) is added to the sprite's own row to get the
trigger's row -- see `markTriggerRow()`. This is where the game checks
for the TRIG* conditions above (type 3 only); for type "1a" walkers,
`trigCol` is reinterpreted as a patrol distance instead (see above), and
for 1b/2 it's unused. The map overlay draws a small red crosshair
connected to the entity whenever the trigger point differs from the
entity's own position, so you can see and adjust the relationship
directly.

## The 3-slot limit on walkers/climbers (why a placed entity can silently not appear)

Confirmed in `ents.c` (`ent_creat2()`): entities of type 1a, 1b, and 2
(`ent` 4-15 -- see "Enemy types" above) all draw from the **same pool of
only 3 slots** (array indices 9, 10, 11) when the real engine activates
them as Rick scrolls into range. `ent_creat2()` simply returns failure
if all 3 are taken, and the caller `continue`s -- **the entity is
silently skipped, with no error, no fallback, nothing drawn.** This is a
hard constraint of the original game engine, not a bug in this editor;
the original 47 stock submaps were authored keeping this in mind (rarely
more than 2-3 such entities close together).

Sprite Tools now flags this: any submap header showing more than 3
entities with `ent` 4-15 turns orange, with an expandable warning giving
the exact count. Note the count is **per submap**, but the real
activation window can pull in entities from just before/after a submap
boundary too, so even exactly-3-per-submap can occasionally still
collide with a straggler from the previous screen not yet gone -- if a
walker/climber still doesn't appear despite this warning being clear,
try spacing it further from its neighbors (a different row/submap) or
removing one of the others nearby.

This pool is entirely separate from the one type-3 traps and
boxes/bonuses use (`ent_creat1()`, 5 slots, indices 4-8) -- placing
extra traps doesn't compete with walkers/climbers for slots, and vice
versa.

## UI polish: stable headers, mode-scoped windows, sprite draw order

- **Deleting a sprite (or a screen connection) no longer collapses its
  submap section.** The CollapsingHeader's visible label included the
  item count ("Submap 2 (3 sprites)"), and ImGui derives a widget's
  identity from its label text by default -- so deleting an item changed
  the count, which changed the label, which made ImGui treat it as a
  brand new (closed) header instead of the one you had open. Fixed by
  giving each header a `"...visible text...###submapN"` label: ImGui
  splits on `###` and only uses the part after it for identity, so the
  visible text can change freely (count, warning suffix) while the
  header's open/closed state stays tied to the submap index alone.
- **Block Palette and Sprite Tools are now mode-scoped**: Block Palette
  only exists while canvas mode is Block, Sprite Tools only while it's
  Sprite -- switching modes closes the one you're leaving and opens the
  one you're entering, instead of both permanently cluttering the
  screen regardless of what you're doing.
- **The "Tools" window is gone.** Its zoom readout, cursor
  coordinates, and Fill-selection button moved into the top bar (after
  Reset zoom); its keyboard-shortcut reference text is superseded by
  the Controls table in this doc; "Clear selection" and "Reset view"
  were dropped as no longer earning a dedicated window (Del/Backspace
  still clears the selection).
- **Sprites no longer draw on top of every window.** They were drawn
  with `ImGui::GetForegroundDrawList()`, which renders after (i.e. on
  top of) every ImGui window by design -- fine for things that must
  always be visible (like a global tooltip), wrong for a canvas overlay
  that should stay under the palette/tools windows the same way the map
  tiles do. Switched to `ImGui::GetBackgroundDrawList()`, which renders
  right after the SDL-drawn map and before any window, so the layering
  is now tiles -> sprites -> windows, as expected.
- **"Submap / Block / Sprite" canvas modes**, renamed from "Select" now
  that it does something concrete: it's the mode for **Screen
  Connections**, which is now mode-scoped the same way Block Palette and
  Sprite Tools already were.
- **"Submap N" labels drawn directly on the canvas**, at each submap's
  top boundary, in every canvas mode -- no need to open Screen
  Connections just to see which submap a given area belongs to.

## Investigation: why a walker/climber can silently "turn into a corpse" mid-patrol

Prompted by a stock entity (submap 2, `ent` 5 at row 192+1, col 24) that
has flags `0xf0` (all four TRIG* bits) set, and visibly changes into a
different-looking sprite when it reaches a certain spot while walking --
looking exactly like it "transforms into another enemy."

**The flags are a red herring.** `ent` 5 is a type-1b entity (chaser
walker -- see "Enemy types" above), and `e_them_t1_action2()` /
`e_them_t1_action()` in `e_them.c` never read `.flags` at all for this
type -- only `latency`, `offsx`, and `step_count`. The `0xf0` sitting in
the stock data for this entity is simply inert, unread leftover data
(same conclusion as the earlier "why is `ent` 4's trigger not really a
trigger" investigation, one flag byte over). This is exactly why the
per-entity flags UI is now hidden for anything that isn't a type-3 trap
(see the UI-polish section below) -- showing controls that do nothing
for a given entity type invites exactly this kind of false lead.

**The real cause is `e_them_gozombie()`.** Confirmed in `e_them.c`:
while a type-1a/1b walker moves (falling or walking horizontally), it
checks the *environment flags of the ground tile it's about to step
onto* via `u_envtest()`; if that tile is flagged `MAP_EFLG_LETHAL`, it
immediately calls `e_them_gozombie()` -- which swaps its entity type to
`0x47` (a distinct "zombie"/corpse entity with its own sprite and a
falling-backward animation), plays a death sound, and awards score. This
*looks* exactly like "the enemy turned into a different enemy" because,
mechanically, it did: its `.n` (entity type) really does change at
runtime, and the zombie state renders as a completely different sprite.
It's a per-**tile** hazard flag -- lava, spikes, that kind of thing --
tripped by a walker stepping onto it, not by anything in the sprite's
own placement data.

**This tile-hazard data (`map_eflg`) was, at the time of that
investigation, a separate table this editor didn't decode or expose at
all -- see "Tile hazard flags" below for the editor support this led to.

## Docked tool panel

Block Palette, Screen Connections, and Sprite Tools -- already
mutually exclusive by canvas mode -- now all render in the same fixed
panel: docked to the right edge, full height under the top bar, not
movable or resizable (`ImGuiWindowFlags_NoMove | NoResize | NoCollapse`,
position/size set unconditionally every frame instead of only
`ImGuiCond_FirstUseEver`). There was nothing to gain from letting them
float independently since only one is ever visible at a time; fixing
the geometry removes a whole category of "where did that window go"
friction, especially since they used to default to small, overlapping
floating positions.
## Tile hazard flags (`map_eflg_c`)

Editable in the **Tile Editor** window (see below) -- all 8 bits, per
tile. This is per-**tile** (0-255), per-**bank** data -- NOT per-block
and NOT per-submap: it's a fixed property of a tile *graphic*, compiled
into the game, entirely separate from a level's own
`map_bnums`/`map_blocks`/marks. Editing it changes what a given tile
graphic *does* everywhere it's used across the whole bank, not just one
placed block. See `src/xrick_eflg.h` for the implementation; the bit
meanings are exactly as documented in `maps.h`:

| Checkbox | Bit | Meaning |
|---|---|---|
| Solid | `0x40` | Can't walk/fall through |
| Lethal | `0x04` | Kills an entity that touches it -- the corpse-transform trigger from the investigation above |
| Climb | `0x02` | Entities can climb here |
| Vert | `0x80` | Vertical move only (usually paired with Climb) |
| WayUp | `0x10` | Solid except when moving up through it (jump-through platform) |
| SuperPad | `0x20` | Solid, but bounces entities skyward |
| Fgnd | `0x08` | Foreground -- drawn in front of / hides entities |
| Bit01 | `0x01` | Undocumented in the original source -- exposed raw |

**Storage is 8 run-length ranges per bank, already 100% full in the
stock data.** Confirmed by decoding the real binary: each bank's 256
per-tile bytes are packed as exactly 8 `(count, value)` pairs (16 bytes
per bank, 32 total -- `map_eflg_c[MAP_NBR_EFLGC]`, `MAP_NBR_EFLGC=0x20`),
and the stock table already uses all 8 ranges in *both* banks. This is
the same class of fixed-capacity constraint as Screen Connections'
153-slot table or a level's 523-sprite cap, but unlike those, **the UI
no longer exposes the ranges directly** -- editing is per-tile (see Tile
Editor below), and a range only gets split behind the scenes when a
tile's flags start to differ from its neighbors'. There used to be a
dedicated range editor (rows with Split/Merge buttons) in the Block
Palette; it was removed once per-tile editing landed in the Tile Editor,
since maintaining both was redundant and the per-tile view is the more
natural way to work tile-by-tile. The Tile Editor shows a running
"N of 8 tile ranges used" count so the limit stays visible without the
old row-editor's complexity -- **Save**/**Patch xrick binary...** will
refuse if an edit pushes a bank's range count past 8 (same hard
capacity limit as before, just discovered at save/patch time now instead
of being impossible to reach by construction).

**Persistence:** round-trips through `.map` files (a new "RKM5" format,
extending RKM4 with 256 bytes per bank appended at the end -- older
RKM4/RKM2/RKMP files still open fine, they just leave the hazard flags
at whatever they currently are, default or previously loaded/edited)
and through **File > Patch xrick binary...** (patches the `map_eflg_c`
symbol alongside level layout/connections/sprites). Also importable
directly from a chosen binary via **File > Import connections, sprites,
tile hazards, tile graphics && blocks from xrick binary...**, alongside
the other data types.

Because every tile of a given graphic shares one hazard byte, there's no
per-**placement** editing here (unlike blocks/sprites) -- changing a
tile's flags affects that tile ID everywhere it appears on the map,
across every submap that uses this bank.

## Tile Editor (tile graphics + all 8 per-tile hazard flags)

A standalone window (**Tile Editor** checkbox in the top bar), independent
of canvas mode -- it doesn't touch the map at all, just the tile
*graphics* themselves (`tiles_data`) and the per-tile hazard flags
described above (`map_eflg_c`), both scoped to whichever single tile is
currently selected. See `src/tile_import.h`.

- **Grid** on the left: all 256 tiles of the selected bank (1 or 2, same
  restriction as the Block Palette -- bank 0 is unused padding). Click to
  select.
- **Detail panel** on the right: an enlarged preview, an **Import from
  image...** button, and all 8 hazard-flag checkboxes (Solid, Lethal,
  Climb, Vert, WayUp, SuperPad, Fgnd, Bit01 -- see the table above) for
  the selected tile, plus a "N of 8 tile ranges used" counter.
- **Import from image...** accepts PNG/BMP/TGA/JPG/GIF/PSD (via
  `stb_image`, vendored in `third_party/stb/`). Any image size works --
  it's resampled to 8x8 (box-filtered average per destination pixel, so
  it handles both up- and downscaling cleanly) and each resulting pixel
  is matched to the closest of the game's 16 colors (Euclidean distance
  in RGB space against `RED`/`GREEN`/`BLUE` in `mapdata.h` -- only
  indices 0-15, since a tile pixel is a single hex digit and can never
  reach the "cheat colors" 16-31). Verified round-trip-exact against
  `decode_tile()` (the same decoder used for on-screen rendering) with a
  dedicated test, `src/test_tile_import.cpp`.
- **Batch import...** (collapsible section above the grid, so it's
  available regardless of which single tile is selected -- you're
  picking a *destination range*, not editing one already-selected tile):
  give a **start tile** number, then choose one image. The image is
  sliced into a grid of *exact* 8x8 cells -- **no resampling** here,
  unlike the single-tile import above -- and each cell becomes one tile,
  walked left-to-right then top-to-bottom, starting at the given tile
  and incrementing by one per cell (`importTilesBatchFromImage()` in
  `tile_import.h`). Stops at tile 255 (doesn't spill into another bank);
  if the image isn't an exact multiple of 8 pixels in either dimension,
  the leftover row/column of pixels is ignored rather than rejecting the
  whole image. A summary (grid size detected, how many tiles landed
  where, anything skipped/ignored and why) is shown in a popup once
  done. **Selecting a tile in the grid also sets the batch start tile**
  to that tile's number (still freely editable in the field afterward --
  it's just a convenient default, e.g. select tile 40 then batch-import
  starting there without retyping "40"). Verified against `decode_tile()`
  with a dedicated test that
  checks for bleed between adjacent source tiles (a marker pixel in one
  corner of each source tile, decoded back and checked pixel-by-pixel),
  plus the tile-255 overflow case, `src/test_tile_batch_import.cpp`.
- Editing a tile's graphic here (single or batch) immediately rebuilds
  that bank's tile *and* block atlas textures (blocks are a baked
  snapshot of the tile atlas, not a live view of it -- see
  `rebuildBankAtlases()` in `editor.cpp`), so changes show up right away
  in the Tile Editor, the Block Palette, and the map itself. Hazard-flag
  edits don't touch any texture (they're gameplay data, not graphics) --
  only `st.dirty`.
- **Border color-codes the hazard flags** on every tile thumbnail (this
  grid and preview, plus the Block Editor's 4x4 cell grid and tile
  picker below) -- `drawTileHazardBorder()` in `editor.cpp`, checked in
  priority order so only one color shows even if several bits are set:
  Solid -> gray, Lethal -> red, Climb -> green, WayUp -> the plain
  default blue border with a gray dashed overlay (WayUp shows up on
  almost every platform in a real level, so a flat color there would be
  as visually loud as Lethal); anything else (Vert/SuperPad/Fgnd/Bit01
  alone, or no flags) stays plain blue.

**Persistence:** tile graphics (banks 1 and 2 only -- bank 0 is never
edited, so it isn't stored) round-trip through `.map` files (a new
"RKM6" format, extending RKM5 with `2 * 256 * sizeof(tile_t)` bytes of
raw `tiles_data` appended at the end -- older files still open fine,
graphics are just left as whatever they currently are) and through
**File > Patch xrick binary...** (patches the `tiles_data` symbol --
all 3 banks worth, since bank 0's compiled-in default still needs to
reach the target binary -- alongside level layout/connections/sprites/
hazard flags). Also importable directly from a chosen binary via
**File > Import connections, sprites, tile hazards, tile graphics &&
blocks from xrick binary...**. Both the `.map` round-trip and the ELF
patch + reload round-trip are covered by dedicated tests
(`src/test_tile_import.cpp` covers the image-import/decode path only;
the `.map` and ELF-patch round-trips were verified with standalone
throwaway test programs during development -- worth turning into
proper `src/test_*.cpp` files if this area gets touched again).

## Block Editor (which tile goes where within a block -- `map_blocks`)

A second standalone window (**Block Editor** checkbox in the top bar),
independent of both the map canvas and the Tile Editor's own selection.
Composes blocks out of tiles by editing `map_blocks[256][16]` -- which
tile fills each of a block's 16 cells (4x4 grid, column `i%4`/row `i/4`,
same layout `build_block_atlas()` and the original `drawblock()` use).
See `src/mapdata.h` (the array itself, now non-`const` so it can be
edited) and `src/tile_import.h` (`loadBlocksFromXrickBinary()`).

Unlike tile graphics/hazard flags, block composition is **shared across
both tile banks** -- a block's cell layout doesn't depend on which
bank's graphics render it (see `build_block_atlas()` in
`tiles_render.h`: same `map_blocks` indices, called once per bank with
that bank's tile atlas). So there's only one bank selector here and it's
preview-only, picking which bank's tile graphics to show while editing,
not a second copy of the data.

- **Grid** on the left: all 256 blocks (same block atlas as the Block
  Palette). Click to select.
- **Detail panel** on the right: an editable 4x4 grid showing the
  selected block's current tile composition -- click a cell to select
  it (highlighted), then click a tile in the picker below to place it
  there. Placing a tile auto-advances to the next cell (stops at the
  last one, doesn't wrap) so composing a block left-to-right,
  top-to-bottom is a quick series of clicks; click an earlier cell
  directly for an out-of-order fix without disturbing the rest.
- **Clear block (all tile 0)** resets all 16 cells of the selected
  block at once.
- **Swap with...** arms swap mode (button label changes to "Cancel
  swap"); the next block clicked in the grid on the left swaps its
  entire tile composition with the selected block's
  (`std::swap(map_blocks[a], map_blocks[b])` -- swaps the two `int[16]`
  arrays in place, verified not to disturb any other block with a
  dedicated throwaway test during development). Candidate blocks are
  tinted amber while swap mode is armed; clicking the already-selected
  block, or toggling the button again, cancels without swapping.
- Cell/picker thumbnails are border color-coded by hazard flags too --
  see the Tile Editor section above for what each color means.
- Any edit here rebuilds *both* banks' block atlas textures right away
  (`rebuildBlockAtlasOnly()` -- cheaper than `rebuildBankAtlases()`
  since the tile atlases themselves didn't change, just how blocks are
  assembled from them), so changes show up immediately in the Block
  Editor, the Block Palette, and the map itself.

**Persistence:** `map_blocks` round-trips through `.map` files (a new
"RKM7" format, extending RKM6 with `0x100 * 16 * sizeof(int)` bytes of
raw `map_blocks` appended at the end -- older files still open fine,
composition is just left as whatever it currently is) and through
**File > Patch xrick binary...** (patches the `map_blocks` symbol --
the real engine stores each entry as a single byte, `block_t = U8[16]`
in the original source, confirmed against `dat_maps.c`'s initializer,
so this editor's wider `int` copy is packed back down via
`map_blocks_as_bytes()` in `xrick_patch.h`, same "widen for the editor,
pack for the binary" pattern as `map_bnums_as_bytes()`). Also importable
directly from a chosen binary via **File > Import connections, sprites,
tile hazards, tile graphics && blocks from xrick binary...**
(`loadBlocksFromXrickBinary()`). Both the `.map` round-trip and the ELF
patch + reload round-trip were verified with standalone throwaway test
programs during development, same as tile graphics above.

## Sprite Editor (sprite graphics -- `sprites_data`)

A third standalone window (**Sprite Editor** checkbox in the top bar),
independent of canvas mode. Same shape as the Tile Editor above (single
image import with resampling + exact-crop batch import), applied to
`sprites_data` instead of `tiles_data`. See `src/sprite_import.h`
(mirrors `src/tile_import.h` closely -- shares `readWholeFile()` and the
stb_image setup via `#include "tile_import.h"`).

Two differences from tiles, both consequences of the sprite format
itself (`sprite_t` = 21 rows x 4 `U32`, i.e. 32x21 pixels, decoded by
`decode_sprite()` in `sprites_render.h`) rather than editor design
choices:
- **No bank split** -- `sprites_data[213]` is one flat table, so there's
  no bank selector anywhere in this window.
- **No hazard flags** -- sprites don't have a `map_eflg_c`-style
  per-graphic flags byte; sprite behavior comes from the marks/triggers
  system (already covered by the existing "Sprite Tools" window), a
  different and unrelated thing.
- **Palette index 0 means transparent**, not an opaque color (unlike
  tiles, where index 0 is real opaque black) -- `draw_sprite()` skips
  writing a pixel when its nibble is 0. So import quantizes opaque
  source pixels to indices 1-15 only (never 0), and any source pixel
  with alpha below 128 becomes transparent (index 0) instead of being
  color-matched. The detail panel's preview is drawn over a checkerboard
  so transparency is visible instead of blending into the window
  background.

- **Grid** on the left: all 213 sprites. Click to select.
- **Detail panel** on the right: an enlarged preview (on checkerboard),
  and **Import from image...** -- resampled to 32x21 (box-filtered
  average per destination pixel, alpha channel included in the average),
  same quantization as above.
- **Batch import...** (collapsible section above the grid, same reasoning
  as the Tile Editor's): a **start sprite** number, then one image,
  sliced into *exact* 32x21 cells (no resampling), left-to-right then
  top-to-bottom, one sprite per cell. Stops at sprite 212 (the last one
  -- no wraparound); a leftover partial row/column of pixels (image size
  not an exact multiple of 32x21) is ignored rather than rejecting the
  image. Selecting a sprite in the grid also sets the batch start sprite
  to that number, same convenience as the Tile Editor. A summary popup
  shows what happened.
- Any import (single or batch) rebuilds the sprite atlas texture right
  away (`rebuildSpriteAtlas()` in `editor.cpp`), so changes show up
  immediately here and in the map canvas/Sprite Tools windows that also
  draw real sprite graphics.

Verified with a dedicated test, `src/test_sprite_import.cpp`: single-
sprite round-trip through `decode_sprite()` including a transparent
pixel, batch import across a 2-sprite-wide image checked for bleed
between the two sprites, and the end-of-table overflow case -- same
methodology as the tile import tests, using a hand-written TGA encoder
(32bpp with a native alpha channel, simpler to get exactly right for a
test than a BMP with `BI_BITFIELDS` masks, which turned out to need more
care than expected during development).

**Persistence:** `sprites_data` round-trips through `.map` files (a new
"RKM8" format, extending RKM7 with `SPRITES_NBR_SPRITES * sizeof(sprite_t)`
bytes of raw `sprites_data` appended at the end -- older files still
open fine, sprite graphics are just left as whatever they currently are)
and through **File > Patch xrick binary...** (patches the `sprites_data`
symbol directly -- no widen/pack step needed, unlike `map_bnums`/
`map_blocks`, since the in-memory type already matches the real
engine's byte-for-byte). Also importable directly from a chosen binary
via **File > Import connections, sprites, tile hazards, tile graphics,
blocks && sprite graphics from xrick binary...**
(`loadSpritesFromXrickBinary()` in `sprite_import.h`). Both the `.map`
round-trip and the ELF patch + reload round-trip are covered by
dedicated tests, `src/test_sprite_persist.cpp` and
`src/test_patch_sprites.cpp` (same methodology as the tile/block
persistence tests -- a synthetic ELF32 target for the latter).

## Investigation: "the last level is all shifted" (submap 38 onward)

`bnum` turns out to have **two different meanings** depending on
context, both confirmed against the real source and both independently
tested -- this is what caused the confusion:

- For marks/connections positioning (`ents.c`, `maps.c`'s `rowout`/
  `rowin` handling): `submapStartRow(bnum) = bnum / 2` gives an
  **absolute TILE row**.
- For actual block *rendering* (`maps.c`'s `map_expand()`): `bnum` is
  used **directly as a flat index into `map_bnums`**, no division at
  all -- `pbnum = bnum + ((2*map_frow) & 0xfff8)`, then
  `map_blocks[map_bnums[pbnum]]`. The inner loop reads 8 consecutive
  *flat* entries per on-screen row and simply increments `pbnum`
  linearly -- it does **not** respect this editor's fixed 8-column grid
  at all. When `bnum` isn't a multiple of 8, a submap's on-screen rows
  each straddle two of this editor's block-rows (e.g. columns 2-7 of
  one row plus columns 0-1 of the next), which the real engine handles
  fine, but which this editor's own block-grid canvas and "Submap N"
  labels have no way to represent (they assume every submap starts at
  column 0 of some row).

Confirmed empirically: submaps 0-37 all have `bnum % 8 == 0` (clean
block-row starts). Starting exactly at **submap 38** (bnum 6794) through
**submap 46** (the last one), all 9 have `bnum % 8 == 2` -- consistently,
not randomly. The two blocks right before submap 38's start (indices
6792, 6793) are genuinely unused (value 0, confirmed never read by any
submap) -- a 2-block gap the original developers apparently didn't
bother block-aligning. Every submap from 38 onward is packed
immediately after the previous one with no further gaps, so they all
inherit that same +2 offset in a chain.

**This isn't a gameplay bug** -- simulated `map_expand()`'s exact
sliding-window read for each of the 9 submaps' full 88-block on-screen
window, byte for byte, before and after the fix below: identical either
way, since the real engine's flat read doesn't care about the offset.
It only confuses *this editor's* block-grid-aligned rendering.

**Fix**: Screen Connections' new **Diagnostics** section (**"Check for /
fix misaligned block run"**) finds the earliest misaligned submap,
verifies the padding blocks right before it are genuinely unused (only
proceeds if they are -- refuses with a clear message otherwise, rather
than guessing), then shifts that submap and every later one back by the
same 2 blocks, absorbing the gap. Verified (see
`test_xrick_levels.cpp`) that this changes no mark's or connection's
absolute row (they're independent of `bnum`, so nothing moves on
screen) and produces byte-for-byte the same in-game block window for
every affected submap, both before and after -- it only fixes the
alignment this editor's own canvas relies on. Save or re-patch
afterward to keep it.

## Investigation: freshly placed sprites silently never appearing in-game

Reported across several sessions: newly placed enemies (any type) simply
never showed up in-game, sometimes with a one-frame flash of the wrong
shape noticeably higher on screen before vanishing. Root cause confirmed
directly in the source, `ents.c`'s `ent_actvis()`:

```c
/*
* go through the list and find the first mark that
* is visible, i.e. which has a row greater than the
* first row (marks being ordered by row number).
*/
for (m = map_submaps[game_submap].mark;
     map_marks[m].row != 0xff && map_marks[m].row < frow;
     m++);
```

This is a **forward-only linear scan that assumes each submap's mark
chain is sorted by ascending row**, and the scan index is carried
forward between calls rather than restarting from the beginning each
time. `repackMarks()` (the function that flattens this editor's own
per-submap sprite lists into that raw chain, used by both `.map`
patching and `File > Patch xrick binary...`) was writing marks in
**insertion order** instead -- so a sprite placed by clicking the canvas
(always appended to the end of that submap's list) that happened to
land at a *lower* row than something already in the list ended up
out of order in the exported chain. The scan above permanently skips
past such an entry without ever spawning it -- exactly matching a
sprite that never appears. The occasional one-frame flash-then-vanish
some sessions reported is consistent with an out-of-order entry getting
activated at the wrong scroll position (hence appearing too high) and
then immediately failing the entity's own out-of-bounds check.

Confirmed against a real report's save file: 3 user-placed sprites (all
`ent` 4, rows 92/100/108) ended up appended after an existing row-164
sprite in the file -- out of order exactly as described above.

**Fixed**: `repackMarks()` now stably sorts each submap's marks by
`rowAbs` immediately before flattening them, regardless of what order
they were placed or edited in. Verified with a dedicated regression test
(`test_xrick_marks.cpp`) reproducing this exact scenario. This is a
one-line, purely-at-export-time fix -- nothing about how sprites are
placed, displayed, or edited in this editor changes; only the order
they're written to the raw chain does.

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

| Action | Effect |
|---|---|
| Left click (hold = paint) | Place the selected block |
| Right click (hold = keep picking) | Eyedropper: selects the block under the cursor into the palette |
| Shift + drag | Rectangle selection |
| Del / Backspace | Clear the selection (sets it to block 0) |
| `F` | Fill the selection with the selected block |
| Mouse wheel, or `+`/`-` | Zoom (centered on cursor for the wheel, on screen center for the keys) |
| Middle-drag, or arrow keys Up/Down | Pan (fine, continuous) |
| Left/Right, Page Up/Page Down | Fast scroll: jumps 20 cells per key press (repeats while held) |
| `1` / `2` | Tile bank 1 / 2 |
| `S` | Toggle sprite placement mode on/off |
| Escape | Cancel the current selection |

## File menu (top menu bar only -- no separate dockable window)

- **Open...** / **Save** / **Save As...** -- this editor's own `.map`
  format (see `src/mapfile.h`): a small header (magic + version + element
  count) followed by the 8152 `map_bnums` values as-is. Loading refuses any
  file whose element count doesn't match. A `*` next to the file name means
  there are unsaved changes.
- **Patch xrick binary...** -- injects the level currently open in the
  editor directly into a compiled xrick executable. See below.

### How the xrick patch works

The real xrick engine stores the level as a global C array called
`map_bnums` (same name, same values, just packed as one byte per entry
instead of the editor's 4-byte `int`, and same for `map_blocks`/
`tiles_data` -- this editor's data actually *comes from* xrick's own
`dat_tilesST.c`, so it lines up exactly).

Given an **unstripped** xrick executable (ELF32, Linux), the patcher:

1. Parses the ELF symbol table to find `map_bnums` by name and computes
   where it lives in the file (works for any matching build -- the
   location isn't hard-coded to one specific binary).
2. Checks its size matches exactly (8152 bytes); refuses to touch anything
   if it doesn't look like a compatible build.
3. Writes the current level (each block index clamped to 0-255) over those
   bytes, in a **copy** of the file named `<name>_patched` next to the
   original -- the input executable is never modified -- and copies over
   its file permissions (so the output stays executable).

Tested against a real xrick 20-year-old Linux/x86 build: the patched
region matches byte-for-byte and every other byte in the file is left
untouched (see `src/test_xrick_patch.cpp`).

Only `map_bnums` (the level layout) is patched -- not `map_blocks` or
`tiles_data`, since this editor doesn't modify those.

## Files

- `src/editor.cpp` -- main editor (UI, input handling, rendering).
- `src/mapfile.h` -- `.map` save/load, isolated from the UI so it can be
  unit-tested on its own.
- `src/xrick_patch.h` -- ELF32 symbol lookup + xrick binary patching,
  likewise isolated from the UI.
- `src/test_mapfile.cpp` -- standalone round-trip test for the `.map`
  format (not part of the CMake build; compile/run manually: `g++
  -std=c++17 -Iinclude -Isrc src/test_mapfile.cpp src/dat_tilesST.c -o
  test_mapfile && ./test_mapfile`).
- `src/test_xrick_patch.cpp` -- standalone test for the xrick patcher,
  takes a real xrick binary path on the command line (not part of the
  CMake build): `g++ -std=c++17 -Iinclude -Isrc src/test_xrick_patch.cpp
  src/dat_tilesST.c -o test_xrick_patch && ./test_xrick_patch
  /path/to/xrick`.
- `src/tiles_render.h` -- tile decoding + atlas texture construction.
- `src/legacy_main.cpp.txt` -- old faithful SDL1-port version (bug-fixed,
  behavior-preserving), kept for reference, not compiled.

## Still ahead

Sprites and the rest of the engine: not addressed yet, as agreed.

## Screen connections (links between submaps)

Rick Dangerous has no horizontal scrolling: the level is a chain of fixed
screens ("submaps" in the engine), and moving to the next one is a
vertical transition -- walking off the top or bottom of the current
screen. Each submap owns a chain of links to other submaps; a link is
`{dir, row, target submap, target row}`.

**Coordinates are absolute**, in the same row numbering as everywhere
else in the editor (matches "Cell under cursor" in the Tools window) --
not the raw engine's per-submap-relative byte values. A link is shown as
one row (on the submap whose card it's listed under) connected to one row
on the target submap; `dir` (Down/Up) just says which way through it
counts as a trigger, direction isn't otherwise called out as
"entrance"/"exit" in the UI. Absolute row `submapStartRow(submap) = bnum
/ 8` marks where a submap's own data begins in the flat map grid.

**Loaded automatically at startup** (compiled into the program from a
stock xrick binary, `src/connections_default.h` -- same idea as the
default map data) and **saved/loaded with the map itself**: `.map` files
now include the full screen-connection graph (`File > Save` / `Open...`).
Older bnums-only `.map` files still open fine; they just leave the
connections as they currently are (default or previously loaded).

Per submap, in the **Screen Connections** window:
- **Tile bank** selector (1 or 2 -- same banks as the Block Palette;
  maps to the engine's internal `page` field).
- **Delete (disconnect)**: clears this submap's own links, and redirects
  any other submap's link that pointed to it to "end of world". Doesn't
  erase its tiles from the map (the underlying arrays are fixed-size --
  see below) -- just makes it unreachable through the level's flow.
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
32-tile map width) and a few bits of fine vertical offset. **Caveat**:
the exact pixel meaning of that fine offset, of `flags` (only the top
`MAP_MARK_NACT` bit is documented), and of `lt` (trigger info for
trap-like entities) isn't independently verified against actual game
behavior -- no source or running game was available to confirm them, so
treat those three as best-effort/raw, editable but not fully understood.
Row + tile-column placement follows the exact same proven pattern as
Screen Connections and should be reliable.

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
  `flags`/`lt`/fine-offset are set to 0 for newly placed sprites and can
  only be edited by adjusting the underlying value at the moment --
  no dedicated advanced-fields UI yet).

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
- The exact bit meaning of the rest of `flags` (only `MAP_MARK_NACT`,
  the top bit, is documented) -- still exposed raw, not decoded.
- `ent_entdata`'s source table has 76 entries; this project's actual
  xrick binary's `ent_entdata` symbol is sized for 74 -- a discrepancy
  between the provided source and that specific compiled binary that
  doesn't affect anything patched back into the binary (sprite art is a
  purely local editor/rendering aid, never written back), but is worth
  knowing about if the two are compared directly.

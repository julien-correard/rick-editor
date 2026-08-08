# RickEditor -- level editor (v3, Dear ImGui)

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
| Escape | Cancel the current selection |

**File** operations are available two ways: the dedicated **"File" window**
(Open / Save / Save As buttons, always visible, same style as the Block
Palette and Tools windows) and the **File menu** in the top menu bar.  Both
read/write a custom `.map` binary file (see `src/mapfile.h`): a small
header (magic + version + element count) followed by the 8152 `map_bnums`
values as-is. Loading refuses any file whose element count doesn't match
(prevents silently loading a corrupt or foreign file). A `*` next to the
file name means there are unsaved changes.

## Files

- `src/editor.cpp` -- main editor (UI, input handling, rendering).
- `src/mapfile.h` -- `.map` save/load, isolated from the UI so it can be
  unit-tested on its own.
- `src/test_mapfile.cpp` -- standalone round-trip test for the `.map`
  format (not part of the CMake build; compile/run manually if you touch
  the format: `g++ -std=c++17 -Iinclude -Isrc src/test_mapfile.cpp
  src/dat_tilesST.c -o test_mapfile && ./test_mapfile`).
- `src/tiles_render.h` -- tile decoding + atlas texture construction
  (unchanged from the previous version).
- `src/legacy_main.cpp.txt` -- old faithful SDL1-port version (bug-fixed,
  behavior-preserving), kept for reference, not compiled.

## Still ahead

Sprites and the rest of the engine: not addressed yet, as agreed.

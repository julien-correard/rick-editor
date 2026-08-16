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

namespace fs = std::filesystem;

static inline int mapIndex(int col, int row) { return col + row * MAP_COLS; }

// Nombre de cases franchies par appui sur Gauche/Droite/PageUp/PageDown
// (defilement rapide).
static const int FAST_SCROLL_CELLS = 20;

// Bank 0 is unused padding data (never shown in the UI); only banks 1 and
// 2 hold actual editable content, same as the previous editor's keys.
static const int FIRST_USABLE_BANK = 1;
static const int LAST_USABLE_BANK = 2;

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
    fs::path currentPath;      // empty if the map was never saved/loaded

    // Canvas mode: what a left-click on the map does. Submap = nothing
    // (just look around and inspect screen connections, e.g. to line
    // things up without risking an accidental edit) -- shows Screen
    // Connections; Block = paint/pick tiles (the old default) -- shows
    // Block Palette; Sprite = place/remove sprites (the old
    // spritePlacementMode) -- shows Sprite Tools. Each of the 3 side
    // windows is scoped to its matching mode, so only one is ever up.
    CanvasMode canvasMode = CanvasMode::Block;
    bool showGrid = false;         // always-available block-grid overlay (also on above a zoom threshold, see drawMap())

    // Sprites
    bool showSprites = true;       // overlay toggle -- can be turned off to focus on tile editing
    int selectedEnt = 4;           // entity type id to place (lowest observed in stock data)
    int hoverMarkSubmap = -1, hoverMarkIndex = -1; // nearest sprite under cursor, for right-click removal
};

// State for the standalone Tile Editor window -- separate from the map
// canvas entirely (own bank/selection, doesn't touch st.bank so picking
// a tile to edit never disturbs what's selected in the Block Palette).
struct TileEditorState
{
    bool open = false;
    int bank = 1;          // FIRST_USABLE_BANK..LAST_USABLE_BANK, same convention as st.bank
    int selectedTile = -1; // -1 = nothing selected yet
    int batchStartTile = 0; // "Batch import..." starting tile index
};

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

enum class DialogPurpose { OpenMap, SaveMap, PickXrickBinary, PickXrickBinaryForConnections, ImportTileImage, BatchImportTileImage, ImportSpriteImage, BatchImportSpriteImage };

struct FileDialog
{
    bool show = false;
    bool saveMode = false;
    DialogPurpose purpose = DialogPurpose::OpenMap;
    fs::path dir = fs::current_path();
    char filename[256] = "";
    std::string error;
    // Empty = show every regular file; otherwise only files whose
    // extension matches one of these (case-sensitive, includes the dot).
    std::vector<std::string> extFilter = {".map"};
};

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

static void drawMap(SDL_Renderer* renderer, SDL_Texture* blockAtlas, const EditorState &st, int viewportW, int viewportH)
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
    if (st.showGrid || st.cam.zoom >= 3.0f)
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
static bool renderFileDialog(FileDialog &fd, fs::path &outPath)
{
    if (!fd.show) return false;
    const char* title = fd.purpose == DialogPurpose::PickXrickBinary ? "Select xrick binary"
                       : fd.purpose == DialogPurpose::PickXrickBinaryForConnections ? "Select xrick binary"
                       : fd.purpose == DialogPurpose::ImportTileImage ? "Import tile image"
                       : fd.purpose == DialogPurpose::BatchImportTileImage ? "Batch import tile image"
                       : fd.purpose == DialogPurpose::ImportSpriteImage ? "Import sprite image"
                       : fd.purpose == DialogPurpose::BatchImportSpriteImage ? "Batch import sprite image"
                       : fd.saveMode ? "Save As" : "Open Map";
    ImGui::OpenPopup(title);

    bool confirmed = false;
    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    bool stillOpen = true;
    if (ImGui::BeginPopupModal(title, &stillOpen, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextDisabled("%s", fd.dir.string().c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("##filelist", ImVec2(0, 300), true))
        {
            if (fd.dir.has_parent_path() && fd.dir != fd.dir.root_path())
            {
                if (ImGui::Selectable("[..]"))
                    fd.dir = fd.dir.parent_path();
            }

            std::error_code ec;
            std::vector<fs::directory_entry> dirs, files;
            for (auto &e : fs::directory_iterator(fd.dir, ec))
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
                    fd.dir = d.path();
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
                fs::path p = fd.dir / name;
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
static void doSave(EditorState &st, FileDialog &fd, const ConnectionsData &conn, const MarksData &sprites, const EflgData &eflg)
{
    if (st.currentPath.empty()) { requestSaveAs(fd, st.currentPath); return; }
    std::string err;
    if (saveMapFileWithSprites(st.currentPath, conn, sprites, eflg, err)) st.dirty = false;
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
//   Solid -> gray, Lethal -> red, Climb -> green, WayUp -> blue with a
//   gray hatch overlay (blue is also the plain default border when none
//   of these four are set -- Vert/SuperPad/Fgnd/Bit01 don't get their
//   own color here). WayUp keeps the default blue underneath and adds
//   dashes on top rather than a flat color, since a real xrick level
//   uses WayUp constantly on ordinary-looking platforms and a solid
//   color there would be visually as loud as Lethal.
static void drawTileHazardBorder(ImDrawList *dl, ImVec2 rmin, ImVec2 rmax, uint8_t flags)
{
    const ImU32 gray  = IM_COL32(150, 150, 150, 255);
    const ImU32 red   = IM_COL32(220, 60, 60, 255);
    const ImU32 green = IM_COL32(70, 200, 90, 255);
    const ImU32 blue  = IM_COL32(80, 140, 240, 255);
    const float thickness = 2.0f;

    ImU32 color = blue;
    bool hatch = false;
    if (flags & EFLG_SOLID) color = gray;
    else if (flags & EFLG_LETHAL) color = red;
    else if (flags & EFLG_CLIMB) color = green;
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
    BlockEditorState blockEditor;
    SpriteEditorState spriteEditor;
    FileDialog fileDialog;
    ConnectionsData connections = defaultConnections();
    MarksData sprites = defaultMarks();
    EflgData eflg = defaultEflg();
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
            bool kbWantedByUI = io.WantCaptureKeyboard;

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
                    else if (event.button.button == SDL_BUTTON_RIGHT)
                    {
                        // Right button = eyedropper: pick the block under
                        // the cursor into the palette selection.
                        st.picking = true;
                        if (cellValid(col, row)) st.selectedBlock = map_bnums[mapIndex(col, row)];
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
                }
            }

            if (!kbWantedByUI && event.type == SDL_KEYDOWN)
            {
                switch (event.key.keysym.sym)
                {
                    case SDLK_ESCAPE:
                        st.sel.active = false;
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
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open...", "Ctrl+O")) requestOpen(fileDialog);
                if (ImGui::MenuItem("Save", "Ctrl+S")) doSave(st, fileDialog, connections, sprites, eflg);
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
                if (ImGui::MenuItem("Import connections, sprites, tile hazards, tile graphics, blocks && sprite graphics from xrick binary..."))
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
            if (!st.currentPath.empty())
                ImGui::Text("  %s%s", st.currentPath.filename().string().c_str(), st.dirty ? " *" : "");

            ImGui::Separator();
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
                ImGui::SameLine();
            }

            ImGui::Separator();
            ImGui::Checkbox("Tile Editor", &tileEditor.open);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit individual tile graphics (import from image) and their hazard flags -- independent of the map canvas");
            ImGui::SameLine();
            ImGui::Checkbox("Block Editor", &blockEditor.open);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Compose blocks out of tiles (map_blocks) -- independent of the map canvas");
            ImGui::SameLine();
            ImGui::Checkbox("Sprite Editor", &spriteEditor.open);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit sprite graphics (import from image, single or batch) -- independent of the map canvas");
            ImGui::SameLine();

            ImGui::Separator();
            ImGui::Checkbox("Grid", &st.showGrid);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the block grid (one square = one block) -- G");
            ImGui::SameLine();

            ImGui::Separator();
            if (ImGui::SmallButton("-")) zoomAt(st, viewportW / 2.0f, viewportH / 2.0f, 1.0f / 1.25f);
            ImGui::SameLine();
            ImGui::Text("%.0f%%", st.cam.zoom * 100.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) zoomAt(st, viewportW / 2.0f, viewportH / 2.0f, 1.25f);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset zoom")) { st.cam.zoom = 2.0f; }

            ImGui::Separator();
            if (ImGui::SmallButton("Fill selection (F)")) clearSelection(st, st.selectedBlock);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill the current rectangle selection with the selected block");

            ImGui::Separator();
            {
                int mx, my; SDL_GetMouseState(&mx, &my);
                int col, row;
                screenToCell(st, (float)mx, (float)my, col, row);
                if (cellValid(col, row))
                    ImGui::Text("col %d, row %d (index %d) -- tile row %d", col, row, mapIndex(col, row), screenToTileRow(st, (float)my));
                else
                    ImGui::Text("out of map -- tile row %d", screenToTileRow(st, (float)my));
            }

            ImGui::EndMainMenuBar();
        }

        // Shared docked-panel geometry for the 3 mutually-exclusive tool
        // windows below (Block Palette / Screen Connections / Sprite
        // Tools -- exactly one is ever visible, gated by canvas mode).
        // Fixed to the right edge, full height under the menu bar, not
        // movable or resizable -- there's nothing to arrange, so letting
        // them float/overlap the canvas was just friction.
        const float toolPanelW = 430.0f; // wide enough for 8 block thumbnails/row with room for the scrollbar
        const float topBarH = ImGui::GetFrameHeight();
        const ImVec2 toolPanelPos((float)viewportW - toolPanelW, topBarH);
        const ImVec2 toolPanelSize(toolPanelW, (float)viewportH - topBarH);
        const ImGuiWindowFlags toolPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

        fs::path chosenPath;
        if (renderFileDialog(fileDialog, chosenPath))
        {
            std::string err;
            switch (fileDialog.purpose)
            {
                case DialogPurpose::SaveMap:
                    if (saveMapFileWithSprites(chosenPath, connections, sprites, eflg, err)) { st.currentPath = chosenPath; st.dirty = false; }
                    else fileDialog.error = err;
                    break;
                case DialogPurpose::OpenMap:
                    if (loadMapFileWithSprites(chosenPath, connections, sprites, eflg, err))
                    {
                        st.currentPath = chosenPath; st.dirty = false; st.sel.active = false;
                        // Harmless no-op if this file predates RKM6/RKM7/
                        // RKM8 (tile graphics/block composition/sprite
                        // graphics unchanged) -- always rebuilding is
                        // simpler than tracking whether they actually
                        // moved, and this only runs once per Open.
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 1);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 2);
                        rebuildSpriteAtlas(renderer, spriteAtlas);
                    }
                    else fileDialog.error = err;
                    break;
                case DialogPurpose::PickXrickBinary:
                {
                    PatchResult r = patchXrickBinaryWithSprites(chosenPath, connections, sprites, eflg);
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
                    if (ok)
                    {
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 1);
                        rebuildBankAtlases(renderer, tileAtlas, blockAtlas, 2);
                        rebuildSpriteAtlas(renderer, spriteAtlas);
                    }
                    patchResultMessage = ok
                        ? "Imported " + std::to_string(MAP_NBR_SUBMAPS) + " submaps (connections + sprites + tile "
                          "hazard flags + tile graphics + block composition + sprite graphics) from "
                          + chosenPath.filename().string()
                          + ". See the \"Screen Connections\", \"Sprite Tools\", Block Palette, Tile Editor, "
                            "Block Editor, and Sprite Editor windows."
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
            ImGui::Text("Tile bank:");
            ImGui::SameLine();
            for (int b = FIRST_USABLE_BANK; b <= LAST_USABLE_BANK; b++)
            {
                if (b > FIRST_USABLE_BANK) ImGui::SameLine();
                char label[16]; std::snprintf(label, sizeof label, "%d##bank", b);
                if (ImGui::RadioButton(label, st.bank == b)) st.bank = b;
            }
            ImGui::Separator();
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
            for (int b = FIRST_USABLE_BANK; b <= LAST_USABLE_BANK; b++)
            {
                if (b > FIRST_USABLE_BANK) ImGui::SameLine();
                char label[16]; std::snprintf(label, sizeof label, "%d##tebank", b);
                if (ImGui::RadioButton(label, tileEditor.bank == b)) tileEditor.bank = b;
            }
            ImGui::TextDisabled("(bank 0 is unused padding data, hidden here too)");
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
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                    if (ImGui::ImageButton("##tile", tileTexId, ImVec2(thumb, thumb), ImVec2(u0, v0), ImVec2(u1, v1)))
                    {
                        tileEditor.selectedTile = t;
                        tileEditor.batchStartTile = t; // still freely editable in the Batch import field below
                    }
                    if (selected) ImGui::PopStyleColor();
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
                if (tileEditor.selectedTile < 0)
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

                    ImGui::Separator();
                    ImGui::Text("Hazard flags (this tile, bank %d):", tileEditor.bank);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("map_eflg_c -- what actually makes a TILE solid, "
                                                                   "climbable, lethal, etc. (see xrick_eflg.h). "
                                                                   "Edits this tile's byte directly.");
                    uint8_t &flags = eflg.bank[tileEditor.bank - 1][tileEditor.selectedTile];
                    auto flagBit = [&](const char *label, int bit, const char *tip)
                    {
                        bool on = (flags & bit) != 0;
                        if (ImGui::Checkbox(label, &on))
                        {
                            flags = on ? (flags | bit) : (flags & ~bit);
                            st.dirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                    };
                    flagBit("Solid", EFLG_SOLID, "SOLID -- can't walk/fall through");
                    flagBit("Lethal", EFLG_LETHAL, "LETHAL -- kills an entity that touches it (this is the corpse-transform trigger)");
                    flagBit("Climb", EFLG_CLIMB, "CLIMB -- entities can climb here");
                    flagBit("Vert", EFLG_VERT, "VERT -- vertical move only (usually paired with Climb)");
                    flagBit("WayUp", EFLG_WAYUP, "WAYUP -- solid except when moving up through it (jump-through platform)");
                    flagBit("SuperPad", EFLG_SPAD, "SPAD -- solid, but bounces entities skyward");
                    flagBit("Fgnd", EFLG_FGND, "FGND -- foreground, drawn in front of / hides entities");
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

                    bool selected = (b == blockEditor.selectedBlock);
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                    else if (blockEditor.swapMode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.45f, 0.10f, 1.0f));
                    if (ImGui::ImageButton("##blk", blkTexId, ImVec2(thumb, thumb), ImVec2(u0, v0), ImVec2(u1, v1)))
                    {
                        if (blockEditor.swapMode)
                        {
                            if (b != blockEditor.selectedBlock)
                            {
                                std::swap(map_blocks[blockEditor.selectedBlock], map_blocks[b]);
                                rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 1);
                                rebuildBlockAtlasOnly(renderer, tileAtlas, blockAtlas, 2);
                                st.dirty = true;
                            }
                            blockEditor.swapMode = false;
                        }
                        else
                        {
                            blockEditor.selectedBlock = b;
                            blockEditor.selectedCell = 0;
                        }
                    }
                    if (selected || blockEditor.swapMode) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(blockEditor.swapMode ? "Swap with block %d" : "Block %d", b);
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
                        if (selected) ImGui::PopStyleColor();
                        drawTileHazardBorder(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), eflg.bank[blockEditor.bank - 1][t]);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cell %d (tile %d)", i, t);
                        if (i % 4 != 3) ImGui::SameLine();
                        ImGui::PopID();
                    }

                    ImGui::Spacing();
                    ImGui::Text("Selected cell: %d (tile %d)", blockEditor.selectedCell, std::clamp(cells[blockEditor.selectedCell], 0, 255));
                    if (ImGui::SmallButton("Clear block (all tile 0)"))
                    {
                        for (int i = 0; i < 16; i++) cells[i] = 0;
                        blockChanged = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(blockEditor.swapMode ? "Cancel swap" : "Swap with..."))
                        blockEditor.swapMode = !blockEditor.swapMode;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click, then pick another block on the left to "
                                                                   "swap the two blocks' tile compositions");

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
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile %d", t);
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
                    ImGui::TextWrapped("Any image size works -- it's resampled to %dx%d. Alpha below %d "
                                        "becomes transparent; opaque pixels are matched to the closest of "
                                        "the 15 non-transparent palette colors.", SPRITE_W, SPRITE_H, SPRITE_ALPHA_THRESHOLD);
                }
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

                    int bank = connections.submaps[s].page == 1 ? 2 : 1;
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::Combo("Tile bank", &bank, "1\0002\0\0")) connections.submaps[s].page = (bank == 2) ? 1 : 0;
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
            // Quick-pick chips for entity ids already used elsewhere on the map.
            std::vector<int> seen;
            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
                for (auto &m : sprites.marks[s])
                    if (std::find(seen.begin(), seen.end(), m.ent) == seen.end()) seen.push_back(m.ent);
            std::sort(seen.begin(), seen.end());
            ImGui::TextDisabled("Used on this map:");
            ImTextureID spriteTexIdQP = (ImTextureID)(intptr_t)spriteAtlas;
            ImGuiStyle &qpStyle = ImGui::GetStyle();
            float qpWindowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            for (size_t qi = 0; qi < seen.size(); qi++)
            {
                int e = seen[qi];
                ImGui::PushID(e);
                int spr = (e >= 0 && e < (int)entDataTable.size()) ? entDataTable[e].spr : 0;
                bool selected = (e == st.selectedEnt);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
                if (spr > 0 && spr < SPRITES_NBR_SPRITES)
                {
                    float u0, v0, u1, v1;
                    sprite_uv(spr, u0, v0, u1, v1);
                    if (ImGui::ImageButton("##qp", spriteTexIdQP, ImVec2(24, 21), ImVec2(u0, v0), ImVec2(u1, v1)))
                        st.selectedEnt = e;
                }
                else
                {
                    char label[16]; std::snprintf(label, sizeof label, "%d", e);
                    if (ImGui::SmallButton(label)) st.selectedEnt = e;
                }
                if (selected) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Entity %d", e);
                // Wrap onto a new line once the next chip would overflow the
                // window's content region, instead of forcing everything
                // onto one never-ending row that clips the last entries.
                float lastItemRight = ImGui::GetItemRectMax().x;
                float nextItemRight = lastItemRight + qpStyle.ItemSpacing.x + ImGui::GetItemRectSize().x;
                if (qi + 1 < seen.size() && nextItemRight < qpWindowRight)
                    ImGui::SameLine();
                ImGui::PopID();
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
                            ImGui::SetTooltip("This entity type (1a, patrol walker) reuses the trigger "
                                               "point's column as its walking distance before a U-turn, "
                                               "instead of a spatial trigger -- higher = walks further.");
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
                        flagBox("Stop", ENT_FLG_TRIGSTOP, "TRIGSTOP -- wakes up when Rick does his \"stop\" move there");
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
                }
            }
        }

        // --- Render ---
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        drawMap(renderer, blockAtlas[st.bank], st, viewportW, viewportH);

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

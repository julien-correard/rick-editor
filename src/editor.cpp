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
#include "sprites_render.h"

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

    // Sprites
    bool showSprites = true;       // overlay toggle -- can be turned off to focus on tile editing
    bool spritePlacementMode = false; // when on, canvas clicks place/remove sprites instead of tiles
    int selectedEnt = 4;           // entity type id to place (lowest observed in stock data)
    int hoverMarkSubmap = -1, hoverMarkIndex = -1; // nearest sprite under cursor, for right-click removal
};

enum class DialogPurpose { OpenMap, SaveMap, PickXrickBinary, PickXrickBinaryForConnections };

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

    // Light grid overlay above a certain zoom level (visual guide).
    if (st.cam.zoom >= 3.0f)
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
static void doSave(EditorState &st, FileDialog &fd, const ConnectionsData &conn, const MarksData &sprites)
{
    if (st.currentPath.empty()) { requestSaveAs(fd, st.currentPath); return; }
    std::string err;
    if (saveMapFileWithSprites(st.currentPath, conn, sprites, err)) st.dirty = false;
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
    FileDialog fileDialog;
    ConnectionsData connections = defaultConnections();
    MarksData sprites = defaultMarks();
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
                    int mx, my; SDL_GetMouseState(&mx, &my);
                    float factor = event.wheel.y > 0 ? 1.15f : 1.0f / 1.15f;
                    zoomAt(st, (float)mx, (float)my, factor);
                }
                else if (event.type == SDL_MOUSEBUTTONDOWN)
                {
                    int col, row;
                    screenToCell(st, (float)event.button.x, (float)event.button.y, col, row);

                    if (st.spritePlacementMode)
                    {
                        int tileCol = std::clamp(screenToTileCol(st, (float)event.button.x), 0, 31);
                        int tileRow = screenToTileRow(st, (float)event.button.y);
                        if (event.button.button == SDL_BUTTON_LEFT)
                        {
                            int owner = submapForAbsRow(connections, tileRow);
                            if (owner >= 0)
                            {
                                sprites.marks[owner].push_back(MarkEntry{tileRow, tileCol, 0, st.selectedEnt, 0, 0, 0});
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
                        continue; // don't also run the tile-editing handlers below
                    }

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
                    else if (event.button.button == SDL_BUTTON_MIDDLE)
                    {
                        st.panning = true;
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
                        if (!event.key.repeat) st.spritePlacementMode = !st.spritePlacementMode;
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
                if (ImGui::MenuItem("Save", "Ctrl+S")) doSave(st, fileDialog, connections, sprites);
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
                if (ImGui::MenuItem("Import connections && sprites from xrick binary..."))
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
            ImGui::EndMainMenuBar();
        }

        fs::path chosenPath;
        if (renderFileDialog(fileDialog, chosenPath))
        {
            std::string err;
            switch (fileDialog.purpose)
            {
                case DialogPurpose::SaveMap:
                    if (saveMapFileWithSprites(chosenPath, connections, sprites, err)) { st.currentPath = chosenPath; st.dirty = false; }
                    else fileDialog.error = err;
                    break;
                case DialogPurpose::OpenMap:
                    if (loadMapFileWithSprites(chosenPath, connections, sprites, err)) { st.currentPath = chosenPath; st.dirty = false; st.sel.active = false; }
                    else fileDialog.error = err;
                    break;
                case DialogPurpose::PickXrickBinary:
                {
                    PatchResult r = patchXrickBinaryWithSprites(chosenPath, connections, sprites);
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
                    patchResultMessage = ok
                        ? "Imported " + std::to_string(MAP_NBR_SUBMAPS) + " submaps (connections + sprites) from "
                          + chosenPath.filename().string() + ". See the \"Screen Connections\" and \"Sprite Tools\" windows."
                        : cerr;
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

        // --- Block palette ---
        ImGui::SetNextWindowPos(ImVec2(10, 34), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
        ImGui::Begin("Block Palette");
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
        float avail = ImGui::GetContentRegionAvail().x;
        int perRow = std::max(1, (int)(avail / (thumb + 6)));
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
            ImGui::PopID();

            if ((b + 1) % perRow != 0 && b != 255) ImGui::SameLine();
        }
        ImGui::End();

        // --- Toolbar / status ---
        ImGui::SetNextWindowPos(ImVec2((float)viewportW - 340, 34), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 260), ImGuiCond_FirstUseEver);
        ImGui::Begin("Tools");
        ImGui::Text("Zoom: x%.2f (%.0f%%)", st.cam.zoom, st.cam.zoom * 100.0f);
        {
            int mx, my; SDL_GetMouseState(&mx, &my);
            int col, row;
            screenToCell(st, (float)mx, (float)my, col, row);
            if (cellValid(col, row))
                ImGui::Text("Cell under cursor: col %d, row %d (index %d)", col, row, mapIndex(col, row));
            else
                ImGui::Text("Cell under cursor: out of map");
            ImGui::Text("Tile row under cursor: %d (Screen Connections / Sprite Tools use this)", screenToTileRow(st, (float)my));
        }
        ImGui::Separator();
        ImGui::TextWrapped("Left click: place selected block (hold to paint)");
        ImGui::TextWrapped("Right click: pick the block under the cursor (hold to keep picking)");
        ImGui::TextWrapped("Shift + drag: rectangle selection");
        ImGui::TextWrapped("Mouse wheel or +/-: zoom (centered on cursor / screen)");
        ImGui::TextWrapped("Middle-drag, arrow keys: pan -- Left/Right, Page Up/Down: fast scroll");
        ImGui::Separator();
        if (ImGui::Button("Clear selection (Del)")) clearSelection(st, 0);
        ImGui::SameLine();
        if (ImGui::Button("Fill selection (F)")) clearSelection(st, st.selectedBlock);
        if (ImGui::Button("Reset view"))
        {
            st.cam.x = 0; st.cam.y = 0; st.cam.zoom = 2.0f;
        }
        ImGui::End();

        // --- Screen Connections (links between submaps) ---
        ImGui::SetNextWindowPos(ImVec2((float)viewportW - 380, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(370, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Screen Connections");
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

            int submapToDelete = -1;
            for (int s = 0; s < MAP_NBR_SUBMAPS; s++)
            {
                ImGui::PushID(s);
                char header[80];
                std::snprintf(header, sizeof header, "Submap %d -- starts at row %d / block %d (%d links)",
                              s, submapStartRow(connections.submaps[s]), submapStartRow(connections.submaps[s]) / 4,
                              (int)connections.exits[s].size());
                if (ImGui::CollapsingHeader(header))
                {
                    ImGui::Indent();

                    int bank = connections.submaps[s].page == 1 ? 2 : 1;
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::Combo("Tile bank", &bank, "1\0002\0\0")) connections.submaps[s].page = (bank == 2) ? 1 : 0;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete (disconnect)"))
                        ImGui::OpenPopup("confirm_delete_submap");
                    if (ImGui::BeginPopup("confirm_delete_submap"))
                    {
                        ImGui::Text("This clears submap %d's own links and disconnects\n"
                                     "any other submap's link that pointed to it.\n"
                                     "Its tiles stay on the map, just unreachable.", s);
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

        // --- Sprite Tools ---
        ImGui::SetNextWindowPos(ImVec2(10, 600), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
        ImGui::Begin("Sprite Tools");
        ImGui::Checkbox("Show sprites on map", &st.showSprites);
        ImGui::Checkbox("Sprite placement mode", &st.spritePlacementMode);
        ImGui::TextWrapped(st.spritePlacementMode
            ? "Left click: place the selected entity. Right click: remove the nearest sprite."
            : "Off: canvas clicks edit tiles as usual. Turn this on to place/remove sprites instead.");
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
            char header[64];
            std::snprintf(header, sizeof header, "Submap %d (%d sprites)", s, (int)sprites.marks[s].size());
            if (ImGui::CollapsingHeader(header))
            {
                ImGui::Indent();
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
                    ImGui::DragInt("##markrow", &m.rowAbs, 1.0f, 0, MAP_TILE_ROWS - 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Block row ~%d", m.rowAbs / 4);
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
                        ImGui::SetTooltip("Where this entity's trigger reacts (e.g. an arrow trap's "
                                           "firing spot), not its own drawn position. Only some entity "
                                           "types use this -- inert otherwise.");
                    ImGui::PopID();
                }
                if (removeIdx >= 0) list.erase(list.begin() + removeIdx);
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        ImGui::End();

        // Sprite overlay on the map canvas: real sprite art when available
        // (ent_entdata's spr field, decoded from sprites_data -- see
        // sprites_render.h), a colored marker fallback otherwise. Drawn on
        // top of the tiles via SDL (so it can use the sprite atlas texture
        // the same way the map itself does); the entity-id label and the
        // trigger-point indicator use the ImGui foreground draw list.
        if (st.showSprites)
        {
            ImDrawList *dl = ImGui::GetForegroundDrawList();
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

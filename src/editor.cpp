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
};

struct FileDialog
{
    bool show = false;
    bool saveMode = false;
    fs::path dir = fs::current_path();
    char filename[256] = "";
    std::string error;
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
    const char* title = fd.saveMode ? "Save As" : "Open Map";
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
                else if (e.path().extension() == ".map") files.push_back(e);
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

        if (ImGui::Button(fd.saveMode ? "Save" : "Open", ImVec2(120, 0)))
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
    fd.show = true; fd.saveMode = false; fd.filename[0] = '\0'; fd.error.clear();
}
static void requestSaveAs(FileDialog &fd, const fs::path &currentPath)
{
    fd.show = true; fd.saveMode = true; fd.filename[0] = '\0';
    if (!currentPath.empty())
        std::snprintf(fd.filename, sizeof(fd.filename), "%s", currentPath.filename().string().c_str());
    fd.error.clear();
}
static void doSave(EditorState &st, FileDialog &fd)
{
    if (st.currentPath.empty()) { requestSaveAs(fd, st.currentPath); return; }
    std::string err;
    if (saveMapFile(st.currentPath, err)) st.dirty = false;
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

    EditorState st;
    FileDialog fileDialog;
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

        // --- Main menu bar (kept as a secondary access point) ---
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open...", "Ctrl+O")) requestOpen(fileDialog);
                if (ImGui::MenuItem("Save", "Ctrl+S")) doSave(st, fileDialog);
                if (ImGui::MenuItem("Save As...")) requestSaveAs(fileDialog, st.currentPath);
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
            if (fileDialog.saveMode)
            {
                if (saveMapFile(chosenPath, err)) { st.currentPath = chosenPath; st.dirty = false; }
                else fileDialog.error = err;
            }
            else
            {
                if (loadMapFile(chosenPath, err)) { st.currentPath = chosenPath; st.dirty = false; st.sel.active = false; }
                else fileDialog.error = err;
            }
        }

        // --- Dedicated File toolbox (separate window, same style as
        // Block Palette / Tools -- easier to spot than the menu bar) ---
        ImGui::SetNextWindowPos(ImVec2((float)viewportW / 2.0f - 150.0f, 34), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 110), ImGuiCond_FirstUseEver);
        ImGui::Begin("File");
        if (ImGui::Button("Open...", ImVec2(90, 0))) requestOpen(fileDialog);
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(90, 0))) doSave(st, fileDialog);
        ImGui::SameLine();
        if (ImGui::Button("Save As...", ImVec2(90, 0))) requestSaveAs(fileDialog, st.currentPath);
        ImGui::Separator();
        if (st.currentPath.empty())
            ImGui::TextDisabled("(new, unsaved map)");
        else
            ImGui::Text("%s%s", st.currentPath.filename().string().c_str(), st.dirty ? " *" : "");
        ImGui::End();

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

        updateWindowTitle(window, st);

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
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

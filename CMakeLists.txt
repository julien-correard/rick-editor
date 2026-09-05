cmake_minimum_required(VERSION 3.16)
project(rickeditor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

find_package(SDL2 REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)

add_executable(rickeditor
    src/editor.cpp
    src/dat_tilesST.c   # source de verite unique pour les tuiles graphiques
    src/dat_spritesST.c # idem pour les sprites (entites/graphismes)
    src/dat_screens.c   # idem pour le texte d'intro entre les niveaux (partiel, voir le fichier)
)
# dat_spritesST.c est vendorise tel quel depuis la source xrick d'origine ;
# son style d'initialisation imbrique (accolades internes elidees) declenche
# -Wmissing-braces de facon benigne -- on le silence pour ce seul fichier
# plutot que de modifier les donnees vendorisees.
set_source_files_properties(src/dat_spritesST.c PROPERTIES COMPILE_OPTIONS "-Wno-missing-braces")

target_include_directories(rickeditor PRIVATE
    include
    src        # pour mapdata.h/tiles_render.h, a cote de editor.cpp
    third_party # pour stb/stb_image.h (tile_import.h)
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(rickeditor PRIVATE imgui ${SDL2_LIBRARIES})
target_compile_options(rickeditor PRIVATE -Wall)
# stb_image.h est vendorise tel quel (cf. src/tile_import.h) ; en usage
# STB_IMAGE_STATIC, les nombreuses fonctions de son API qu'on n'appelle
# pas declenchent -Wunused-function de facon benigne -- silence localise
# a editor.cpp (seul fichier qui l'inclut avec STB_IMAGE_IMPLEMENTATION),
# meme approche que pour dat_spritesST.c ci-dessus.
set_source_files_properties(src/editor.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

# -- map2c: converts an RUxF (.map/RKMC) file into the C data file
#    (game_ruxf.c) that the modified RUxF xrick engine compiles instead of
#    its stock dat_*.c data. Links the data globals (tiles_data, sprites_data)
#    and reuses this project's own repack/encode headers so the emitted
#    arrays match what the editor would write into a patched binary.
add_executable(map2c
    src/map2c.cpp
    src/dat_tilesST.c
    src/dat_spritesST.c
)
set_source_files_properties(src/dat_spritesST.c PROPERTIES COMPILE_OPTIONS "-Wno-missing-braces")
target_include_directories(map2c PRIVATE include src third_party)
target_compile_options(map2c PRIVATE -Wall)
set_target_properties(map2c PROPERTIES OUTPUT_NAME rickeditor-map2c)

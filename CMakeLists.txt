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
)

target_include_directories(rickeditor PRIVATE
    include
    src        # pour mapdata.h/tiles_render.h, a cote de editor.cpp
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(rickeditor PRIVATE imgui ${SDL2_LIBRARIES})
target_compile_options(rickeditor PRIVATE -Wall)

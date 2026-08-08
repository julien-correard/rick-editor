cmake_minimum_required(VERSION 3.16)
project(rickeditor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

find_package(SDL2 REQUIRED)

add_executable(rickeditor
    src/main.cpp
    src/dat_tilesST.c   # source de verite unique pour les tuiles graphiques
)

target_include_directories(rickeditor PRIVATE
    include
    src        # pour mapdata.h, a cote de main.cpp
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(rickeditor PRIVATE ${SDL2_LIBRARIES})
target_compile_options(rickeditor PRIVATE -Wall)

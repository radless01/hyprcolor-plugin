
cmake_minimum_required(VERSION 3.20)
project(hyprcolor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)

find_path(HYPRLAND_INCLUDE_ROOT
    NAMES hyprland/src/plugins/PluginAPI.hpp
    PATHS /usr/include /usr/local/include
    REQUIRED
)

add_library(hyprcolor SHARED main.cpp)

target_include_directories(hyprcolor PRIVATE
    ${HYPRLAND_INCLUDE_ROOT}
    /usr/include/hyprland/protocols
)

target_link_libraries(hyprcolor PRIVATE
    PkgConfig::DRM
    PkgConfig::WAYLAND_SERVER
    PkgConfig::PIXMAN
    dl
)

target_compile_options(hyprcolor PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

set_target_properties(hyprcolor PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hyprcolor"
)

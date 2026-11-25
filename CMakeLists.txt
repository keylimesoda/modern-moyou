cmake_minimum_required(VERSION 3.24)
project(modern-moyou VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

find_package(Vulkan QUIET)
find_package(ZLIB REQUIRED)
find_package(SDL3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

# ---------------------------------------------------------------------------
# Wayland wlr-layer-shell protocol generation (for --layer-shell underlay mode)
# ---------------------------------------------------------------------------
set(WL_PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${WL_PROTO_OUT_DIR}")
set(XDG_SHELL_XML "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(LAYER_SHELL_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml")
set(XDG_SHELL_H "${WL_PROTO_OUT_DIR}/xdg-shell-client-protocol.h")
set(XDG_SHELL_C "${WL_PROTO_OUT_DIR}/xdg-shell-protocol.c")
set(LAYER_SHELL_H "${WL_PROTO_OUT_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h")
set(LAYER_SHELL_C "${WL_PROTO_OUT_DIR}/wlr-layer-shell-unstable-v1-protocol.c")

add_custom_command(
    OUTPUT "${XDG_SHELL_H}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${XDG_SHELL_XML}" "${XDG_SHELL_H}"
    DEPENDS "${XDG_SHELL_XML}"
    COMMENT "Generating xdg-shell-client-protocol.h"
    VERBATIM
)
add_custom_command(
    OUTPUT "${XDG_SHELL_C}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${XDG_SHELL_XML}" "${XDG_SHELL_C}"
    DEPENDS "${XDG_SHELL_XML}" "${XDG_SHELL_H}"
    COMMENT "Generating xdg-shell-protocol.c"
    VERBATIM
)
add_custom_command(
    OUTPUT "${LAYER_SHELL_H}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${LAYER_SHELL_XML}" "${LAYER_SHELL_H}"
    DEPENDS "${LAYER_SHELL_XML}" "${XDG_SHELL_H}"
    COMMENT "Generating wlr-layer-shell-unstable-v1-client-protocol.h"
    VERBATIM
)
add_custom_command(
    OUTPUT "${LAYER_SHELL_C}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${LAYER_SHELL_XML}" "${LAYER_SHELL_C}"
    DEPENDS "${LAYER_SHELL_XML}" "${LAYER_SHELL_H}"
    COMMENT "Generating wlr-layer-shell-unstable-v1-protocol.c"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Decoded RSX shader compilation (shaders/decoded/*.{vert,frag} -> build/shaders/*.spv)
# ---------------------------------------------------------------------------
set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/decoded")
set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

set(DECODED_SHADERS
    background.vert
    background.frag
    back_colours0.frag
    lines1.vert
    lines1.frag
    lines_composite.frag
    ffd_shader0.frag
    ffd_shader1.frag
    particles_quads.vert
    particles_quads.frag
    particles_second.vert
    particles_second.frag
    glare_source_pre.frag
    glare_sum.frag
    gaussian.vert
    gaussian.frag
    gauss_bloom_and_blur.frag
    apply_glare.frag
    tonemap.vert
    tonemap.frag
    hud.vert
    hud.frag
)

set(SHADER_SPV_OUTPUTS "")
foreach(SHADER_FILE IN LISTS DECODED_SHADERS)
    set(SRC_PATH "${SHADER_SRC_DIR}/${SHADER_FILE}")
    set(OUT_PATH "${SHADER_OUT_DIR}/${SHADER_FILE}.spv")
    add_custom_command(
        OUTPUT "${OUT_PATH}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.2 "${SRC_PATH}" -o "${OUT_PATH}"
        DEPENDS "${SRC_PATH}"
        COMMENT "Compiling decoded RSX GLSL shader ${SHADER_FILE}"
        VERBATIM
    )
    list(APPEND SHADER_SPV_OUTPUTS "${OUT_PATH}")
endforeach()

add_custom_target(xmb_shaders ALL DEPENDS ${SHADER_SPV_OUTPUTS})

# ---------------------------------------------------------------------------
# Asset loaders & procedural LUTs library (QRC, MNU, DDS, _Stripes/_FresLUT/_IridescentTex)
# ---------------------------------------------------------------------------
add_library(xmb_assets STATIC
    src/assets/qrc_loader.cpp
    src/assets/mnu_parser.cpp
    src/assets/dds_loader.cpp
    src/assets/json_theme.cpp
    src/gfx/proc_luts.cpp
)
target_include_directories(xmb_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xmb_assets PUBLIC ZLIB::ZLIB)
target_compile_options(xmb_assets PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Simulation library (AVX-512 native by default; -DXMB_PORTABLE=ON for AVX2+FMA)
# ---------------------------------------------------------------------------
option(XMB_PORTABLE "Build portable x86-64-v3 (AVX2+FMA) binary instead of native AVX-512" OFF)
add_library(xmb_sim STATIC
    src/sim/ribbon_sim.cpp
    src/sim/particle_sim.cpp
)
target_include_directories(xmb_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xmb_sim PUBLIC xmb_assets)
if(XMB_PORTABLE)
    target_compile_options(xmb_sim PRIVATE -Wall -Wextra -Wpedantic -O2 -march=x86-64-v3)
else()
    target_compile_options(xmb_sim PRIVATE -Wall -Wextra -Wpedantic -O2 -march=native -mavx512f -mavx512vl)
endif()

# ---------------------------------------------------------------------------
# Vulkan renderer library (requires vulkan-headers)
# ---------------------------------------------------------------------------
if(Vulkan_FOUND)
    add_library(xmb_gfx STATIC
        src/gfx/vk_renderer.cpp
    )
    target_include_directories(xmb_gfx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(xmb_gfx PUBLIC
        xmb_assets
        xmb_sim
        Vulkan::Vulkan
        SDL3::SDL3
    )
    target_compile_definitions(xmb_gfx
        PUBLIC VK_USE_PLATFORM_WAYLAND_KHR=1
        PRIVATE
            XMB_SHADER_DIR="${SHADER_OUT_DIR}"
            XMB_INSTALL_SHADER_DIR="${CMAKE_INSTALL_FULL_DATADIR}/modern-moyou/shaders"
    )
    target_compile_options(xmb_gfx PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(xmb_gfx xmb_shaders)
else()
    message(WARNING "Vulkan headers not found (install 'vulkan-headers' package). Building headless assets, AVX-512 sim, decoded SPIR-V shaders, --inspect/--benchmark CLI, and unit tests without xmb_gfx.")
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(modern-moyou
    src/main.cpp
    src/wayland_layer.cpp
    "${LAYER_SHELL_C}"
    "${LAYER_SHELL_H}"
    "${XDG_SHELL_C}"
    "${XDG_SHELL_H}"
)
target_include_directories(modern-moyou PRIVATE "${WL_PROTO_OUT_DIR}")
target_link_libraries(modern-moyou PRIVATE
    xmb_assets
    xmb_sim
    PkgConfig::WAYLAND_CLIENT
)
if(Vulkan_FOUND)
    target_link_libraries(modern-moyou PRIVATE xmb_gfx)
    target_compile_definitions(modern-moyou PRIVATE XMB_HAS_VULKAN=1)
else()
    add_dependencies(modern-moyou xmb_shaders)
endif()
target_compile_definitions(modern-moyou PRIVATE
    XMB_INSTALL_THEMES_DIR="${CMAKE_INSTALL_FULL_DATADIR}/modern-moyou/themes"
    XMB_SOURCE_THEMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/themes"
)
target_compile_options(modern-moyou PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(xmb_tests
    src/tests/test_assets_sim.cpp
)
target_link_libraries(xmb_tests PRIVATE
    xmb_assets
    xmb_sim
)
target_compile_definitions(xmb_tests PRIVATE
    XMB_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    XMB_THEMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/themes"
)
target_compile_options(xmb_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME xmb_assets_and_sim COMMAND xmb_tests)

# ---------------------------------------------------------------------------
# Install rules (used by Arch PKGBUILD / `cmake --install`)
# ---------------------------------------------------------------------------
install(TARGETS modern-moyou RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ${SHADER_SPV_OUTPUTS} DESTINATION "${CMAKE_INSTALL_DATADIR}/modern-moyou/shaders")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/themes/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/modern-moyou/themes"
    FILES_MATCHING PATTERN "*.json")

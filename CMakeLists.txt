cmake_minimum_required(VERSION 3.24)
project(OpenSkies LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
add_compile_options(/utf-8 /MP)

set(EXT ${CMAKE_CURRENT_SOURCE_DIR}/external)

# SafetyHook (fetches Zydis)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/safetyhook EXCLUDE_FROM_ALL)

# zstd decoder only (for game pak reading)
file(GLOB ZSTD_SRC ${EXT}/zstd/lib/common/*.c ${EXT}/zstd/lib/decompress/*.c)
add_library(zstd_dec STATIC ${ZSTD_SRC})
target_include_directories(zstd_dec PUBLIC ${EXT}/zstd/lib)
target_compile_definitions(zstd_dec PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)

# Dear ImGui core
add_library(imgui STATIC
  ${EXT}/imgui/imgui.cpp ${EXT}/imgui/imgui_draw.cpp ${EXT}/imgui/imgui_tables.cpp
  ${EXT}/imgui/imgui_widgets.cpp ${EXT}/imgui/imgui_demo.cpp
  ${EXT}/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${EXT}/imgui ${EXT}/imgui/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# Embedded English locale as a byte array
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/locales/en.json)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/locales/en.json OS_EN_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OS_EN_HEX "${OS_EN_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/EmbeddedLocale.inc "static const unsigned char kEnJson[] = {${OS_EN_HEX}0x00};\n")

# Embedded hook targets: game/targets.json plus one file per track in game/targets/
file(GLOB OS_TG_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game/targets.json ${CMAKE_CURRENT_SOURCE_DIR}/game/targets/*.json)
set(OS_TG_INC "")
set(OS_TG_LIST "")
set(OS_TG_N 0)
foreach(f ${OS_TG_FILES})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${f})
  file(READ ${f} OS_TG_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OS_TG_HEX "${OS_TG_HEX}")
  string(APPEND OS_TG_INC "static const unsigned char kTargets${OS_TG_N}[] = {${OS_TG_HEX}0x00};\n")
  string(APPEND OS_TG_LIST "(const char*)kTargets${OS_TG_N},")
  math(EXPR OS_TG_N "${OS_TG_N} + 1")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/EmbeddedTargets.inc "${OS_TG_INC}static const char* kTargetFiles[] = {${OS_TG_LIST}};\n")

# Expedition season data
set(OS_XP ${CMAKE_CURRENT_SOURCE_DIR}/game/expeditions.pack)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_XP})
file(READ ${OS_XP} OS_XP_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OS_XP_HEX "${OS_XP_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/EmbeddedSeasons.inc "static const unsigned char kSeasonPack[] = {${OS_XP_HEX}};\n")

# Game independent UI and core
file(GLOB OS_CORE src/core/*.cpp src/assets/*.cpp src/ui/*.cpp)
add_library(os_ui STATIC ${OS_CORE})
target_include_directories(os_ui PUBLIC src ${EXT}/json/include ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(os_ui PUBLIC imgui zstd_dec)

# The mod: version.dll proxy
file(GLOB OS_MOD src/mod/*.cpp src/game/*.cpp src/hooks/*.cpp src/render/*.cpp src/input/*.cpp src/features/*.cpp)
add_library(version SHARED proxy/version_exports.c ${OS_MOD} ${EXT}/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(version PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/gen ${EXT}/Vulkan-Headers/include)
target_compile_definitions(version PRIVATE VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(version PRIVATE os_ui safetyhook)
target_link_options(version PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/proxy/version.def")
set_target_properties(version PROPERTIES OUTPUT_NAME "version" PREFIX "")
# Ship a PDB with release builds so the dll can be inspected
target_compile_options(version PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(version PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF /PDBALTPATH:version.pdb>)

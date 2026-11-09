cmake_minimum_required(VERSION 3.16)
project(qt_player VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_TESTS        "Build unit / integration tests" OFF)
option(ENABLE_SANITIZERS  "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# ── Dependencies ──────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network)

# FFmpeg >= 5.1 (new channel-layout API). pkg-config on Linux / MSYS2,
# otherwise manual paths via -DFFMPEG_DIR=<root with include/ and lib/>.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT FFMPEG_DIR)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=59
        libavcodec>=59
        libavutil>=57
        libswresample>=4
        libswscale>=6
        libavfilter>=8)
    set(FFMPEG_TARGET PkgConfig::FFMPEG)
else()
    if(NOT FFMPEG_DIR)
        set(FFMPEG_DIR "C:/Users/My_PC/ffmpeg-7.0.2-full_build" CACHE PATH "FFmpeg root")
    endif()
    find_library(AVFORMAT_LIBRARY    avformat    PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(AVCODEC_LIBRARY     avcodec     PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(AVUTIL_LIBRARY      avutil      PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(SWRESAMPLE_LIBRARY  swresample  PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(SWSCALE_LIBRARY     swscale     PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(AVFILTER_LIBRARY    avfilter    PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    add_library(ffmpeg_manual INTERFACE)
    target_include_directories(ffmpeg_manual INTERFACE "${FFMPEG_DIR}/include")
    target_link_libraries(ffmpeg_manual INTERFACE
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${AVFILTER_LIBRARY})
    set(FFMPEG_TARGET ffmpeg_manual)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────
add_library(qt_player_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qt_player_flags INTERFACE -Wall -Wextra -Wno-unused-parameter)
    if(ENABLE_SANITIZERS)
        target_compile_options(qt_player_flags INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(qt_player_flags INTERFACE -fsanitize=address,undefined)
    endif()
elseif(MSVC)
    target_compile_options(qt_player_flags INTERFACE /W3 /utf-8)
    target_compile_definitions(qt_player_flags INTERFACE _USE_MATH_DEFINES NOMINMAX)
endif()

# ── Core static library (no Qt Widgets dependency) ───────────────────────
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp src/core/*.h
    src/network/*.cpp src/network/*.h
    src/utils/*.cpp src/utils/*.h)

add_library(qt_player_core STATIC ${CORE_SOURCES})
target_include_directories(qt_player_core PUBLIC src)
target_link_libraries(qt_player_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network ${FFMPEG_TARGET}
    PRIVATE qt_player_flags)

# ── UI library ────────────────────────────────────────────────────────────
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp src/ui/*.h)
add_library(qt_player_ui STATIC ${UI_SOURCES})
target_include_directories(qt_player_ui PUBLIC src)
target_link_libraries(qt_player_ui
    PUBLIC Qt6::Widgets Qt6::Multimedia qt_player_core
    PRIVATE qt_player_flags)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(qt_player src/main.cpp)
target_link_libraries(qt_player PRIVATE qt_player_ui qt_player_core qt_player_flags)

# ── Tests ─────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

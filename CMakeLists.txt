cmake_minimum_required(VERSION 3.16)
project(qt_player VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# ── Dependencies ──────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia Network)

# FFmpeg via pkg-config (MSYS2, Linux) or manual path
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVFORMAT   REQUIRED libavformat>=58)
    pkg_check_modules(AVCODEC    REQUIRED libavcodec>=58)
    pkg_check_modules(AVUTIL     REQUIRED libavutil>=56)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample>=3)
    pkg_check_modules(SWSCALE    REQUIRED libswscale>=5)
    set(FFMPEG_INCLUDE_DIRS
        ${AVFORMAT_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWRESAMPLE_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS})
    set(FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES}
        ${SWSCALE_LIBRARIES})
else()
    # Fallback: manual FFmpeg paths (set via -DFFMPEG_DIR=...)
    if(NOT FFMPEG_DIR)
        set(FFMPEG_DIR "C:/Users/My_PC/ffmpeg-7.0.2-full_build" CACHE PATH "FFmpeg root")
    endif()
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_DIR}/include")
    find_library(AVFORMAT_LIBRARY    avformat    PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(AVCODEC_LIBRARY     avcodec     PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(AVUTIL_LIBRARY      avutil      PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(SWRESAMPLE_LIBRARY  swresample  PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    find_library(SWSCALE_LIBRARY     swscale     PATHS "${FFMPEG_DIR}/lib" REQUIRED)
    set(FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${SWSCALE_LIBRARY})
endif()

# ── Core static library (no Qt UI dependency) ────────────────────────────
file(GLOB_RECURSE CORE_SOURCES
    src/core/*.cpp
    src/network/*.cpp
    src/utils/*.cpp)

add_library(qt_player_core STATIC ${CORE_SOURCES})
target_include_directories(qt_player_core PUBLIC src)
target_include_directories(qt_player_core PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(qt_player_core PUBLIC Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network)
target_link_libraries(qt_player_core PRIVATE ${FFMPEG_LIBRARIES})

# ── UI library ────────────────────────────────────────────────────────────
file(GLOB_RECURSE UI_SOURCES src/ui/*.cpp)
add_library(qt_player_ui STATIC ${UI_SOURCES})
target_include_directories(qt_player_ui PUBLIC src)
target_link_libraries(qt_player_ui PUBLIC Qt6::Widgets Qt6::Multimedia qt_player_core)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(qt_player src/main.cpp)
target_link_libraries(qt_player PRIVATE qt_player_ui qt_player_core)

# ── Tests (optional, enabled via -DBUILD_TESTS=ON) ───────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
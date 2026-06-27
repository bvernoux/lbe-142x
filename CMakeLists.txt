cmake_minimum_required(VERSION 3.15)
project(lbe-142x C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True)

# Windows CRT linkage. MinGW links libgcc statically below; MSVC defaults
# to the static CRT (/MT) so the produced .exe has no VCRUNTIME140.dll /
# ucrtbase.dll runtime dependency. Note libusb-1.0.dll is built against
# the dynamic CRT -- mixing runtimes across the DLL boundary works here
# because no CRT objects (FILE *, heap pointers, etc) cross it.
# Override with -DLBE_MSVC_STATIC_CRT=OFF to link the dynamic CRT.
if(WIN32 AND MSVC)
    option(LBE_MSVC_STATIC_CRT "Link MSVC against the static CRT (/MT)" ON)
    if(LBE_MSVC_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

# Default to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include_directories(${PROJECT_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/main.c
    src/lbe_device.c
    src/lbe_platform.c
    src/model_1420.c
    src/model_1421.c
    src/model_mini.c
    src/gnss_view.c
    src/nmea.c
    src/ubx.c
)
if(WIN32)
    list(APPEND SOURCES src/lbe_transport_windows.c src/lbe_serial_windows.c)
else()
    list(APPEND SOURCES src/lbe_transport_linux.c src/lbe_serial_linux.c)
endif()

set(HEADERS
    include/lbe_common.h
    include/lbe_device.h
    include/lbe_transport.h
    include/lbe_model.h
    include/lbe_platform.h
    include/gnss_view.h
    include/nmea.h
    include/lbe_serial.h
    include/ubx.h
)

if(NOT MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "-s")
endif()

# libusb resolution on Windows — no pkg-config.
#
# Escape hatch: if LIBUSB_INCLUDE_DIR, LIBUSB_LIBRARY, and LIBUSB_DLL are
# all set on the command line, use them as-is (e.g. CI).
#
# MSVC: download the official libusb pre-built .7z from GitHub releases
# into build/_deps/libusb-<ver>/ and pick the VS subdir that matches the
# toolset. Requires CMake 3.18+ (file(ARCHIVE_EXTRACT)).
#
# MinGW: locate libusb from the MinGW64 toolchain itself — headers, import
# library, and runtime DLL all live next to the compiler (MSYS2 provides
# mingw-w64-x86_64-libusb at mingw64/{include,lib,bin}).
if(WIN32)
    set(LIBUSB_VERSION "1.0.29" CACHE STRING "libusb version to download for MSVC")

    if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY AND LIBUSB_DLL
       AND EXISTS "${LIBUSB_LIBRARY}" AND EXISTS "${LIBUSB_DLL}")
        message(STATUS "libusb: using pre-provided paths")
    elseif(MSVC)
        if(CMAKE_VERSION VERSION_LESS 3.18)
            message(FATAL_ERROR
                "MSVC build requires CMake >= 3.18 for file(ARCHIVE_EXTRACT). "
                "You have ${CMAKE_VERSION}.")
        endif()

        set(LIBUSB_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/libusb-${LIBUSB_VERSION}")
        set(LIBUSB_ARCHIVE_FILE "${LIBUSB_DOWNLOAD_DIR}/libusb-${LIBUSB_VERSION}.7z")

        if(NOT EXISTS "${LIBUSB_DOWNLOAD_DIR}/include/libusb-1.0/libusb.h")
            file(MAKE_DIRECTORY "${LIBUSB_DOWNLOAD_DIR}")
            if(NOT EXISTS "${LIBUSB_ARCHIVE_FILE}")
                message(STATUS "Downloading libusb-${LIBUSB_VERSION}.7z")
                file(DOWNLOAD
                    "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.7z"
                    "${LIBUSB_ARCHIVE_FILE}"
                    SHOW_PROGRESS
                    STATUS _dl_status)
                list(GET _dl_status 0 _dl_rc)
                if(NOT _dl_rc EQUAL 0)
                    file(REMOVE "${LIBUSB_ARCHIVE_FILE}")
                    message(FATAL_ERROR "libusb download failed: ${_dl_status}")
                endif()
            endif()
            # CMake's bundled libarchive fails on libusb's .7z with
            # "Lzma library error: Invalid options" (observed with CMake
            # 4.3.1). Prefer 7z.exe when available; fall back to
            # file(ARCHIVE_EXTRACT) for environments where it works.
            find_program(SEVENZIP_EXE NAMES 7z 7za 7zz
                HINTS "$ENV{ProgramFiles}/7-Zip"
                      "$ENV{ProgramFiles\(x86\)}/7-Zip")
            if(SEVENZIP_EXE)
                message(STATUS "Extracting libusb with ${SEVENZIP_EXE}")
                execute_process(
                    COMMAND "${SEVENZIP_EXE}" x -y
                            "-o${LIBUSB_DOWNLOAD_DIR}"
                            "${LIBUSB_ARCHIVE_FILE}"
                    RESULT_VARIABLE _7z_rc
                    OUTPUT_QUIET)
                if(NOT _7z_rc EQUAL 0)
                    message(FATAL_ERROR
                        "7z extraction of ${LIBUSB_ARCHIVE_FILE} failed (rc=${_7z_rc})")
                endif()
            else()
                file(ARCHIVE_EXTRACT INPUT "${LIBUSB_ARCHIVE_FILE}"
                                     DESTINATION "${LIBUSB_DOWNLOAD_DIR}")
            endif()
        endif()

        if(MSVC_VERSION GREATER_EQUAL 1930)
            set(LIBUSB_VS_DIR "VS2022")
        elseif(MSVC_VERSION GREATER_EQUAL 1920)
            set(LIBUSB_VS_DIR "VS2019")
        else()
            set(LIBUSB_VS_DIR "VS2015")
        endif()

        set(LIBUSB_INCLUDE_DIR "${LIBUSB_DOWNLOAD_DIR}/include")
        set(LIBUSB_LIBRARY     "${LIBUSB_DOWNLOAD_DIR}/${LIBUSB_VS_DIR}/MS64/dll/libusb-1.0.lib")
        set(LIBUSB_DLL         "${LIBUSB_DOWNLOAD_DIR}/${LIBUSB_VS_DIR}/MS64/dll/libusb-1.0.dll")

        if(NOT EXISTS "${LIBUSB_LIBRARY}" OR NOT EXISTS "${LIBUSB_DLL}")
            message(FATAL_ERROR "libusb ${LIBUSB_VS_DIR} binaries not found after extraction:\n"
                                "  ${LIBUSB_LIBRARY}\n"
                                "  ${LIBUSB_DLL}")
        endif()
    elseif(MINGW)
        get_filename_component(_MINGW_BIN  "${CMAKE_C_COMPILER}" DIRECTORY)
        get_filename_component(_MINGW_ROOT "${_MINGW_BIN}" DIRECTORY)

        find_path(LIBUSB_INCLUDE_DIR libusb.h
            HINTS "${_MINGW_ROOT}/include/libusb-1.0"
            NO_DEFAULT_PATH)
        find_library(LIBUSB_LIBRARY
            NAMES usb-1.0 libusb-1.0
            HINTS "${_MINGW_ROOT}/lib"
            NO_DEFAULT_PATH)
        find_file(LIBUSB_DLL libusb-1.0.dll
            HINTS "${_MINGW_BIN}"
            NO_DEFAULT_PATH)

        if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY OR NOT LIBUSB_DLL)
            message(FATAL_ERROR
                "libusb-1.0 not found in the MinGW64 toolchain "
                "(searched under ${_MINGW_ROOT}).\n"
                "Install the MSYS2 package: pacman -S mingw-w64-x86_64-libusb")
        endif()
    else()
        message(FATAL_ERROR "Unsupported Windows toolchain (need MSVC or MinGW)")
    endif()

    message(STATUS "LIBUSB_INCLUDE_DIR: ${LIBUSB_INCLUDE_DIR}")
    message(STATUS "LIBUSB_LIBRARY:     ${LIBUSB_LIBRARY}")
    message(STATUS "LIBUSB_DLL:         ${LIBUSB_DLL}")
endif()

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

if(WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE "${LIBUSB_INCLUDE_DIR}")
    target_link_libraries(${PROJECT_NAME} "${LIBUSB_LIBRARY}")

    # Copy libusb-1.0.dll next to the produced executable.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LIBUSB_DLL}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying libusb-1.0.dll next to ${PROJECT_NAME}")
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUDEV REQUIRED libudev)
    if(NOT LIBUDEV_FOUND)
        message(FATAL_ERROR "libudev not found. sudo apt install libudev-dev")
    endif()
    target_link_libraries(${PROJECT_NAME} udev)
endif()

# Compiler flags
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-pedantic -Werror)
    # MinGW: statically link libgcc (and winpthread if it ever gets pulled
    # in). libusb stays as a dynamic dependency via its import library.
    if(MINGW)
        target_link_options(${PROJECT_NAME} PRIVATE
            -static-libgcc
            -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive
            -Wl,-Bdynamic)
    endif()
endif()

# Installation
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
if(WIN32)
    install(FILES "${LIBUSB_DLL}" DESTINATION bin)
endif()

message(STATUS "CMAKE_SYSTEM_NAME:        ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMAKE_SYSTEM_PROCESSOR:   ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_BUILD_TYPE:         ${CMAKE_BUILD_TYPE}")
message(STATUS "CMAKE_C_COMPILER_ID:      ${CMAKE_C_COMPILER_ID}")
message(STATUS "CMAKE_C_COMPILER_VERSION: ${CMAKE_C_COMPILER_VERSION}")

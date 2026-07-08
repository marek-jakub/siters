cmake_minimum_required(VERSION 3.10)
project(siters C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(CMOCKA REQUIRED cmocka)

# MuPDF static libs need --static so pkg-config emits all transitive deps
pkg_check_modules(MUPDF REQUIRED mupdf)
execute_process(
    COMMAND pkg-config --libs --static mupdf
    OUTPUT_VARIABLE MUPDF_STATIC_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
separate_arguments(MUPDF_STATIC_LIBS NATIVE_COMMAND "${MUPDF_STATIC_LIBS}")

# MuPDF's pkg-config --static lists -lgumbo and -lmujs as shared libs,
# but neither libgumbo2 nor libmujs3 are available on Debian 12 (MX Linux 23).
# Replace them with static versions to eliminate runtime NEEDED entries.
find_library(GUMBO_STATIC_LIB libgumbo.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib NO_DEFAULT_PATH)
find_library(MUJS_STATIC_LIB libmujs.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib NO_DEFAULT_PATH)
if(GUMBO_STATIC_LIB)
    list(REMOVE_ITEM MUPDF_STATIC_LIBS -lgumbo)
    list(APPEND MUPDF_STATIC_LIBS ${GUMBO_STATIC_LIB})
endif()
if(MUJS_STATIC_LIB)
    list(REMOVE_ITEM MUPDF_STATIC_LIBS -lmujs)
    list(APPEND MUPDF_STATIC_LIBS ${MUJS_STATIC_LIB})
endif()
find_library(JPEG_STATIC_LIB libjpeg.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib NO_DEFAULT_PATH)
if(JPEG_STATIC_LIB)
    list(REMOVE_ITEM MUPDF_STATIC_LIBS -ljpeg)
    list(APPEND MUPDF_STATIC_LIBS ${JPEG_STATIC_LIB})
endif()

# Statically link jbig2dec and openjp2 to avoid header/library version mismatches
# between the build system (Ubuntu) and target system (Debian 12 / MX Linux).
find_library(JBIG2DEC_STATIC_LIB libjbig2dec.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib NO_DEFAULT_PATH)
if(JBIG2DEC_STATIC_LIB)
    list(REMOVE_ITEM MUPDF_STATIC_LIBS -ljbig2dec)
    list(APPEND MUPDF_STATIC_LIBS ${JBIG2DEC_STATIC_LIB})
endif()
find_library(OPENJP2_STATIC_LIB libopenjp2.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib NO_DEFAULT_PATH)
if(OPENJP2_STATIC_LIB)
    list(REMOVE_ITEM MUPDF_STATIC_LIBS -lopenjp2)
    list(APPEND MUPDF_STATIC_LIBS ${OPENJP2_STATIC_LIB})
endif()

# Include directories
include_directories(include include/json-glib src
    ${GTK_INCLUDE_DIRS} ${MUPDF_INCLUDE_DIRS} ${CMOCKA_INCLUDE_DIRS})

# Data directory — default to source root for development, override with -DDATADIR=... for install
include(GNUInstallDirs)
if(NOT DEFINED DATADIR)
  set(DATADIR "${CMAKE_SOURCE_DIR}")
endif()
add_definitions(-DDATADIR="${DATADIR}")

# Main application
add_executable(siters
    src/main.c
    src/siters.c
    src/sessions_model.c
    src/session_model.c
    src/document_model.c
    src/pdf_mupdf.c
    src/glibc_compat.c
    src/log.c
)

# MuPDF static libs must appear before GTK libs so GNU ld can resolve
# symbols (e.g. harfbuzz references inside mupdf-third).
target_link_libraries(siters
    ${MUPDF_STATIC_LIBS}
    ${GTK_LIBRARIES}
    m
    /usr/lib/x86_64-linux-gnu/libjson-glib-1.0.so.0
)

# Wrap GLIBC_2.38 math symbols (fmod/fmodf) with our own implementations
# for compatibility with Debian 12 (glibc 2.36).
target_link_options(siters PRIVATE
    -Wl,--wrap=fmodf
    -Wl,--wrap=fmod
)

# Enable testing
enable_testing()

# CMocka unit tests
add_executable(test_siters_unit
    tests/test_siters_unit.c
    src/sessions_model.c
    src/session_model.c
    src/document_model.c
    src/pdf_mupdf.c
    src/glibc_compat.c
    src/log.c
)

target_include_directories(test_siters_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_siters_unit
    ${MUPDF_STATIC_LIBS}
    ${GTK_LIBRARIES}
    ${CMOCKA_LIBRARIES}
    m
    /usr/lib/x86_64-linux-gnu/libjson-glib-1.0.so.0
)

# Add linker flags for function wrapping
target_link_options(test_siters_unit PRIVATE
    -Wl,--wrap=fmodf
    -Wl,--wrap=fmod
    -Wl,--wrap=gtk_window_new
    -Wl,--wrap=gtk_window_set_title
    -Wl,--wrap=gtk_window_set_default_size
    -Wl,--wrap=g_signal_connect_data
    -Wl,--wrap=gtk_window_set_geometry_hints
    -Wl,--wrap=gtk_widget_get_allocation
    -Wl,--wrap=gtk_range_get_adjustment
    -Wl,--wrap=gtk_adjustment_get_value
    -Wl,--wrap=gtk_scrolled_window_get_vadjustment
)

add_test(NAME unit_tests COMMAND test_siters_unit)

# Installation
install(TARGETS siters RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/siters/data
        PATTERN "test_pdfs" EXCLUDE)
install(FILES data/icons/siters.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/icons/siters_16.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/apps RENAME siters.png)
install(FILES data/icons/siters_24.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/24x24/apps RENAME siters.png)
install(FILES data/icons/siters_32.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps RENAME siters.png)
install(FILES data/icons/siters_48.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps RENAME siters.png)
install(FILES data/icons/siters_64.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps RENAME siters.png)
install(FILES data/icons/siters_128.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps RENAME siters.png)

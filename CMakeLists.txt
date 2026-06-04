cmake_minimum_required(VERSION 3.10)
project(siters C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(POPPLER REQUIRED poppler-glib)
pkg_check_modules(CMOCKA REQUIRED cmocka)

# Include directories
include_directories(include include/json-glib ${GTK_INCLUDE_DIRS} ${POPPLER_INCLUDE_DIRS} ${CMOCKA_INCLUDE_DIRS})

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
)

target_link_libraries(siters 
    ${GTK_LIBRARIES}
    ${POPPLER_LIBRARIES}
    m
    /usr/lib/x86_64-linux-gnu/libjson-glib-1.0.so.0
)

# Enable testing
enable_testing()

# CMocka unit tests
# Unit test executable
add_executable(test_siters_unit
    tests/test_siters_unit.c
    src/siters.c
    src/sessions_model.c
    src/session_model.c
    src/document_model.c
)

target_link_libraries(test_siters_unit
    ${GTK_LIBRARIES}
    ${POPPLER_LIBRARIES}
    ${CMOCKA_LIBRARIES}
    m
    /usr/lib/x86_64-linux-gnu/libjson-glib-1.0.so.0
)

# Add linker flags for function wrapping
target_link_options(test_siters_unit PRIVATE
    -Wl,--wrap=gtk_window_new
    -Wl,--wrap=gtk_window_set_title
    -Wl,--wrap=gtk_window_set_default_size
    -Wl,--wrap=g_signal_connect_data
    -Wl,--wrap=gtk_window_set_geometry_hints
)

add_test(NAME unit_tests COMMAND test_siters_unit)

# Installation
install(TARGETS siters RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/siters/data
        PATTERN "test_pdfs" EXCLUDE)
install(FILES data/icons/siters48x48.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps RENAME siters.png)

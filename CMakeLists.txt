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
include_directories(${GTK_INCLUDE_DIRS} ${POPPLER_INCLUDE_DIRS} ${CMOCKA_INCLUDE_DIRS})

# Main application
add_executable(siters 
    src/main.c
    src/siters.c
)

target_link_libraries(siters 
    ${GTK_LIBRARIES}
    ${POPPLER_LIBRARIES}
    m
)

# Enable testing
enable_testing()

# CMocka unit tests
# Unit test executable
add_executable(test_siters_unit
    tests/test_siters_unit.c
    src/siters.c
)

target_link_libraries(test_siters_unit
    ${GTK_LIBRARIES}
    ${POPPLER_LIBRARIES}
    ${CMOCKA_LIBRARIES}
    m
)

# Add linker flags for function wrapping
target_link_options(test_siters_unit PRIVATE
    -Wl,--wrap=gtk_window_new
    -Wl,--wrap=gtk_window_set_title
    -Wl,--wrap=gtk_window_set_default_size
    -Wl,--wrap=g_signal_connect_data
)

add_test(NAME unit_tests COMMAND test_siters_unit)

# Installation
install(TARGETS siters DESTINATION bin)

cmake_minimum_required(VERSION 3.20)
project(xournalpp_gtk4_canvas LANGUAGES CXX)

# A separate executable: never link GTK 3 and GTK 4 into the same process.
find_package(PkgConfig REQUIRED)
pkg_check_modules(UI REQUIRED IMPORTED_TARGET gtk4>=4.10 libadwaita-1>=1.4 poppler-glib)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

add_executable(xournalpp-gtk4-canvas main.cpp)
target_compile_features(xournalpp-gtk4-canvas PRIVATE cxx_std_20)
target_compile_options(xournalpp-gtk4-canvas PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(xournalpp-gtk4-canvas PRIVATE PkgConfig::UI)

enable_testing()
add_executable(canvas-state-test CanvasStateTest.cpp)
target_compile_features(canvas-state-test PRIVATE cxx_std_20)
target_compile_options(canvas-state-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME canvas-state COMMAND canvas-state-test)

add_executable(ink-renderer-test InkRendererTest.cpp)
target_compile_features(ink-renderer-test PRIVATE cxx_std_20)
target_compile_options(ink-renderer-test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ink-renderer-test PRIVATE PkgConfig::CAIRO)
add_test(NAME ink-renderer COMMAND ink-renderer-test)

add_test(NAME reject-unknown-option COMMAND xournalpp-gtk4-canvas --unknown-option)
add_test(NAME reject-missing-pdf COMMAND xournalpp-gtk4-canvas --pdf ${CMAKE_CURRENT_BINARY_DIR}/missing.pdf)
set_tests_properties(reject-unknown-option reject-missing-pdf PROPERTIES WILL_FAIL TRUE)

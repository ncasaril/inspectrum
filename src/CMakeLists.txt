set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

# For OSX - don't clear RPATH on install
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

if (MSVC)
    #force std::complex<> typedefs in liquiddsp
    add_definitions(-D_LIBCPP_COMPLEX)

    #enable math definitions in math.h
    add_definitions(-D_USE_MATH_DEFINES)

    #build a graphical application without the console
    option(BUILD_WIN32 "Build win32 app, false for console" TRUE)
    if (BUILD_WIN32)
        set(EXE_ARGS WIN32)
        set(CMAKE_EXE_LINKER_FLAGS "/entry:mainCRTStartup ${CMAKE_EXE_LINKER_FLAGS}")
    endif (BUILD_WIN32)
endif (MSVC)

if (NOT CMAKE_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "-O2 -ggdb -march=native")
endif (NOT CMAKE_CXX_FLAGS)

# This only works in cmake >3.1
set(CMAKE_CXX_STANDARD 14)

list(APPEND inspectrum_sources
    abstractsamplesource.cpp
    aibridge.cpp
    aidock.cpp
    amplitudedemod.cpp
    annotationdialog.cpp
    cursor.cpp
    cursors.cpp
    fft.cpp
    exportfiles.cpp
    frequencydemod.cpp
    fskdemod.cpp
    fskpolarplot.cpp
    histogramplot.cpp
    mainwindow.cpp
    inputsource.cpp
    phasedemod.cpp
    plot.cpp
    plots.cpp
    plotview.cpp
    plugin.cpp
    samplebuffer.cpp
    samplesource.cpp
    spectrogramcontrols.cpp
    spectrogramplot.cpp
    spectrumview.cpp
    threshold.cpp
    traceplot.cpp
    tuner.cpp
    tunertransform.cpp
    util.cpp
)

find_package(Qt5Widgets REQUIRED)
find_package(Qt5Concurrent REQUIRED)
find_package(Qt5Network REQUIRED)
find_package(FFTW REQUIRED)
find_package(Liquid REQUIRED)

# Optional: libzstd enables transparent loading of .zst-compressed inputs
# (e.g. a zstd-compressed SigMF archive). The build still succeeds without it;
# opening a .zst file then reports that zstd support was not compiled in.
find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(ZSTD_LIBRARY NAMES zstd)
if (ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    message(STATUS "zstd found (${ZSTD_LIBRARY}) — enabling transparent .zst input")
    add_definitions(-DHAVE_ZSTD)
    set(ZSTD_INCLUDES ${ZSTD_INCLUDE_DIR})
    set(ZSTD_LIBRARIES ${ZSTD_LIBRARY})
else()
    message(STATUS "zstd not found — .zst inputs disabled (install libzstd-dev to enable)")
endif()

include_directories(
    ${FFTW_INCLUDES}
    ${LIQUID_INCLUDES}
    ${ZSTD_INCLUDES}
)

add_library(inspectrum_core STATIC ${inspectrum_sources})
target_include_directories(inspectrum_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(inspectrum_core PRIVATE INSPECTRUM_AI_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tools/ai")
add_executable(inspectrum ${EXE_ARGS} main.cpp)
target_link_libraries(inspectrum inspectrum_core)

target_link_libraries(inspectrum_core PUBLIC
    Qt5::Core Qt5::Widgets Qt5::Concurrent Qt5::Network
    ${FFTW_LIBRARIES}
    ${LIQUID_LIBRARIES}
    ${ZSTD_LIBRARIES}
)

if(BUILD_TESTING)
    find_package(Qt5Test REQUIRED)
    add_executable(core_tests ../tests/core_tests.cpp)
    target_link_libraries(core_tests inspectrum_core Qt5::Test)
    add_test(NAME core_regressions COMMAND core_tests)
    set_tests_properties(core_regressions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(ai_tests ../tests/ai_tests.cpp)
    target_link_libraries(ai_tests inspectrum_core Qt5::Test)
    add_test(NAME ai_gui_regressions COMMAND ai_tests)
    set_tests_properties(ai_gui_regressions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy"
                    RESULT_VARIABLE numpy_missing)
    if(numpy_missing)
        message(FATAL_ERROR "Tests require Python 3 + numpy; install numpy or use -DBUILD_TESTING=OFF")
    endif()
    add_test(NAME fsk_regressions
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/examples/plugins/test_fsk_analyze.py)
    set_tests_properties(fsk_regressions PROPERTIES TIMEOUT 180 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    add_test(NAME ai_mcp_integration COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test_ai_bridge.py $<TARGET_FILE:inspectrum>)
    set_tests_properties(ai_mcp_integration PROPERTIES TIMEOUT 90 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

set(INSTALL_DEFAULT_BINDIR "bin" CACHE STRING "Appended to CMAKE_INSTALL_PREFIX")

install(TARGETS inspectrum RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
install(FILES ../tools/ai/inspectrum_mcp.py
              ../examples/plugins/fsk-analyze.py ../examples/plugins/energy-detect.py
        DESTINATION share/inspectrum/ai)

# Create uninstall target
configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
@ONLY)

add_custom_target(uninstall
    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)

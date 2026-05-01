list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

find_package(Liquid REQUIRED)

if (NOT CMAKE_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "-O2 -ggdb -march=native")
endif()
set(CMAKE_CXX_STANDARD 14)

include_directories(${LIQUID_INCLUDES})

add_executable(fm_filter_compare fm_filter_compare.cpp)
target_link_libraries(fm_filter_compare ${LIQUID_LIBRARIES} m)

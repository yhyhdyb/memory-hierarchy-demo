cmake_minimum_required(VERSION 3.10)
project(memory_hierarchy_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/O2 /EHsc /utf-8 /W3)
else()
  add_compile_options(-O2 -Wall -Wextra)
endif()

add_executable(memhier
  src/main.cpp
  src/cache_hierarchy.cpp
  src/ram_vs_disk.cpp
  src/dma_sim.cpp
)

if(NOT MSVC)
  find_package(Threads REQUIRED)
  target_link_libraries(memhier PRIVATE Threads::Threads)
endif()

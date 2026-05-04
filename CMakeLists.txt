cmake_minimum_required(VERSION 3.24)

project(obs-wmp-legacy VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED CONFIG)

add_library(obs-wmp-legacy MODULE
  src/plugin-main.cpp
  src/wmp_monitor.cpp
  src/wmp_source.cpp
)

target_include_directories(obs-wmp-legacy PRIVATE src)
target_compile_definitions(obs-wmp-legacy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(obs-wmp-legacy
  PRIVATE
    OBS::libobs
    user32
    ole32
    oleaut32
    comsuppw
)

set_target_properties(obs-wmp-legacy PROPERTIES
  PREFIX ""
)

install(TARGETS obs-wmp-legacy
  RUNTIME DESTINATION obs-plugins/64bit
  LIBRARY DESTINATION obs-plugins/64bit
)

cmake_minimum_required(VERSION 3.24)

project(obs-wmp-smtc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED CONFIG)

add_library(obs-wmp-smtc MODULE
  src/plugin-main.cpp
  src/smtc_monitor.cpp
  src/wmp_source.cpp
)

target_include_directories(obs-wmp-smtc PRIVATE src)
target_compile_definitions(obs-wmp-smtc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(obs-wmp-smtc
  PRIVATE
    OBS::libobs
    windowsapp
)

set_target_properties(obs-wmp-smtc PROPERTIES
  PREFIX ""
)

install(TARGETS obs-wmp-smtc
  RUNTIME DESTINATION obs-plugins/64bit
  LIBRARY DESTINATION obs-plugins/64bit
)

cmake_minimum_required(VERSION 3.24)

project(obs-wmp-legacy VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED CONFIG)

# ---- OBS Plugin (DLL) ----
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
    shell32
    advapi32
)

set_target_properties(obs-wmp-legacy PROPERTIES
  PREFIX ""
)

# ---- WMP Bridge helper (standalone exe for reliable COM access) ----
add_executable(wmp_bridge src/wmp_bridge.cpp)
target_compile_definitions(wmp_bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wmp_bridge PRIVATE user32 ole32 oleaut32 comsuppw)

# ---- Install rules ----
install(TARGETS obs-wmp-legacy
  RUNTIME DESTINATION obs-plugins/64bit
  LIBRARY DESTINATION obs-plugins/64bit
)

install(TARGETS wmp_bridge
  RUNTIME DESTINATION obs-plugins/64bit
)

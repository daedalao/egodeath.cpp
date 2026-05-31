cmake_minimum_required(VERSION 3.20)
project(egodeath LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-O3 -mcpu=power8 -mtune=power8)
endif()

find_package(Threads REQUIRED)
find_package(nlohmann_json QUIET)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(NCURSES REQUIRED ncursesw)
pkg_check_modules(GIT2 REQUIRED libgit2)
pkg_check_modules(FMT REQUIRED fmt)

include_directories(src ${NCURSES_INCLUDE_DIRS} ${GIT2_INCLUDE_DIRS} ${FMT_INCLUDE_DIRS})

add_executable(egodeath
    src/main.cpp
    src/common.cpp
    src/client.cpp
    src/agent.cpp
    src/tools.cpp
    src/tui.cpp
    src/memory.cpp
    src/sys_monitor.cpp
)

target_link_libraries(egodeath 
    PRIVATE 
    Threads::Threads 
    CURL::libcurl 
    ${NCURSES_LIBRARIES}
    ${GIT2_LIBRARIES}
    ${FMT_LIBRARIES}
)

if(nlohmann_json_FOUND)
    target_link_libraries(egodeath PRIVATE nlohmann_json::nlohmann_json)
endif()

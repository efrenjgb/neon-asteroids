cmake_minimum_required(VERSION 3.24)
project(neon_asteroids CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# raylib 5.5 and its vendored glfw declare cmake_minimum_required() below 3.5,
# which CMake 4 rejects outright. This is the documented escape hatch.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(raylib)

# ENet: UDP transport for networked play. Small portable C library; its own
# CMake links winmm/ws2_32 on Windows. The `enet` target it defines uses the
# directory-scoped include path, so consumers add ${enet_SOURCE_DIR}/include.
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(enet)

add_executable(neon_asteroids
    src/main.cpp
    src/game.cpp
    src/entities.cpp
    src/postfx.cpp
    src/audio.cpp
)

target_link_libraries(neon_asteroids PRIVATE raylib)

# Shaders are read from the source tree so edits take effect on the next run
# without a rebuild or a copy step.
target_compile_definitions(neon_asteroids PRIVATE
    SHADERS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
)

# Diagnostic tool, not part of the game. Talks to GLFW directly as well as
# through raylib, to tell a missing controller mapping apart from the OS not
# delivering input at all.
add_executable(gamepad_probe tools/gamepad_probe.cpp)
target_link_libraries(gamepad_probe PRIVATE raylib glfw)
target_include_directories(gamepad_probe PRIVATE
    ${raylib_SOURCE_DIR}/src/external/glfw/include
)

# Headless unit test for the network serialization. Links raylib only for the
# Vector2 struct definition; opens no window and runs in CI.
add_executable(net_selftest tools/net_selftest.cpp)
target_link_libraries(net_selftest PRIVATE raylib)

# Localhost connect proof for the NetLink layer. Two processes (host + client)
# exchange messages over loopback. No window.
add_executable(net_conntest tools/net_conntest.cpp src/net.cpp)
target_link_libraries(net_conntest PRIVATE enet raylib)
target_include_directories(net_conntest PRIVATE ${enet_SOURCE_DIR}/include)

if(APPLE)
    target_link_libraries(neon_asteroids PRIVATE
        "-framework IOKit"
        "-framework Cocoa"
        "-framework OpenGL"
    )
endif()

# Packaging: `cmake --install build --prefix <dir>` stages the game and its
# shaders together, so the shipped binary finds shaders/ next to itself. The
# probe is a dev tool and is deliberately not installed.
install(TARGETS neon_asteroids RUNTIME DESTINATION .)
install(DIRECTORY shaders DESTINATION .)

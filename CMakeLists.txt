cmake_minimum_required(VERSION 3.16)
project(Exodus LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

# Static linking configuration
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)

include_directories(include k-module)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(SERVER_BIN_DIR ${CMAKE_SOURCE_DIR}/s-bin)

find_package(Threads REQUIRED)
find_package(Curses REQUIRED)
find_library(M_LIB m)
find_library(Z_LIB z)

add_library(ctz_json OBJECT src/ctz-json.c)
add_library(ctz_set OBJECT src/ctz-set.c)
add_library(cortez_mesh OBJECT src/cortez-mesh.c)
add_library(cortez_ipc OBJECT src/cortez_ipc.c)

add_executable(exctl src/exctl.c $<TARGET_OBJECTS:ctz_json>)

add_executable(exodus src/exodus.c 
    $<TARGET_OBJECTS:cortez_mesh> 
    $<TARGET_OBJECTS:ctz_json> 
    $<TARGET_OBJECTS:cortez_ipc> 
    $<TARGET_OBJECTS:ctz_set>
)
target_link_libraries(exodus PRIVATE Threads::Threads)

add_executable(exodus_snapshot src/exodus-anchor-weaver.c 
    $<TARGET_OBJECTS:cortez_ipc> 
    $<TARGET_OBJECTS:ctz_json>
)
target_link_libraries(exodus_snapshot PRIVATE ${M_LIB} ${Z_LIB})

add_executable(cloud_daemon src/exodus-cloud-daemon.c 
    $<TARGET_OBJECTS:cortez_mesh> 
    $<TARGET_OBJECTS:ctz_json>
)
target_link_libraries(cloud_daemon PRIVATE Threads::Threads)

add_executable(exodus-node-guardian src/exodus-node-guardian.c 
    $<TARGET_OBJECTS:ctz_json>
)
target_link_libraries(exodus-node-guardian PRIVATE Threads::Threads)

add_executable(query_daemon src/exodus-query-daemon.c 
    $<TARGET_OBJECTS:cortez_mesh>
)
target_link_libraries(query_daemon PRIVATE Threads::Threads)

find_library(TINFO_LIB tinfo)

add_executable(exodus-tui src/exodus-tui.c $<TARGET_OBJECTS:ctz_json>)
target_link_libraries(exodus-tui PRIVATE ${CURSES_LIBRARIES} ${TINFO_LIB})

add_executable(node-editor src/node-editor.c)
target_link_libraries(node-editor PRIVATE ${CURSES_LIBRARIES} ${TINFO_LIB})

add_executable(exodus-signal src/exodus-signal.c 
    $<TARGET_OBJECTS:cortez_mesh> 
    $<TARGET_OBJECTS:ctz_json> 
    $<TARGET_OBJECTS:ctz_set>
)
target_link_libraries(exodus-signal PRIVATE Threads::Threads)

add_executable(exodus-coordinator server-side/exodus-coordinator.c $<TARGET_OBJECTS:ctz_json>)
target_link_libraries(exodus-coordinator PRIVATE Threads::Threads)
set_target_properties(exodus-coordinator PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SERVER_BIN_DIR})

add_custom_target(module
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${CMAKE_SOURCE_DIR}/k-module
)

add_custom_target(kmod_only
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${CMAKE_SOURCE_DIR}/k-module module
)

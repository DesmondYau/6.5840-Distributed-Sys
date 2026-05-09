cmake_minimum_required(VERSION 3.20)
project(6.8540_lab4)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest REQUIRED)


# ==================== Source files ====================

# Lab3 files (reuse from previous lab)
set(LAB3_SOURCES
    ../Lab3_Raft/src/raft.cpp
    ../Lab3_Raft/src/persister.cpp
    ../Lab3_Raft/src/helper.cpp
    ../Lab3_Raft/src/rpc/labrpc.cpp
)

# Lab4 files
file(GLOB LAB4_SOURCES
    "tests/*.cpp"
    "tests/porcupine/*.cpp"
    "src/*.cpp"
)

add_executable(kvRaft_tests
    ${LAB4_SOURCES}
    ${LAB3_SOURCES}
)

# ==================== Include directories ====================

target_include_directories(kvRaft_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../Lab3_Raft/include   
    ${CMAKE_SOURCE_DIR}/../Lab3_Raft/src             
)

# ==================== Linking ====================
target_link_libraries(kvRaft_tests PRIVATE GTest::gtest GTest::gtest_main pthread)


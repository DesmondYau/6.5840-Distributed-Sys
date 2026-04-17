cmake_minimum_required(VERSION 3.20)
project(6.824_lab2)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest REQUIRED)
include_directories(${CMAKE_SOURCE_DIR}/include)




#  executable
add_executable(raft_tests
    tests/raft_tests.cpp
    src/persister.cpp
    src/config.cpp
    src/rpc/labrpc.cpp
    src/raft.cpp
    src/helper.cpp
)
target_link_libraries(raft_tests PRIVATE GTest::gtest GTest::gtest_main pthread)



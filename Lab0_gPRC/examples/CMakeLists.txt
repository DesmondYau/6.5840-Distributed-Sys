cmake_minimum_required(VERSION 3.20)
project(myserver)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)


# Declare a new library target called protolib (using proto/hello.prot as source file)
add_library(protolib proto/hello.proto)
# Links the protolib target against the gRPC C++ library. This ensures the generated gRPC service stubs can call into gRPC’s APIs.
target_link_libraries(protolib gRPC::grpc++)
# Add build directory to compiler's include path so that we can use .pb.h and grpc.pb.h in our code.
# PUBLIC means any target that links against protolib (like our server executable) will also inherit this include path automatically
target_include_directories(protolib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
# Run the Protobuf compiler (protoc) to generate message classes (.pb.cc and .pb.h files) from hello.proto 
protobuf_generate(TARGET protolib 
                  LANGUAGE cpp
                 )
# Find filesystem path for grpc_cpp_plugin binary. gRPC requires the extra plugin to generate the service stubs
get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
# Run protoc with the gRPC plugin to generate service stubs (.grpc.pb.h/.grpc.pb.cc) from hello.proto.
protobuf_generate(TARGET protolib 
                  LANGUAGE grpc
                  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
                  PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}")



# Server executable
add_executable(server src/server.cpp)
# Link the server against protolib so it can use the generated message classes and service stubs.
target_link_libraries(server PRIVATE protolib)

# Client executable
add_executable(client src/client.cpp)
# Link the client against protolib so it can use the generated message classes and service stubs.
target_link_libraries(client PRIVATE protolib)


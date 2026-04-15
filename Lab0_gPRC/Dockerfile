FROM ubuntu:latest

RUN apt-get update && apt-get install -y cmake build-essential git

WORKDIR /deps
RUN git clone --recurse-submodules -b v1.62.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
RUN mkdir -p /deps/grpc/build && \
    cd /deps/grpc/build && \
    cmake -DgRPC_INSTALL=ON \
          -DgRPC_BUILD_TESTS=OFF \
          .. && \
    make -j2 install
    
# Build and install GoogleTest
RUN git clone --depth 1 --branch v1.14.0 https://github.com/google/googletest.git
RUN mkdir -p /deps/googletest/build && \
    cd /deps/googletest/build && \
    cmake .. && \
    make -j2 install


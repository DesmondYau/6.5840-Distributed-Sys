#include <grpcpp/grpcpp.h>
#include "proto/hello.grpc.pb.h"
#include "proto/hello.pb.h"
#include <iostream>

int main() {
    // 1. Create a channel to connect to the server
    auto channel = grpc::CreateChannel("localhost:9999", grpc::InsecureChannelCredentials());

    // 2. Create a stub (client) for the ComputeService
    std::unique_ptr<ComputeService::Stub> stub = ComputeService::NewStub(channel);

    // 3. Prepare the request
    HelloRequest request;
    request.set_a(10);
    request.set_b(20);
    request.set_c(5);

    // 4. Prepare the response and context
    HelloResponse response;
    grpc::ClientContext context;

    // 5. Call the RPC
    grpc::Status status = stub->computeSum(&context, request, &response);

    // 6. Handle the result
    if (status.ok()) {
        std::cout << "Sum = " << response.a() << std::endl;
    } else {
        std::cerr << "RPC failed: " << status.error_message() << std::endl;
    }

    return 0;
}

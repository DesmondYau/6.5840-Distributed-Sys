#include <grpcpp/grpcpp.h>
#include "proto/hello.grpc.pb.h"
#include "proto/hello.pb.h"
#include <memory>


class ComputeImpl : public ComputeService::Service
{
    ::grpc::Status computeSum(::grpc::ServerContext* context, const ::HelloRequest* request, ::HelloResponse* response)
    {
        response->set_a(request->a() + request->b() + request->c());
        return grpc::Status::OK;
    }
};

int main()
{
    ComputeImpl service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:9999", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    server->Wait();

    return 0;
}
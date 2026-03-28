#include <string>
#include <iostream>
#include "buttonrpc.hpp"

void foo_1() 
{
    std::cout << "foo_1" << std::endl;
}

void foo_2(int arg1)
{
    std::cout << arg1 << std::endl;
}

int foo_3 (int arg1)
{
    return arg1;
}

int main()
{
    buttonrpc server;
    server.as_server(5555);

    server.bind("foo_1", foo_1);
    server.bind("foo_2", foo_2);
    server.bind("foo_3", foo_3);
    
    std::cout << "run rpc server on: " << 5555 << std::endl;
	server.run();

    return 0;
}
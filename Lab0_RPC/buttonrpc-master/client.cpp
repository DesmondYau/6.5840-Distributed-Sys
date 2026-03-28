#include <iostream>
#include "buttonrpc.hpp"

int main()
{
	buttonrpc client;
	client.as_client("127.0.0.1", 5555);
	
    client.call<void>("foo_1");
    client.call<void>("foo_2", 10);

    int foo3 = client.call<int>("foo_3", 10).val();
    std::cout << foo3 << std::endl;

	return 0;
}
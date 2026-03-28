#include <iostream>
#include <fstream>
#include <sstream>
#include <dlfcn.h>
#include <vector>
#include <algorithm>


struct KeyValuePair 
{
    std::string key_;
    std::string value_;
};

using MapFunc = std::vector<KeyValuePair>(*)(const std::string&, const std::string&);
using ReduceFunc = std::string(*)(const std::string&, std::vector<std::string>&);

int main(int argc, char* argv[]) 
{
    if (argc < 3) 
    {
        std::cerr << "Usage: ./mrsequential ./xxx.so ./pg*txt";
        return 1;
    }

    // Load Shared Object files
    void* handle = dlopen(argv[1], RTLD_LAZY);
    if (!handle) 
    {
        std::cerr << "Cannot load share object files " << argv[1] << std::endl;
        return 1;
    }

    // Find location of Map and Reduce functions
    MapFunc mapFunc = (MapFunc)dlsym(handle, "Map");
    ReduceFunc reduceFunc = (ReduceFunc)dlsym(handle, "Reduce");
    if (!mapFunc || !reduceFunc) 
    {
        std::cerr << "Could not find Map or Reduce function in shared object file" << std::endl;
        return 1;
    }

    // Read each input file, pass it to Map function and get intermediate output
    std::vector<KeyValuePair> intermediate {};
    for (int i=2; i<argc; i++) 
    {
        std::ifstream inf { argv[i] };
        if (!inf)
        {
            std::cerr << "Cannot open file " << argv[i] << std::endl;
            continue;
        }
        std::stringstream buffer {};
        buffer << inf.rdbuf();
        inf.close();

        auto kva = mapFunc(argv[i], buffer.str());
        for (const auto& kv : kva)
            intermediate.push_back(kv);
    }

    // Sort intermediate by key
    std::sort(
        intermediate.begin(), intermediate.end(),
        [](const KeyValuePair& a, const KeyValuePair& b) {
            return a.key_ < b.key_;
        }
    );

    // Open output file
    std::ofstream ofile("mr-out-0");

    // Run Reduce on each distinct key
    for (size_t i = 0; i < intermediate.size();) {
        size_t j = i + 1;
        while (j < intermediate.size() && intermediate[j].key_ == intermediate[i].key_) {
            j++;
        }
        std::vector<std::string> values;
        for (size_t k = i; k < j; k++) {
            values.push_back(intermediate[k].value_);
        }
        std::string output = reduceFunc(intermediate[i].key_, values);
        ofile << intermediate[i].key_ << " " << output << "\n";
        i = j;
    }

    ofile.close();
    dlclose(handle);
    return 0;
}
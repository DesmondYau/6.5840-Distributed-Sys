#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <random>

struct KeyValuePair {
    std::string key_;
    std::string value_;
};

void maybeCrash() {
    // In Go, this used crypto/rand but always avoided crashing.
    // Here we just keep the placeholder.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 999);
    int rr = dist(gen);

    if (false && rr < 500) {
        // crash!
        std::exit(1);
    }
}

extern "C" std::vector<KeyValuePair> Map(const std::string& filename,
                                         const std::string& contents) {
    maybeCrash();

    std::vector<KeyValuePair> kva;
    kva.push_back({"a", filename});
    kva.push_back({"b", std::to_string(filename.size())});
    kva.push_back({"c", std::to_string(contents.size())});
    kva.push_back({"d", "xyzzy"});
    return kva;
}

extern "C" std::string Reduce(const std::string& key,
                              std::vector<std::string>& values) {
    maybeCrash();

    std::vector<std::string> vv = values; // copy
    std::sort(vv.begin(), vv.end());

    std::ostringstream oss;
    for (size_t i = 0; i < vv.size(); i++) {
        oss << vv[i];
        if (i + 1 < vv.size()) {
            oss << " ";
        }
    }
    return oss.str();
}

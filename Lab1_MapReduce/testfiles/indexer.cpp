#include <string>
#include <vector>
#include <unordered_set>
#include <sstream>
#include <algorithm>

struct KeyValuePair {
    std::string key_;
    std::string value_;
};

// Map: produce (word, document) pairs for each unique word in the file
extern "C" std::vector<KeyValuePair> Map(const std::string& document,
                                         const std::string& contents) {
    std::unordered_set<std::string> uniqueWords;
    std::string word;

    for (char c : contents) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            word.push_back(c);
        } else {
            if (!word.empty()) {
                uniqueWords.insert(word);
                word.clear();
            }
        }
    }
    if (!word.empty()) {
        uniqueWords.insert(word);
    }

    std::vector<KeyValuePair> kva;
    for (const auto& w : uniqueWords) {
        kva.push_back({w, document});
    }
    return kva;
}

// Reduce: combine document list for each word
extern "C" std::string Reduce(const std::string& key,
                              std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    std::ostringstream oss;
    oss << values.size() << " ";
    for (size_t i = 0; i < values.size(); i++) {
        oss << values[i];
        if (i + 1 < values.size()) {
            oss << ",";
        }
    }
    return oss.str();
}

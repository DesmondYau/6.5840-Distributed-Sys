#include <string>
#include <vector>


struct KeyValuePair 
{
    std::string key_;
    std::string value_;
};

extern "C" std::vector<KeyValuePair> Map(const std::string& filename, const std::string& contents)
{
    std::vector<KeyValuePair> kva;
    std::string word;
    for (char c : contents) 
    {
        if (std::isalpha(static_cast<unsigned char>(c))) 
        {
            word.push_back(c);
        }
        else 
        {
            if (!word.empty()) 
            {
                kva.push_back({ word, "1" });
                word.clear();
            }
        }
    }
    if (!word.empty()) 
    {
        kva.push_back({ word, "1" });
    }

    return kva;
}

extern "C" std::string Reduce(const std::string& key, std::vector<std::string>& values) 
{
    return std::to_string(values.size());
}
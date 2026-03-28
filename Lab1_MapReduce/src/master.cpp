#include<iostream>

class Master
{
public:
    Master(int mapNum, int reduceNum);

private:
    int mapNum_;                                // number of map tasks
    int reduceNum_;                             // number of reduce tasks

};

Master::Master(int mapNum, int reduceNum)
    : mapNum_ { mapNum }
    , reduceNum_ { reduceNum }
{}


int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout << "Missing parameter! Input format is './Master ../testfiles/pg*.txt'" << std::endl;
    }

    return 0;
}
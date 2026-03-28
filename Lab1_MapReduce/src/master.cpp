#include<iostream>
#include<vector>

class Master
{
public:
    Master(int mapNum, int reduceNum);
    void getAllFiles(int argc, char* argv[]);
    int getMapNum();
    int getReduceNum();


private:
    int mapNum_;                                // number of map tasks
    int reduceNum_;                             // number of reduce tasks
    std::vector<std::string> files_;            // vector storing all input files

};

Master::Master(int mapNum, int reduceNum)
    : mapNum_ { mapNum }
    , reduceNum_ { reduceNum }
{}

void Master::getAllFiles(int argc, char* argv[])
{
    for (int i=2; i<argc; i++)
    {
        files_.emplace_back(argv[i]);
    }
}

int Master::getMapNum() {
    return mapNum_;
}

int Master::getReduceNum() {
    return reduceNum_;
}


int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout << "Missing parameter! Input format is './master ../testfiles/pg*.txt'" << std::endl;
    }

    Master master { 8, 8 };
    master.getAllFiles(argc, argv);


    return 0;
}
#include "pch.h"
#include "LoadTesterConfig.h"
#include "LoadTester.h"

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        LoadTesterConfig config = LoadTesterConfig::Parse(argc, argv);
        LoadTester tester(std::move(config));
        tester.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LoadTester] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

#include "pch.h"
#include "LoadTesterConfig.h"

namespace
{
    // "--flag value" 형태에서 다음 토큰을 값으로 소비. 다음 토큰이 없으면 예외.
    String NextValue(int argc, char** argv, int& i, const String& flag)
    {
        if (i + 1 >= argc)
            throw std::runtime_error("LoadTesterConfig: " + flag + " requires a value");
        return String(argv[++i]);
    }
}

LoadTesterConfig LoadTesterConfig::Parse(int argc, char** argv)
{
    LoadTesterConfig config;

    for (int i = 1; i < argc; ++i)
    {
        String arg = argv[i];

        if (arg == "--agents")
            config.agentCount = std::stoi(NextValue(argc, argv, i, arg));
        else if (arg == "--interval-ms")
            config.sendInterval = std::chrono::milliseconds(std::stoll(NextValue(argc, argv, i, arg)));
        else if (arg == "--duration-sec")
            config.duration = std::chrono::seconds(std::stoll(NextValue(argc, argv, i, arg)));
        else if (arg == "--ramp-up-ms")
            config.rampUp = std::chrono::milliseconds(std::stoll(NextValue(argc, argv, i, arg)));
        else if (arg == "--queue-size")
            config.queueSize = static_cast<size_t>(std::stoull(NextValue(argc, argv, i, arg)));
        else if (arg == "--collector-host")
            config.collectorHost = NextValue(argc, argv, i, arg);
        else if (arg == "--collector-port")
            config.collectorPort = static_cast<unsigned short>(std::stoi(NextValue(argc, argv, i, arg)));
        else if (arg == "--csv-out")
            config.csvOutputPath = NextValue(argc, argv, i, arg);
        else if (arg == "--key-file")
            config.keyFilePath = NextValue(argc, argv, i, arg);
        else
            throw std::runtime_error("LoadTesterConfig: unknown argument '" + arg + "'");
    }

    if (config.agentCount <= 0)
        throw std::runtime_error("LoadTesterConfig: --agents must be > 0");

    return config;
}

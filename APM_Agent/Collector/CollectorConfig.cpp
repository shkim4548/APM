#include "pch.h"
#include "CollectorConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>

CollectorConfig LoadCollectorConfig(const String& path)
{
    CollectorConfig config;

    std::ifstream file(path);
    if (!file.is_open())
        return config;

    nlohmann::json json;
    file >> json;

    if (json.contains("webserver_host"))
        config.webServerHost = json["webserver_host"].get<String>();
    if (json.contains("webserver_port"))
        config.webServerPort = json["webserver_port"].get<unsigned short>();
    if (json.contains("push_interval_seconds"))
        config.pushIntervalSeconds = json["push_interval_seconds"].get<int>();

    return config;
}

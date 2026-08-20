#include "config.h"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string expandPath(std::string value)
{
    const char *home = std::getenv("HOME");
    if (!home || *home == 0)
        return value;

    std::size_t position = 0;
    while ((position = value.find("$HOME", position)) != std::string::npos) {
        value.replace(position, 5, home);
        position += std::char_traits<char>::length(home);
    }
    if (value == "~")
        value = home;
    else if (value.starts_with("~/"))
        value = std::string(home) + "/" + value.substr(2);
    if (!value.empty() && !fs::path(value).is_absolute())
        value = (fs::path(home) / value).string();
    return value;
}

std::string configPath()
{
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return (fs::path(xdg) / "toma/toma.conf").string();
    const char *home = std::getenv("HOME");
    return home ? (fs::path(home) / ".config/toma/toma.conf").string() : std::string();
}

std::map<std::string, std::string> readIni()
{
    std::map<std::string, std::string> values;
    const std::string path = configPath();
    if (path.empty())
        return values;
    std::ifstream stream(path);
    if (!stream)
        return values;

    std::string section;
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, separator));
        std::string value = trim(line.substr(separator + 1));
        if (key.empty())
            continue;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        values[section.empty() ? key : section + "/" + key] = value;
    }
    return values;
}

std::string valueFor(const std::map<std::string, std::string> &values,
                     std::string_view key, std::string fallback = {})
{
    const auto iterator = values.find(std::string(key));
    return iterator == values.end() ? std::move(fallback) : iterator->second;
}

std::string choiceValue(std::string value, std::initializer_list<std::string_view> choices,
                        std::string fallback)
{
    value = trim(std::move(value));
    for (const auto choice : choices)
        if (value == choice)
            return value;
    return fallback;
}

unsigned int unsignedValue(const std::map<std::string, std::string> &values,
                           std::string_view key, unsigned int fallback,
                           unsigned int minimum, unsigned int maximum)
{
    const std::string value = valueFor(values, key);
    unsigned int result = fallback;
    if (!value.empty()) {
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
            result = fallback;
    }
    return result < minimum ? minimum : result > maximum ? maximum : result;
}

bool booleanValue(const std::map<std::string, std::string> &values,
                  std::string_view key, bool fallback)
{
    const std::string value = trim(valueFor(values, key));
    if (value == "true" || value == "1" || value == "yes")
        return true;
    if (value == "false" || value == "0" || value == "no")
        return false;
    return fallback;
}
}

TomaConfig loadTomaConfig()
{
    const auto values = readIni();
    TomaConfig config;
    config.screenshotsPath = expandPath(valueFor(values, "Paths/screenshots"));
    config.recordingsPath = expandPath(valueFor(values, "Paths/recordings"));
    config.recordingFormat = choiceValue(valueFor(values, "Recording/format"), {"mp4", "mkv"}, "mp4");
    config.recordingPreset = choiceValue(valueFor(values, "Recording/preset"), {"low", "balanced", "high"}, "balanced");
    config.recordingFramerate = unsignedValue(values, "Recording/framerate", 60, 15, 360);
    config.recordingCountdown = unsignedValue(values, "Recording/countdown", 0, 0, 10);
    config.audioEnabled = booleanValue(values, "Audio/enabled", false);
    config.audioDevice = trim(valueFor(values, "Audio/device", "default"));
    if (config.audioDevice.empty())
        config.audioDevice = "default";
    return config;
}

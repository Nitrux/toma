// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 <Nitrux Latinoamericana S.C. <hello@nxos.org>>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace {

struct PipeCloser {
    void operator()(FILE* pipe) const noexcept {
        if (pipe) {
            pclose(pipe);
        }
    }
};

enum class CaptureMode { Full, Select, Window };

// Quote one value for use as a POSIX shell argument.
std::string shellQuote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool debugEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TOMA_DEBUG");
        return value && std::string_view(value) != "0";
    }();
    return enabled;
}

void debugLog(std::string_view message) {
    if (debugEnabled()) {
        std::cerr << "[toma] " << message << std::endl;
    }
}

// Execute a shell command and return its standard output.
std::string execCommand(std::string_view command) {
    std::array<char, 256> buffer{};
    std::string result;
    const std::string commandString(command);
    debugLog(std::format("capture command: {}", commandString));
    std::unique_ptr<FILE, PipeCloser> pipe(popen(commandString.c_str(), "r"));
    if (!pipe) {
        throw std::runtime_error("popen() failed");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result.append(buffer.data());
    }
    const std::string output = trim(std::move(result));
    debugLog(std::format("capture output: {}", output));
    return output;
}

int runCommand(std::string_view command) {
    const std::string commandString(command);
    debugLog(std::format("command: {}", commandString));
    const int status = std::system(commandString.c_str());
    debugLog(std::format("command status: {}", status));
    return status;
}

struct OpenAction {
    std::string label;
    std::string executable;
};

bool commandAvailable(std::string_view command) {
    return runCommand(std::format("command -v {} >/dev/null 2>&1", shellQuote(command))) == 0;
}

OpenAction getOpenAction(std::string_view captureType) {
    const bool isScreenshot = captureType == "screenshot";
    const std::string application = isScreenshot ? "pix" : "clip";
    if (commandAvailable(application)) {
        return {std::format("Open {}", isScreenshot ? "Pix" : "Clip"), application};
    }
    return {"Open", "xdg-open"};
}

void notify(std::string_view urgency, std::string_view icon, std::string_view title,
            std::string_view body, std::string_view actionLabel = {},
            std::string_view opener = {}, std::string_view file = {}) {
    const std::string notification = std::format("notify-send -a {} -u {} -i {} {} {}",
                                                  shellQuote(title), shellQuote(urgency),
                                                  shellQuote(icon), shellQuote(title), shellQuote(body));
    if (actionLabel.empty() || opener.empty() || file.empty()) {
        runCommand(notification);
        return;
    }

    const std::string openCommand = std::format("{} {}", shellQuote(opener), shellQuote(file));
    const std::string actionNotification = std::format("{} --action={}", notification,
                                                        shellQuote(std::format("default={}", actionLabel)));
    runCommand(std::format("(action=$({}); if [ \"$action\" = default ]; then {}; fi) >/dev/null 2>&1 &",
                           actionNotification, openCommand));
}

fs::path getOutputDir(std::string_view xdgType, std::string_view fallbackFolder) {
    std::string pathString = execCommand(std::format("xdg-user-dir {}", shellQuote(xdgType)));
    if (pathString.empty()) {
        const char* homeDirectory = std::getenv("HOME");
        pathString = homeDirectory ? std::format("{}/{}", homeDirectory, fallbackFolder) : "/tmp";
    }

    fs::path outputDirectory(pathString);
    std::error_code error;
    fs::create_directories(outputDirectory, error);
    if (error) {
        throw std::system_error(error, std::format("Could not create {}", outputDirectory.string()));
    }
    return outputDirectory;
}

std::string generateFilename(std::string_view prefix, std::string_view extension) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeNow = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&timeNow, &localTime);

    char timeString[32]{};
    if (std::strftime(timeString, sizeof(timeString), "%Y%m%d-%H%M%S", &localTime) == 0) {
        throw std::runtime_error("Could not format timestamp");
    }
    return std::format("{}-{}.{}", prefix, timeString, extension);
}

fs::path uniqueOutputPath(const fs::path& directory, std::string_view prefix,
                          std::string_view extension) {
    const fs::path base = directory / generateFilename(prefix, extension);
    fs::path candidate = base;

    for (unsigned int suffix = 1;; ++suffix) {
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            if (error) {
                throw std::system_error(error, std::format("Could not inspect {}", candidate.string()));
            }
            return candidate;
        }
        candidate = directory / std::format("{}-{}.{}", base.stem().string(), suffix, extension);
    }
}

std::string getGeometry(CaptureMode mode) {
    if (mode == CaptureMode::Select) {
        return execCommand("slurp 2>/dev/null");
    }
    if (mode != CaptureMode::Window) {
        return {};
    }

    const std::string workspace = execCommand("hyprctl activeworkspace -j | jq -r '.id'");
    unsigned int workspaceId = 0;
    const auto [end, parseError] = std::from_chars(workspace.data(), workspace.data() + workspace.size(), workspaceId);
    if (parseError != std::errc{} || end != workspace.data() + workspace.size()) {
        return {};
    }

    constexpr std::string_view jqFilter =
        ".[] | select(.workspace.id == $workspaceId and .size[0] > 2 and .size[1] > 2) "
        "| \"\\(.at[0]),\\(.at[1]) \\(.size[0])x\\(.size[1])\"";
    return execCommand(std::format("hyprctl clients -j | jq -r --argjson workspaceId {} {} | slurp 2>/dev/null",
                                   workspaceId, shellQuote(jqFilter)));
}

// Validate a PNG and reject captures smaller than 3x3 without file(1)/sed.
bool validateCapture(const fs::path& image) {
    std::error_code error;
    if (!fs::is_regular_file(image, error) || error || fs::file_size(image, error) < 24 || error) {
        return false;
    }

    std::ifstream stream(image, std::ios::binary);
    std::array<std::uint8_t, 24> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size())) {
        return false;
    }

    constexpr std::array<std::uint8_t, 8> pngSignature{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (!std::equal(pngSignature.begin(), pngSignature.end(), header.begin())) {
        return false;
    }

    const std::uint32_t width = (static_cast<std::uint32_t>(header[16]) << 24)
        | (static_cast<std::uint32_t>(header[17]) << 16)
        | (static_cast<std::uint32_t>(header[18]) << 8)
        | static_cast<std::uint32_t>(header[19]);
    const std::uint32_t height = (static_cast<std::uint32_t>(header[20]) << 24)
        | (static_cast<std::uint32_t>(header[21]) << 16)
        | (static_cast<std::uint32_t>(header[22]) << 8)
        | static_cast<std::uint32_t>(header[23]);
    return width > 2 && height > 2;
}

bool captureScreenshot(CaptureMode mode) {
    const fs::path outputDirectory = getOutputDir("PICTURES", "Pictures");
    const fs::path outputFile = uniqueOutputPath(outputDirectory, "screenshot", "png");
    const std::string geometry = getGeometry(mode);

    if (mode != CaptureMode::Full && geometry.empty()) {
        notify("low", "dialog-information", "Screenshot Cancelled", "No selection made.");
        return false;
    }

    const fs::path temporaryFile = outputDirectory
        / std::format(".toma-{}-{}.png", generateFilename("tmp", "png"),
                      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr int attempts = 4;
    constexpr auto delay = std::chrono::milliseconds(120);
    bool success = false;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::this_thread::sleep_for(delay);
        std::error_code error;
        fs::remove(temporaryFile, error);

        const std::string geometryArgument = geometry.empty()
            ? std::string{}
            : std::format("-g {} ", shellQuote(geometry));
        const std::string grimCommand = std::format("grim {}{}", geometryArgument, shellQuote(temporaryFile.string()));

        if (runCommand(grimCommand) == 0 && validateCapture(temporaryFile)) {
            fs::rename(temporaryFile, outputFile, error);
            success = !error;
            if (success) {
                break;
            }
        }
        std::this_thread::sleep_for(delay);
    }

    std::error_code cleanupError;
    fs::remove(temporaryFile, cleanupError);
    if (!success) {
        notify("critical", "dialog-error", "Screenshot Failed", "Could not capture image.");
        return false;
    }

    const std::string clipboardCommand = std::format("wl-copy -t image/png < {}", shellQuote(outputFile.string()));
    const int clipboardStatus = runCommand(clipboardCommand);
    std::string clipboardMessage = clipboardStatus == 0
        ? "Also copied to clipboard."
        : "Could not copy to clipboard.";

    if (debugEnabled()) {
        debugLog(std::format("clipboard expected file: {}", outputFile.string()));
        debugLog(std::format("clipboard expected size: {}", fs::file_size(outputFile)));
        debugLog(std::format("clipboard expected sha256: {}",
                             execCommand(std::format("sha256sum -- {} 2>&1", shellQuote(outputFile.string())))));
        debugLog(std::format("clipboard offered types: {}", execCommand("wl-paste --list-types 2>&1")));
        debugLog(std::format("clipboard image/png sha256: {}",
                             execCommand("wl-paste --type image/png 2>&1 | sha256sum")));
    }
    const OpenAction openAction = getOpenAction("screenshot");
    notify("normal", outputFile.string(), "Screenshot Saved",
           std::format("Saved to: {}\n\n{}", outputFile.string(), clipboardMessage),
           openAction.label, openAction.executable, outputFile.string());
    return true;
}

bool recordScreen(CaptureMode mode) {
    const fs::path outputDirectory = getOutputDir("VIDEOS", "Videos");
    const fs::path outputFile = uniqueOutputPath(outputDirectory, "screencast", "mp4");
    const std::string geometry = getGeometry(mode);

    if (mode != CaptureMode::Full && geometry.empty()) {
        notify("low", "dialog-information", "Screen Capture Cancelled", "No selection made.");
        return false;
    }

    notify("normal", "media-record", "Screen Capture Started",
           "Press Ctrl+C in the terminal to stop.");
    const std::string geometryArgument = geometry.empty()
        ? std::string{}
        : std::format("-g {} ", shellQuote(geometry));
    runCommand(std::format("wf-recorder {}-f {}", geometryArgument, shellQuote(outputFile.string())));

    std::error_code error;
    const bool saved = fs::is_regular_file(outputFile, error) && !error
        && fs::file_size(outputFile, error) > 0 && !error;
    if (saved) {
        const OpenAction openAction = getOpenAction("record");
        notify("normal", "video-x-generic", "Screen Capture Saved",
               std::format("Saved to: {}", outputFile.string()),
               openAction.label, openAction.executable, outputFile.string());
    } else {
        notify("critical", "dialog-error", "Screen Capture Failed", "Could not save video file.");
    }

    // wf-recorder may exit non-zero when stopped with Ctrl+C; the file is authoritative.
    return saved;
}

void printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " <action> <mode>\n"
              << "Actions:\n"
              << "  screenshot   Take a static screenshot\n"
              << "  record       Record a video of the screen\n"
              << "Modes:\n"
              << "  -f           Full screen\n"
              << "  -s           Select region\n"
              << "  -w           Select window\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printUsage(argv[0]);
        return 2;
    }

    const std::string_view action(argv[1]);
    const std::string_view flag(argv[2]);
    CaptureMode mode;
    if (flag == "-f") {
        mode = CaptureMode::Full;
    } else if (flag == "-s") {
        mode = CaptureMode::Select;
    } else if (flag == "-w") {
        mode = CaptureMode::Window;
    } else {
        printUsage(argv[0]);
        return 2;
    }

    try {
        if (action == "screenshot") {
            return captureScreenshot(mode) ? 0 : 1;
        }
        if (action == "record") {
            return recordScreen(mode) ? 0 : 1;
        }
    } catch (const std::exception& exception) {
        notify("critical", "dialog-error", "Screen Capture Failed", exception.what());
        std::cerr << "toma: " << exception.what() << '\n';
        return 1;
    }

    printUsage(argv[0]);
    return 2;
}

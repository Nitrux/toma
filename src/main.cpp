// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 <Nitrux Latinoamericana S.C. <hello@nxos.org>>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <system_error>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
#include <linux/fs.h>

namespace fs = std::filesystem;

namespace {

enum class CaptureMode { Full, Select, Window };

volatile sig_atomic_t selectionCancelled = 0;
volatile sig_atomic_t selectionProcessGroup = 0;
bool selectionCancelledByExistingProcess = false;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string replaceAll(std::string value, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return value;
    }
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
    return value;
}

std::optional<fs::path> findExecutable(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }

    const fs::path requested{name};
    if (requested.has_parent_path()) {
        if (::access(requested.c_str(), X_OK) == 0) {
            return requested;
        }
        return std::nullopt;
    }

    const char* pathEnvironment = std::getenv("PATH");
    const std::string pathValue = pathEnvironment
        ? pathEnvironment
        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    std::size_t start = 0;
    while (start <= pathValue.size()) {
        const std::size_t end = pathValue.find(':', start);
        const std::string_view pathView = pathValue;
        const std::string_view directory = pathView.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!directory.empty()) {
            const fs::path candidate = fs::path(directory) / requested;
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

struct ProcessOptions {
    bool captureOutput = false;
    bool processGroup = false;
    bool suppressStderr = false;
    std::optional<std::string> input;
    std::optional<fs::path> inputFile;

    ProcessOptions() = default;
    ProcessOptions(bool capture, bool group, bool suppress,
                   std::optional<std::string> inputValue = std::nullopt,
                   std::optional<fs::path> inputFileValue = std::nullopt)
        : captureOutput(capture), processGroup(group), suppressStderr(suppress),
          input(std::move(inputValue)), inputFile(std::move(inputFileValue)) {}
};

struct ProcessResult {
    int status = -1;
    std::string output;
    bool outputLimitExceeded = false;
};

constexpr std::size_t maximumCapturedOutput = 16 * 1024 * 1024;

bool processSucceeded(const ProcessResult& result) {
    return WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0;
}

void closeDescriptor(int& descriptor) {
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

std::optional<unsigned long long> processStartTime(pid_t process) {
    std::ifstream statFile(std::format("/proc/{}/stat", static_cast<long long>(process)));
    std::string statLine;
    if (!std::getline(statFile, statLine)) {
        return std::nullopt;
    }
    const std::size_t commandEnd = statLine.rfind(") ");
    if (commandEnd == std::string::npos) {
        return std::nullopt;
    }
    std::istringstream fields(statLine.substr(commandEnd + 2));
    std::string field;
    unsigned long long startTime = 0;
    for (int number = 3; number <= 22; ++number) {
        if (!(fields >> field)) {
            return std::nullopt;
        }
        if (number == 22) {
            const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), startTime);
            if (error != std::errc{} || end != field.data() + field.size()) {
                return std::nullopt;
            }
        }
    }
    return startTime;
}

void terminateProcess(pid_t process, bool processGroup) {
    if (process > 0) {
        ::kill(processGroup ? -process : process, SIGTERM);
    }
}

ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options = {}) {
    if (arguments.empty() || arguments.front().empty()) {
        throw std::invalid_argument("Cannot run an empty command");
    }
    if (options.input && options.inputFile) {
        throw std::invalid_argument("A process cannot have two stdin sources");
    }

    std::vector<std::string> resolvedArguments = arguments;
    if (!resolvedArguments.front().contains('/')) {
        const auto executable = findExecutable(resolvedArguments.front());
        if (!executable) {
            return {127, {}, false};
        }
        resolvedArguments.front() = executable->string();
    }

    int outputPipe[2]{-1, -1};
    int inputPipe[2]{-1, -1};
    if (options.captureOutput && ::pipe2(outputPipe, O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "Could not create output pipe");
    }
    if (options.input && ::pipe2(inputPipe, O_CLOEXEC) != 0) {
        closeDescriptor(outputPipe[0]);
        closeDescriptor(outputPipe[1]);
        throw std::system_error(errno, std::generic_category(), "Could not create input pipe");
    }

    const pid_t process = ::fork();
    if (process < 0) {
        const int error = errno;
        closeDescriptor(outputPipe[0]);
        closeDescriptor(outputPipe[1]);
        closeDescriptor(inputPipe[0]);
        closeDescriptor(inputPipe[1]);
        throw std::system_error(error, std::generic_category(), "Could not start process");
    }

    if (process == 0) {
        if (options.processGroup) {
            ::setpgid(0, 0);
        }

        if (options.inputFile) {
            const int inputDescriptor = ::open(options.inputFile->c_str(), O_RDONLY | O_CLOEXEC);
            if (inputDescriptor < 0 || ::dup2(inputDescriptor, STDIN_FILENO) < 0) {
                _exit(126);
            }
            ::close(inputDescriptor);
        } else if (options.input) {
            if (::dup2(inputPipe[0], STDIN_FILENO) < 0) {
                _exit(126);
            }
        }

        if (options.captureOutput && ::dup2(outputPipe[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }

        if (options.suppressStderr) {
            const int nullDescriptor = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (nullDescriptor < 0 || ::dup2(nullDescriptor, STDERR_FILENO) < 0) {
                _exit(126);
            }
            ::close(nullDescriptor);
        }

        closeDescriptor(outputPipe[0]);
        closeDescriptor(outputPipe[1]);
        closeDescriptor(inputPipe[0]);
        closeDescriptor(inputPipe[1]);

        std::vector<char*> childArguments;
        childArguments.reserve(resolvedArguments.size() + 1);
        for (const auto& argument : resolvedArguments) {
            childArguments.push_back(const_cast<char*>(argument.c_str()));
        }
        childArguments.push_back(nullptr);
        ::execv(resolvedArguments.front().c_str(), childArguments.data());
        _exit(127);
    }

    closeDescriptor(outputPipe[1]);
    closeDescriptor(inputPipe[0]);

    if (options.processGroup) {
        ::setpgid(process, process);
        selectionProcessGroup = static_cast<sig_atomic_t>(process);
    }

    struct sigaction oldPipeAction{};
    struct sigaction ignorePipeAction{};
    const bool ignorePipe = options.input.has_value();
    if (ignorePipe) {
        ignorePipeAction.sa_handler = SIG_IGN;
        sigemptyset(&ignorePipeAction.sa_mask);
        ::sigaction(SIGPIPE, &ignorePipeAction, &oldPipeAction);
    }

    if (options.input) {
        std::size_t offset = 0;
        while (offset < options.input->size()) {
            const ssize_t written = ::write(inputPipe[1], options.input->data() + offset,
                                            options.input->size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        closeDescriptor(inputPipe[1]);
    }

    if (ignorePipe) {
        ::sigaction(SIGPIPE, &oldPipeAction, nullptr);
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    bool outputLimitReached = false;
    while (options.captureOutput) {
        const ssize_t bytesRead = ::read(outputPipe[0], buffer.data(), buffer.size());
        if (bytesRead > 0) {
            if (result.output.size() + static_cast<std::size_t>(bytesRead) <= maximumCapturedOutput) {
                result.output.append(buffer.data(), static_cast<std::size_t>(bytesRead));
            } else if (!outputLimitReached) {
                outputLimitReached = true;
                terminateProcess(process, options.processGroup);
            }
        } else if (bytesRead < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    closeDescriptor(outputPipe[0]);

    while (::waitpid(process, &result.status, 0) < 0 && errno == EINTR) {
    }

    if (options.processGroup) {
        selectionProcessGroup = 0;
    }
    result.outputLimitExceeded = outputLimitReached;
    return result;
}

struct OpenAction {
    std::string label;
    std::string executable;
};

OpenAction getOpenAction(std::string_view captureType) {
    const bool isScreenshot = captureType == "screenshot";
    const std::string application = isScreenshot ? "pix" : "clip";
    if (const auto executable = findExecutable(application)) {
        return {std::format("Open {}", isScreenshot ? "Pix" : "Clip"), executable->string()};
    }
    return {"Open", findExecutable("xdg-open").value_or(fs::path("xdg-open")).string()};
}

void spawnNotificationAction(const std::vector<std::string>& notificationArguments,
                             std::string opener, fs::path file) {
    const pid_t intermediate = ::fork();
    if (intermediate < 0) {
        return;
    }
    if (intermediate > 0) {
        while (::waitpid(intermediate, nullptr, 0) < 0 && errno == EINTR) {
        }
        return;
    }

    const pid_t detached = ::fork();
    if (detached > 0) {
        _exit(0);
    }
    if (detached < 0 || ::setsid() < 0) {
        _exit(1);
    }

    const int nullDescriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullDescriptor >= 0) {
        ::dup2(nullDescriptor, STDIN_FILENO);
        ::dup2(nullDescriptor, STDOUT_FILENO);
        ::dup2(nullDescriptor, STDERR_FILENO);
        ::close(nullDescriptor);
    }

    const ProcessResult notification = runProcess(notificationArguments, ProcessOptions{true, false, true});
    if (processSucceeded(notification) && trim(std::move(notification.output)) == "default") {
        runProcess({std::move(opener), file.string()}, {false, false, true});
    }
    _exit(0);
}

void notify(std::string_view urgency, std::string_view icon, std::string_view title,
            std::string_view body, std::string_view actionLabel = {},
            std::string_view opener = {}, std::string_view file = {}) {
    std::vector<std::string> arguments{
        "notify-send", "-a", std::string(title), "-u", std::string(urgency),
        "-i", std::string(icon), std::string(title), std::string(body)
    };

    if (actionLabel.empty() || opener.empty() || file.empty()) {
        runProcess(arguments, {false, false, true});
        return;
    }

    arguments.push_back(std::format("--action=default={}", actionLabel));
    spawnNotificationAction(arguments, std::string(opener), fs::path(file));
}

std::optional<std::string> userDirectoryValue(std::string_view variable) {
    const char* homeEnvironment = std::getenv("HOME");
    if (!homeEnvironment || *homeEnvironment == 0) {
        return std::nullopt;
    }

    const char* configEnvironment = std::getenv("XDG_CONFIG_HOME");
    const fs::path configDirectory = configEnvironment && *configEnvironment
        ? fs::path(configEnvironment)
        : fs::path(homeEnvironment) / ".config";
    const fs::path userDirectories = configDirectory / "user-dirs.dirs";
    std::ifstream stream(userDirectories);
    if (!stream) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos || trim(line.substr(0, equals)) != variable) {
            continue;
        }

        std::string value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == char(34) && value.back() == char(34)) {
            value = value.substr(1, value.size() - 2);
        }

        std::string unescaped;
        unescaped.reserve(value.size());
        bool escaped = false;
        for (const char character : value) {
            if (escaped) {
                unescaped.push_back(character);
                escaped = false;
            } else if (character == char(92)) {
                escaped = true;
            } else {
                unescaped.push_back(character);
            }
        }
        if (escaped) {
            unescaped.push_back(char(92));
        }

        value = replaceAll(std::move(unescaped), "$HOME", homeEnvironment);
        value = replaceAll(std::move(value), "${HOME}", homeEnvironment);
        if (value == "~") {
            value = homeEnvironment;
        } else if (value.starts_with("~/")) {
            value = std::format("{}/{}", homeEnvironment, value.substr(2));
        }

        fs::path result(value);
        if (result.is_absolute()) {
            return result.string();
        }
        return (fs::path(homeEnvironment) / result).string();
    }
    return std::nullopt;
}

fs::path getOutputDir(std::string_view xdgType, std::string_view fallbackFolder) {
    const std::string variable = std::format("XDG_{}_DIR", xdgType);
    const char* homeDirectory = std::getenv("HOME");
    fs::path outputDirectory;

    if (const auto configured = userDirectoryValue(variable)) {
        outputDirectory = *configured;
    } else if (homeDirectory && *homeDirectory) {
        outputDirectory = fs::path(homeDirectory) / fallbackFolder;
    } else {
        outputDirectory = fs::path("/tmp") / std::format("toma-{}-{}", static_cast<unsigned long long>(::getuid()),
                                                          fallbackFolder);
    }

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

fs::path createTemporaryFile(const fs::path& directory, std::string_view suffix) {
    std::string pattern = (directory / std::format(".toma-XXXXXX{}", suffix)).string();
    std::vector<char> mutablePattern(pattern.begin(), pattern.end());
    mutablePattern.push_back(char(0));

    const int descriptor = ::mkstemps(mutablePattern.data(), static_cast<int>(suffix.size()));
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "Could not create temporary capture file");
    }
    ::close(descriptor);
    return fs::path(mutablePattern.data());
}

bool renameWithoutReplacement(const fs::path& source, const fs::path& destination) {
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        return false;
    }
    throw std::system_error(errno, std::generic_category(),
                            std::format("Could not move capture to {}", destination.string()));
}

bool finalizeCapture(const fs::path& temporaryFile, fs::path& outputFile,
                     const fs::path& directory, std::string_view prefix,
                     std::string_view extension) {
    for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
        if (renameWithoutReplacement(temporaryFile, outputFile)) {
            return true;
        }
        outputFile = uniqueOutputPath(directory, prefix, extension);
    }
    return false;
}

void handleSelectionSignal(int) {
    selectionCancelled = 1;
    const pid_t processGroup = static_cast<pid_t>(selectionProcessGroup);
    if (processGroup > 0) {
        ::kill(-processGroup, SIGTERM);
    }
}

class SelectionSignalGuard {
public:
    SelectionSignalGuard() {
        selectionCancelled = 0;
        struct sigaction action{};
        action.sa_handler = handleSelectionSignal;
        sigemptyset(&action.sa_mask);

        if (::sigaction(SIGTERM, &action, &oldTerm_) != 0) {
            throw std::system_error(errno, std::generic_category(), "Could not install SIGTERM handler");
        }
        termInstalled_ = true;
        if (::sigaction(SIGINT, &action, &oldInt_) != 0) {
            ::sigaction(SIGTERM, &oldTerm_, nullptr);
            termInstalled_ = false;
            throw std::system_error(errno, std::generic_category(), "Could not install SIGINT handler");
        }
        intInstalled_ = true;
    }

    ~SelectionSignalGuard() {
        if (intInstalled_) {
            ::sigaction(SIGINT, &oldInt_, nullptr);
        }
        if (termInstalled_) {
            ::sigaction(SIGTERM, &oldTerm_, nullptr);
        }
        selectionProcessGroup = 0;
    }

    SelectionSignalGuard(const SelectionSignalGuard&) = delete;
    SelectionSignalGuard& operator=(const SelectionSignalGuard&) = delete;

private:
    struct sigaction oldTerm_{};
    struct sigaction oldInt_{};
    bool termInstalled_ = false;
    bool intInstalled_ = false;
};

class SelectionLock {
public:
    SelectionLock() = default;

    bool acquire() {
        lockPath_ = lockPath();

        for (;;) {
            fileDescriptor_ = ::open(lockPath_.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fileDescriptor_ >= 0) {
                const auto ownStartTime = processStartTime(::getpid());
                if (!ownStartTime) {
                    ::close(fileDescriptor_);
                    fileDescriptor_ = -1;
                    ::unlink(lockPath_.c_str());
                    throw std::runtime_error("Could not read the current process start time");
                }
                const std::string pid = std::format("{} {}\n", static_cast<long long>(::getpid()), *ownStartTime);
                std::size_t offset = 0;
                while (offset < pid.size()) {
                    const ssize_t written = ::write(fileDescriptor_, pid.data() + offset, pid.size() - offset);
                    if (written > 0) {
                        offset += static_cast<std::size_t>(written);
                    } else if (written < 0 && errno == EINTR) {
                        continue;
                    } else {
                        const int error = errno;
                        ::close(fileDescriptor_);
                        fileDescriptor_ = -1;
                        ::unlink(lockPath_.c_str());
                        throw std::system_error(error, std::generic_category(), "Could not write selection lock");
                    }
                }
                if (::fstat(fileDescriptor_, &lockStat_) != 0) {
                    const int error = errno;
                    ::close(fileDescriptor_);
                    fileDescriptor_ = -1;
                    ::unlink(lockPath_.c_str());
                    throw std::system_error(error, std::generic_category(), "Could not inspect selection lock");
                }
                acquired_ = true;
                return true;
            }

            if (errno != EEXIST) {
                throw std::system_error(errno, std::generic_category(), "Could not create selection lock");
            }

            const int existing = ::open(lockPath_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (existing < 0) {
                if (errno == ELOOP) {
                    throw std::runtime_error("Selection lock is a symlink");
                }
                if (errno == ENOENT) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "Could not inspect selection lock");
            }

            struct stat existingStat{};
            if (::fstat(existing, &existingStat) != 0) {
                const int error = errno;
                ::close(existing);
                throw std::system_error(error, std::generic_category(), "Could not inspect selection lock");
            }
            if (existingStat.st_uid != ::getuid() || !S_ISREG(existingStat.st_mode)) {
                ::close(existing);
                throw std::runtime_error("Selection lock has unsafe ownership");
            }

            std::string contents;
            std::array<char, 64> buffer{};
            for (;;) {
                const ssize_t bytesRead = ::read(existing, buffer.data(), buffer.size());
                if (bytesRead > 0) {
                    contents.append(buffer.data(), static_cast<std::size_t>(bytesRead));
                } else if (bytesRead < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
            ::close(existing);

            const auto first = contents.find_first_not_of(" \t\r\n");
            const auto last = contents.find_last_not_of(" \t\r\n");
            if (first == std::string::npos || last == std::string::npos) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::istringstream lockContents(contents.substr(first, last - first + 1));
            long long ownerValue = 0;
            unsigned long long ownerStartTime = 0;
            if (!(lockContents >> ownerValue >> ownerStartTime) || ownerValue <= 0) {
                throw std::runtime_error("Selection lock contains invalid process information");
            }

            const pid_t owner = static_cast<pid_t>(ownerValue);
            errno = 0;
            const bool ownerExists = ::kill(owner, 0) == 0 || errno == EPERM;
            const auto currentOwnerStartTime = ownerExists ? processStartTime(owner) : std::nullopt;
            const bool sameProcess = currentOwnerStartTime && *currentOwnerStartTime == ownerStartTime;
            if (sameProcess) {
                if (::kill(owner, SIGTERM) == 0 || errno == EPERM) {
                    return false;
                }
                if (errno != ESRCH) {
                    throw std::system_error(errno, std::generic_category(), "Could not cancel existing selection");
                }
            }

            if (::unlink(lockPath_.c_str()) != 0 && errno != ENOENT) {
                throw std::system_error(errno, std::generic_category(), "Could not remove stale selection lock");
            }
        }
    }

    ~SelectionLock() {
        if (fileDescriptor_ >= 0) {
            if (acquired_) {
                struct stat currentStat{};
                if (::stat(lockPath_.c_str(), &currentStat) == 0
                    && currentStat.st_dev == lockStat_.st_dev
                    && currentStat.st_ino == lockStat_.st_ino) {
                    ::unlink(lockPath_.c_str());
                }
            }
            ::close(fileDescriptor_);
        }
    }

    SelectionLock(const SelectionLock&) = delete;
    SelectionLock& operator=(const SelectionLock&) = delete;

private:
    static fs::path lockPath() {
        const char* runtimeEnvironment = std::getenv("XDG_RUNTIME_DIR");
        if (runtimeEnvironment && *runtimeEnvironment) {
            struct stat runtimeStat{};
            if (::stat(runtimeEnvironment, &runtimeStat) == 0
                && S_ISDIR(runtimeStat.st_mode)
                && runtimeStat.st_uid == ::getuid()
                && (runtimeStat.st_mode & 0022) == 0) {
                return fs::path(runtimeEnvironment) / "toma-selection.lock";
            }
        }
        return std::format("/tmp/toma-selection-{}.lock", static_cast<unsigned long long>(::getuid()));
    }

    fs::path lockPath_;
    int fileDescriptor_ = -1;
    bool acquired_ = false;
    struct stat lockStat_{};
};

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue : std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> {
    using Base = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
    using Base::Base;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue parse() {
        JsonValue value = parseValue();
        skipWhitespace();
        if (position_ != input_.size()) {
            fail();
        }
        return value;
    }

private:
    [[noreturn]] void fail() const {
        throw std::runtime_error(std::format("Invalid Hyprland JSON near byte {}", position_));
    }

    void skipWhitespace() {
        while (position_ < input_.size()
               && (input_[position_] == char(32) || input_[position_] == char(9)
                   || input_[position_] == char(10) || input_[position_] == char(13))) {
            ++position_;
        }
    }

    char consume() {
        if (position_ >= input_.size()) {
            fail();
        }
        return input_[position_++];
    }

    void expect(char expected) {
        if (consume() != expected) {
            fail();
        }
    }

    JsonValue parseValue() {
        skipWhitespace();
        if (position_ >= input_.size()) {
            fail();
        }
        switch (input_[position_]) {
        case char(123):
            return parseObject();
        case char(91):
            return parseArray();
        case char(34):
            return parseString();
        case char(116):
            return parseLiteral("true", true);
        case char(102):
            return parseLiteral("false", false);
        case char(110):
            return parseLiteral("null", nullptr);
        default:
            return parseNumber();
        }
    }

    JsonValue parseObject() {
        JsonObject object;
        expect(char(123));
        skipWhitespace();
        if (position_ < input_.size() && input_[position_] == char(125)) {
            ++position_;
            return object;
        }

        for (;;) {
            skipWhitespace();
            JsonValue keyValue = parseString();
            const std::string key = std::get<std::string>(static_cast<const JsonValue::Base&>(keyValue));
            skipWhitespace();
            expect(char(58));
            object.emplace(key, parseValue());
            skipWhitespace();
            const char separator = consume();
            if (separator == char(125)) {
                return object;
            }
            if (separator != char(44)) {
                fail();
            }
        }
    }

    JsonValue parseArray() {
        JsonArray array;
        expect(char(91));
        skipWhitespace();
        if (position_ < input_.size() && input_[position_] == char(93)) {
            ++position_;
            return array;
        }

        for (;;) {
            array.push_back(parseValue());
            skipWhitespace();
            const char separator = consume();
            if (separator == char(93)) {
                return array;
            }
            if (separator != char(44)) {
                fail();
            }
        }
    }

    static void appendCodePoint(std::string& output, unsigned int codePoint) {
        if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            codePoint = 0xfffd;
        }
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if (codePoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    unsigned int parseHexCodePoint() {
        unsigned int value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = consume();
            value <<= 4;
            if (character >= char(48) && character <= char(57)) {
                value += static_cast<unsigned int>(character - char(48));
            } else if (character >= char(65) && character <= char(70)) {
                value += static_cast<unsigned int>(character - char(65) + 10);
            } else if (character >= char(97) && character <= char(102)) {
                value += static_cast<unsigned int>(character - char(97) + 10);
            } else {
                fail();
            }
        }
        return value;
    }

    JsonValue parseString() {
        expect(char(34));
        std::string value;
        while (position_ < input_.size()) {
            const char character = consume();
            if (character == char(34)) {
                return value;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                fail();
            }
            if (character != char(92)) {
                value.push_back(character);
                continue;
            }

            switch (consume()) {
            case char(34): value.push_back(char(34)); break;
            case char(92): value.push_back(char(92)); break;
            case char(47): value.push_back(char(47)); break;
            case char(98): value.push_back(char(8)); break;
            case char(102): value.push_back(char(12)); break;
            case char(110): value.push_back(char(10)); break;
            case char(114): value.push_back(char(13)); break;
            case char(116): value.push_back(char(9)); break;
            case char(117): appendCodePoint(value, parseHexCodePoint()); break;
            default: fail();
            }
        }
        fail();
    }

    JsonValue parseLiteral(std::string_view literal, JsonValue value) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail();
        }
        position_ += literal.size();
        return value;
    }

    JsonValue parseNumber() {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == char(45)) {
            ++position_;
        }
        if (position_ >= input_.size()) {
            fail();
        }
        if (input_[position_] == char(48)) {
            ++position_;
        } else {
            if (input_[position_] < char(49) || input_[position_] > char(57)) {
                fail();
            }
            while (position_ < input_.size()
                   && input_[position_] >= char(48) && input_[position_] <= char(57)) {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == char(46)) {
            ++position_;
            if (position_ >= input_.size()
                || input_[position_] < char(48) || input_[position_] > char(57)) {
                fail();
            }
            while (position_ < input_.size()
                   && input_[position_] >= char(48) && input_[position_] <= char(57)) {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == char(101) || input_[position_] == char(69))) {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == char(43) || input_[position_] == char(45))) {
                ++position_;
            }
            if (position_ >= input_.size()
                || input_[position_] < char(48) || input_[position_] > char(57)) {
                fail();
            }
            while (position_ < input_.size()
                   && input_[position_] >= char(48) && input_[position_] <= char(57)) {
                ++position_;
            }
        }

        double value = 0;
        const auto [end, error] = std::from_chars(input_.data() + start, input_.data() + position_, value);
        if (error != std::errc{} || end != input_.data() + position_ || !std::isfinite(value)) {
            fail();
        }
        return value;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const JsonObject* objectValue(const JsonValue* value) {
    if (!value || !std::holds_alternative<JsonObject>(static_cast<const JsonValue::Base&>(*value))) {
        return nullptr;
    }
    return &std::get<JsonObject>(static_cast<const JsonValue::Base&>(*value));
}

const JsonArray* arrayValue(const JsonValue* value) {
    if (!value || !std::holds_alternative<JsonArray>(static_cast<const JsonValue::Base&>(*value))) {
        return nullptr;
    }
    return &std::get<JsonArray>(static_cast<const JsonValue::Base&>(*value));
}

const double* numberValue(const JsonValue* value) {
    if (!value || !std::holds_alternative<double>(static_cast<const JsonValue::Base&>(*value))) {
        return nullptr;
    }
    return &std::get<double>(static_cast<const JsonValue::Base&>(*value));
}

const JsonValue* objectMember(const JsonObject& object, std::string_view key) {
    const auto iterator = object.find(std::string(key));
    return iterator == object.end() ? nullptr : &iterator->second;
}

std::optional<unsigned int> jsonUnsigned(const JsonValue* value) {
    const double* number = numberValue(value);
    if (!number || *number < 0 || *number > std::numeric_limits<unsigned int>::max()
        || std::floor(*number) != *number) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(*number);
}

std::string jsonNumber(double number) {
    if (std::floor(number) == number
        && number >= static_cast<double>(std::numeric_limits<long long>::min())
        && number <= static_cast<double>(std::numeric_limits<long long>::max())) {
        return std::format("{}", static_cast<long long>(number));
    }
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    if (error != std::errc{}) {
        throw std::runtime_error("Could not format Hyprland geometry");
    }
    return std::string(buffer.data(), end);
}

unsigned int activeWorkspace(const std::string& json) {
    const JsonValue root = JsonParser(json).parse();
    const JsonObject* object = objectValue(&root);
    const auto workspace = object ? jsonUnsigned(objectMember(*object, "id")) : std::nullopt;
    if (!workspace) {
        throw std::runtime_error("Hyprland returned an invalid active workspace");
    }
    return *workspace;
}

std::string windowSelectionInput(const std::string& json, unsigned int workspaceId) {
    const JsonValue root = JsonParser(json).parse();
    const JsonArray* clients = arrayValue(&root);
    if (!clients) {
        throw std::runtime_error("Hyprland returned an invalid client list");
    }

    std::string result;
    for (const JsonValue& clientValue : *clients) {
        const JsonObject* client = objectValue(&clientValue);
        if (!client) {
            continue;
        }
        const JsonObject* workspace = objectValue(objectMember(*client, "workspace"));
        const auto clientWorkspace = workspace
            ? jsonUnsigned(objectMember(*workspace, "id"))
            : std::nullopt;
        const JsonArray* at = arrayValue(objectMember(*client, "at"));
        const JsonArray* size = arrayValue(objectMember(*client, "size"));
        if (!clientWorkspace || *clientWorkspace != workspaceId || !at || !size
            || at->size() < 2 || size->size() < 2) {
            continue;
        }

        const double* x = numberValue(&(*at)[0]);
        const double* y = numberValue(&(*at)[1]);
        const double* width = numberValue(&(*size)[0]);
        const double* height = numberValue(&(*size)[1]);
        if (!x || !y || !width || !height || !std::isfinite(*x) || !std::isfinite(*y)
            || !std::isfinite(*width) || !std::isfinite(*height) || *width <= 2 || *height <= 2) {
            continue;
        }
        result += std::format("{},{} {}x{}\n", jsonNumber(*x), jsonNumber(*y),
                              jsonNumber(*width), jsonNumber(*height));
    }
    return result;
}

std::string execSelection(const std::vector<std::string>& arguments,
                          std::optional<std::string> input = std::nullopt) {
    const ProcessResult result = runProcess(arguments, ProcessOptions{true, true, true, std::move(input), std::nullopt});
    if (!processSucceeded(result) || result.outputLimitExceeded || selectionCancelled) {
        return {};
    }
    return trim(result.output);
}

std::string getGeometry(CaptureMode mode) {
    selectionCancelledByExistingProcess = false;
    if (mode == CaptureMode::Full) {
        return {};
    }

    SelectionSignalGuard signalGuard;
    SelectionLock selectionLock;
    selectionCancelledByExistingProcess = !selectionLock.acquire();
    if (selectionCancelledByExistingProcess || selectionCancelled) {
        return {};
    }

    if (mode == CaptureMode::Select) {
        return execSelection({"slurp"});
    }

    const ProcessResult workspaceProcess = runProcess({"hyprctl", "activeworkspace", "-j"},
                                                       {true, false, true});
    if (!processSucceeded(workspaceProcess)) {
        return {};
    }

    const ProcessResult clientsProcess = runProcess({"hyprctl", "clients", "-j"},
                                                     {true, false, true});
    if (!processSucceeded(clientsProcess)) {
        return {};
    }

    if (selectionCancelled) {
        return {};
    }
    const std::string input = windowSelectionInput(clientsProcess.output, activeWorkspace(workspaceProcess.output));
    return execSelection({"slurp"}, input);
}

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
    fs::path outputFile = uniqueOutputPath(outputDirectory, "screenshot", "png");
    const std::string geometry = getGeometry(mode);

    if (mode != CaptureMode::Full && geometry.empty()) {
        if (selectionCancelledByExistingProcess) {
            return false;
        }
        notify("low", "dialog-information", "Screenshot Cancelled", "No selection made.");
        return false;
    }

    const fs::path temporaryFile = createTemporaryFile(outputDirectory, ".png");
    constexpr int attempts = 4;
    constexpr auto delay = std::chrono::milliseconds(120);
    bool success = false;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::vector<std::string> arguments{"grim"};
        if (!geometry.empty()) {
            arguments.push_back("-g");
            arguments.push_back(geometry);
        }
        arguments.push_back(temporaryFile.string());

        std::this_thread::sleep_for(delay);
        const ProcessResult capture = runProcess(arguments, {false, false, true});
        if (processSucceeded(capture) && validateCapture(temporaryFile)) {
            success = finalizeCapture(temporaryFile, outputFile, outputDirectory, "screenshot", "png");
            if (success) {
                break;
            }
        }
        std::this_thread::sleep_for(delay);
    }

    if (!success) {
        std::error_code cleanupError;
        fs::remove(temporaryFile, cleanupError);
        notify("critical", "dialog-error", "Screenshot Failed", "Could not capture image.");
        return false;
    }

    const ProcessResult clipboard = runProcess({"wl-copy", "-t", "image/png"},
                                                {false, false, true, std::nullopt, outputFile});
    const std::string clipboardMessage = processSucceeded(clipboard)
        ? "Also copied to clipboard."
        : "Could not copy to clipboard.";
    const OpenAction openAction = getOpenAction("screenshot");
    notify("normal", outputFile.string(), "Screenshot Saved",
           std::format("Saved to: {}\n\n{}", outputFile.string(), clipboardMessage),
           openAction.label, openAction.executable, outputFile.string());
    return true;
}

bool recordScreen(CaptureMode mode) {
    const fs::path outputDirectory = getOutputDir("VIDEOS", "Videos");
    fs::path outputFile = uniqueOutputPath(outputDirectory, "screencast", "mp4");
    const std::string geometry = getGeometry(mode);

    if (mode != CaptureMode::Full && geometry.empty()) {
        if (selectionCancelledByExistingProcess) {
            return false;
        }
        notify("low", "dialog-information", "Screen Capture Cancelled", "No selection made.");
        return false;
    }

    const fs::path temporaryFile = createTemporaryFile(outputDirectory, ".mp4");
    notify("normal", "media-record", "Screen Capture Started",
           "Press Ctrl+C in the terminal to stop.");

    std::vector<std::string> arguments{"wf-recorder"};
    if (!geometry.empty()) {
        arguments.push_back("-g");
        arguments.push_back(geometry);
    }
    arguments.push_back("-f");
    arguments.push_back(temporaryFile.string());
    runProcess(arguments, {false, false, true});

    std::error_code error;
    const bool recorded = fs::is_regular_file(temporaryFile, error) && !error
        && fs::file_size(temporaryFile, error) > 0 && !error;
    const bool saved = recorded && finalizeCapture(temporaryFile, outputFile, outputDirectory, "screencast", "mp4");
    if (saved) {
        const OpenAction openAction = getOpenAction("record");
        notify("normal", "video-x-generic", "Screen Capture Saved",
               std::format("Saved to: {}", outputFile.string()),
               openAction.label, openAction.executable, outputFile.string());
    } else {
        std::error_code cleanupError;
        fs::remove(temporaryFile, cleanupError);
        notify("critical", "dialog-error", "Screen Capture Failed", "Could not save video file.");
    }

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

}

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

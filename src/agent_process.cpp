#include "agent_process.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <dirent.h>
#endif

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

bool python_program(const fs::path &program) {
    auto name = program.filename().string();
    std::ranges::transform(name, name.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (!name.starts_with("python")) return false;
    auto suffix = std::string_view(name).substr(6);
    if (suffix.starts_with('w')) suffix.remove_prefix(1);
    if (suffix.ends_with(".exe")) suffix.remove_suffix(4);
    return std::ranges::all_of(suffix, [](unsigned char value) {
        return std::isdigit(value) || value == '.';
    });
}

std::optional<fs::path> canonical_existing(const fs::path &path) {
    if (!path.is_absolute()) return std::nullopt;
    std::error_code error;
    auto result = fs::canonical(path, error);
    if (error) return std::nullopt;
    return result;
}

#if defined(__APPLE__)
std::optional<std::vector<std::string>> argv_for_pid(pid_t pid) {
    int mib[] = {CTL_KERN, KERN_PROCARGS2, pid};
    std::size_t size = 0;
    if (::sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0
        || size < sizeof(int) || size > (std::size_t{4} << 20)) {
        return std::nullopt;
    }
    std::vector<char> bytes(size);
    if (::sysctl(mib, 3, bytes.data(), &size, nullptr, 0)
        != 0 || size < sizeof(int)) {
        return std::nullopt;
    }
    int argc = 0;
    std::memcpy(&argc, bytes.data(), sizeof(argc));
    if (argc <= 0 || argc > 4096) return std::nullopt;
    auto cursor = sizeof(argc);
    const auto next_string = [&]() -> std::optional<std::string> {
        if (cursor >= size) return std::nullopt;
        const auto begin = cursor;
        while (cursor < size && bytes[cursor] != '\0') ++cursor;
        if (cursor >= size) return std::nullopt;
        auto value = std::string(bytes.data() + begin, cursor - begin);
        ++cursor;
        return value;
    };
    // First string is the executable path outside argv.
    if (!next_string()) return std::nullopt;
    while (cursor < size && bytes[cursor] == '\0') ++cursor;
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(argc));
    for (auto index = 0; index != argc; ++index) {
        auto value = next_string();
        if (!value) return std::nullopt;
        result.push_back(std::move(*value));
    }
    return result;
}

std::optional<std::vector<pid_t>> all_pids() {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    std::size_t size = 0;
    if (::sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) {
        return std::nullopt;
    }
    // The table can grow between calls; leave bounded slack and retry once.
    size += size / 8 + sizeof(kinfo_proc) * 16;
    std::vector<kinfo_proc> entries(size / sizeof(kinfo_proc));
    if (::sysctl(mib, 4, entries.data(), &size, nullptr, 0)
        != 0) {
        return std::nullopt;
    }
    entries.resize(size / sizeof(kinfo_proc));
    std::vector<pid_t> result;
    result.reserve(entries.size());
    for (const auto &entry : entries) {
        if (entry.kp_proc.p_pid > 0) result.push_back(entry.kp_proc.p_pid);
    }
    return result;
}
#elif defined(__linux__)
std::optional<std::vector<std::string>> argv_for_pid(pid_t pid) {
    std::ifstream stream(
        fs::path("/proc") / std::to_string(pid) / "cmdline", std::ios::binary);
    if (!stream.is_open()) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.empty() || bytes.size() > (std::size_t{4} << 20)) {
        return std::nullopt;
    }
    std::vector<std::string> result;
    auto begin = std::size_t{0};
    while (begin < bytes.size()) {
        const auto end = bytes.find('\0', begin);
        const auto stop = end == std::string::npos ? bytes.size() : end;
        result.emplace_back(bytes.data() + begin, stop - begin);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::optional<std::vector<pid_t>> all_pids() {
    auto *directory = ::opendir("/proc");
    if (!directory) return std::nullopt;
    std::vector<pid_t> result;
    while (const auto *entry = ::readdir(directory)) {
        const auto name = std::string_view(entry->d_name);
        if (name.empty() || !std::ranges::all_of(name, [](unsigned char value) {
                return std::isdigit(value);
            })) {
            continue;
        }
        char *end = nullptr;
        const auto value = std::strtol(entry->d_name, &end, 10);
        if (end && *end == '\0' && value > 0) {
            result.push_back(static_cast<pid_t>(value));
        }
    }
    ::closedir(directory);
    return result;
}
#else
std::optional<std::vector<std::string>> argv_for_pid(pid_t) {
    return std::nullopt;
}
std::optional<std::vector<pid_t>> all_pids() { return std::nullopt; }
#endif

} // namespace

bool matches_exact_agent_process(const std::vector<std::string> &argv,
        const fs::path &canonical_agent_dir) noexcept {
    try {
        if (argv.size() != 5 || !python_program(argv[0])
            || argv[1] != "-m" || argv[2] != "lingtai"
            || argv[3] != "run") {
            return false;
        }
        const auto expected = canonical_existing(canonical_agent_dir);
        const auto observed = canonical_existing(fs::path(argv[4]));
        return expected && observed && *expected == *observed;
    } catch (...) {
        return false;
    }
}

AgentProcessObservation observe_exact_agent_processes(
        const fs::path &canonical_agent_dir) noexcept {
    try {
        const auto pids = all_pids();
        if (!pids) return {};
        AgentProcessObservation result;
        result.available = true;
        for (const auto pid : *pids) {
            const auto argv = argv_for_pid(pid);
            if (argv && matches_exact_agent_process(*argv, canonical_agent_dir)) {
                result.pids.push_back(static_cast<AgentProcessId>(pid));
            }
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool signal_exact_agent_process(const fs::path &canonical_agent_dir,
        AgentProcessId pid, AgentTerminationSignal signal) noexcept {
    try {
        if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) return false;
        const auto native_pid = static_cast<pid_t>(pid);
        const auto argv = argv_for_pid(native_pid);
        if (!argv || !matches_exact_agent_process(*argv, canonical_agent_dir)) {
            return false;
        }
        const auto number = signal == AgentTerminationSignal::terminate
            ? SIGTERM : SIGKILL;
        return ::kill(native_pid, number) == 0 || errno == ESRCH;
    } catch (...) {
        return false;
    }
}

} // namespace lingtai::desktop

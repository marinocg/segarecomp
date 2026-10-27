#pragma once

// A small process runner shared by the CLI's build commands: run argv with stdout and stderr redirected to a log file, return the exit code.
// -1: the process could not be started; kProcessTimedOut: it was killed after `timeout_seconds`. `environment` entries are added to the child.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace segarecomp_cli {

using Environment = std::vector<std::pair<std::string, std::string>>;
inline constexpr int kProcessTimedOut = -2;

int run_process(const std::vector<std::string> &argv, const std::filesystem::path &log, const Environment &environment = {}, unsigned timeout_seconds = 0);

}  // namespace segarecomp_cli

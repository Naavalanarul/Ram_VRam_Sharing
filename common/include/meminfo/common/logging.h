#pragma once
#include <string>
#include <spdlog/spdlog.h>

namespace meminfo {

// Initialize logging with stdout + rotating file sinks.
// daemon_name: used for log file naming (e.g. "discoveryd")
// log_dir: directory for log files (default: "logs")
// level: log level string ("trace", "debug", "info", "warn", "error")
void init_logging(const std::string& daemon_name,
                  const std::string& log_dir = "logs",
                  const std::string& level = "info");

// Get the default logger
std::shared_ptr<spdlog::logger> get_logger();

} // namespace meminfo

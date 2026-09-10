#include "meminfo/common/logging.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <filesystem>
#include <vector>

namespace meminfo {

void init_logging(const std::string& daemon_name,
                  const std::string& log_dir,
                  const std::string& level) {
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks {stdout_sink};

    // Losing the log file must not take the daemon down with it. The default
    // directories are POSIX paths the process may well not be able to create
    // -- /var/log/meminfo without privileges, or its drive-relative
    // reinterpretation on Windows -- and create_directories() and the file
    // sink both throw. Fall back to stdout-only instead.
    try {
        std::filesystem::create_directories(log_dir);
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_dir + "/" + daemon_name + ".log", 1024 * 1024 * 5, 3));
    } catch (const std::exception& e) {
        spdlog::warn("File logging disabled ({}): {}", log_dir, e.what());
    }

    auto logger = std::make_shared<spdlog::logger>(daemon_name, sinks.begin(), sinks.end());

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    logger->flush_on(spdlog::level::warn);

    spdlog::level::level_enum lvl = spdlog::level::info;
    if (level == "trace") lvl = spdlog::level::trace;
    else if (level == "debug") lvl = spdlog::level::debug;
    else if (level == "warn") lvl = spdlog::level::warn;
    else if (level == "error") lvl = spdlog::level::err;

    logger->set_level(lvl);

    // register_logger throws if a logger with this name already exists, which
    // would turn a second init_logging call (a re-init, or two daemons hosted
    // in one process) into a fatal error.
    spdlog::drop(daemon_name);
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
}

std::shared_ptr<spdlog::logger> get_logger() {
    return spdlog::default_logger();
}

} // namespace meminfo

#include "meminfo/common/logging.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <filesystem>
#include <vector>

namespace meminfo {

void init_logging(const std::string& daemon_name,
                  const std::string& log_dir,
                  const std::string& level) {
    std::filesystem::create_directories(log_dir);

    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/" + daemon_name + ".log", 1024 * 1024 * 5, 3);

    std::vector<spdlog::sink_ptr> sinks {stdout_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>(daemon_name, sinks.begin(), sinks.end());

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    logger->flush_on(spdlog::level::warn);

    spdlog::level::level_enum lvl = spdlog::level::info;
    if (level == "trace") lvl = spdlog::level::trace;
    else if (level == "debug") lvl = spdlog::level::debug;
    else if (level == "warn") lvl = spdlog::level::warn;
    else if (level == "error") lvl = spdlog::level::err;

    logger->set_level(lvl);
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
}

std::shared_ptr<spdlog::logger> get_logger() {
    return spdlog::default_logger();
}

} // namespace meminfo

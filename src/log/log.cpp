#include "log.h"

#include <filesystem>

namespace minirtc {
namespace {
std::string g_log_dir = "logs";
std::once_flag g_logger_once_flag;
std::mutex g_logger_mutex;
std::shared_ptr<spdlog::logger> g_logger;

std::string NormalizeLogDirectory(const std::string& log_dir) {
  const std::filesystem::path path = log_dir.empty() ? "logs" : log_dir;
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal().string();
}
}  // namespace

void InitLogger(const std::string& log_dir) {
  const auto directory = NormalizeLogDirectory(log_dir);
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (g_logger) {
    if (directory != g_log_dir) {
      g_logger->warn("InitLogger called after logger initialized. Ignoring "
                     "log_dir: {}, using previous log_dir: {}",
                     directory, g_log_dir);
    }
    return;
  }
  g_log_dir = directory;
}

std::shared_ptr<spdlog::logger> get_logger() {
  std::call_once(g_logger_once_flag, []() {
    std::lock_guard<std::mutex> lock(g_logger_mutex);
    g_log_dir = NormalizeLogDirectory(g_log_dir);
    // Keep the rotation set bounded across application restarts.
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        g_log_dir + "/minirtc.log", 5 * 1024 * 1024, 3);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(file_sink);

    g_logger = std::make_shared<spdlog::logger>(LOGGER_NAME, sinks.begin(),
                                                sinks.end());
    g_logger->set_level(SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
                           ? spdlog::level::debug
                           : spdlog::level::info);
    g_logger->flush_on(spdlog::level::info);
    spdlog::register_logger(g_logger);
    g_logger->info("Logger initialized: path={}", file_sink->filename());
  });

  return g_logger;
}
}  // namespace minirtc

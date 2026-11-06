#include "log.h"
#include "minirtc.h"

#include <spdlog/async.h>
#include <spdlog/sinks/dist_sink.h>

#include <filesystem>

namespace minirtc {
namespace {
std::string g_log_dir = "logs";
std::once_flag g_logger_once_flag;
std::mutex g_logger_mutex;
std::mutex g_configuration_mutex;
std::shared_ptr<spdlog::sinks::dist_sink_mt> g_output;
std::shared_ptr<spdlog::details::thread_pool> g_log_pool;
std::shared_ptr<spdlog::logger> g_logger;
MiniRtcOnLog g_callback = nullptr;
void* g_callback_data = nullptr;
MiniRtcLogLevel g_level = SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
                             ? MiniRtcLogDebug : MiniRtcLogInfo;
thread_local bool g_in_callback = false;
constexpr spdlog::level::level_enum kLevels[] = {
    spdlog::level::trace, spdlog::level::debug, spdlog::level::info,
    spdlog::level::warn, spdlog::level::err, spdlog::level::critical,
    spdlog::level::off};

class CallbackSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  CallbackSink(MiniRtcOnLog callback, void* user_data)
      : callback_(callback), user_data_(user_data) {}
 private:
  void sink_it_(const spdlog::details::log_msg& message) override {
    const MiniRtcLogRecord record{
        static_cast<MiniRtcLogLevel>(message.level),
        std::chrono::duration_cast<std::chrono::microseconds>(
            message.time.time_since_epoch()).count(),
        message.source.filename, message.source.line,
        message.payload.data(), message.payload.size()};
    g_in_callback = true;
    try { callback_(&record, user_data_); } catch (...) {}
    g_in_callback = false;
  }
  void flush_() override {}
  MiniRtcOnLog callback_;
  void* user_data_;
};

std::string NormalizeLogDirectory(const std::string& log_dir) {
  const std::filesystem::path path = log_dir.empty() ? "logs" : log_dir;
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal().string();
}
std::vector<spdlog::sink_ptr> Outputs(const std::string& directory,
                                    MiniRtcLogLevel level,
                                    MiniRtcOnLog callback, void* user_data) {
  if (level == MiniRtcLogOff) return {};
  if (callback) return {std::make_shared<CallbackSink>(callback, user_data)};
  return {std::make_shared<spdlog::sinks::stdout_color_sink_mt>(),
          std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
              directory + "/minirtc.log", 5 * 1024 * 1024, 3)};
}
int ConfigureLogging(MiniRtcLogLevel level, MiniRtcOnLog callback, void* user_data) {
  if (level < MiniRtcLogTrace || level > MiniRtcLogOff) return -1;
  if (g_in_callback) return -2;
  std::lock_guard configuration_lock(g_configuration_mutex);
  std::unique_lock lock(g_logger_mutex);
  if (g_logger) {
    const auto logger = g_logger;
    const auto output = g_output;
    const auto directory = g_log_dir;
    lock.unlock();
    // Wait for in-flight output without holding the logger initialization lock.
    // The outer configuration lock serializes competing replacements.
    auto sinks = Outputs(directory, level, callback, user_data);
    output->set_sinks(sinks);
    logger->set_level(kLevels[level]);
    lock.lock();
  }
  g_level = level;
  g_callback = callback;
  g_callback_data = user_data;
  return 0;
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
    auto output = std::make_shared<spdlog::sinks::dist_sink_mt>();
    output->set_sinks(Outputs(g_log_dir, g_level, g_callback, g_callback_data));
    g_log_pool = std::make_shared<spdlog::details::thread_pool>(8192, 1);
    auto logger = std::make_shared<spdlog::async_logger>(
        LOGGER_NAME, output, g_log_pool,
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(kLevels[g_level]);
    logger->flush_on(spdlog::level::info);
    spdlog::register_logger(logger);
    g_output = std::move(output);
    g_logger = std::move(logger);
    if (!g_callback)
      g_logger->info("Logger initialized: path={}", g_log_dir + "/minirtc.log");
  });
  return g_logger;
}
}  // namespace minirtc

int MiniRtcConfigureLogging(MiniRtcLogLevel level, MiniRtcOnLog callback,
                           void* user_data) {
  try { return minirtc::ConfigureLogging(level, callback, user_data); }
  catch (...) { return -3; }
}

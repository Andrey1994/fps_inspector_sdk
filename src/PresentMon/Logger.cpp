#include <logger\spdlog.h>

std::shared_ptr<spdlog::logger> g_InspectorLogger = spdlog::stderr_logger_mt("eventLogger");

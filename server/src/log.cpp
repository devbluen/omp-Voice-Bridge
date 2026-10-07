/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "log.hpp"
#include <core.hpp>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace vbs
{
namespace
{
constexpr std::size_t kMaxQueued = 1024;

enum class Level
{
	Info,
	Warning,
	Error,
	Debug
};

std::mutex g_mutex;
std::deque<std::pair<Level, std::string>> g_queue;
std::size_t g_dropped = 0;
std::thread::id g_mainThread;
bool g_mainKnown = false;
ICore* g_core = nullptr;
void (*g_printer)(const char* format, ...) = nullptr;
std::atomic<bool> g_debug { false };

// Voice log file.  Written directly from any thread under its own lock, so
// worker thread events keep their real time.
constexpr long kMaxFileBytes = 5 * 1024 * 1024;
std::mutex g_fileMutex;
std::FILE* g_file = nullptr;
std::string g_filePath;

const char* levelName(Level level)
{
	switch (level)
	{
	case Level::Warning:
		return "Warning";
	case Level::Error:
		return "Error";
	case Level::Debug:
		return "Detail";
	default:
		return "Info";
	}
}

void writeFile(Level level, const std::string& message)
{
	std::lock_guard<std::mutex> lock(g_fileMutex);
	if (!g_file)
	{
		return;
	}
	if (std::ftell(g_file) > kMaxFileBytes)
	{
		// Keep one previous file: voice-bridge.log -> voice-bridge.log.old
		std::fclose(g_file);
		const std::string old = g_filePath + ".old";
		std::remove(old.c_str());
		std::rename(g_filePath.c_str(), old.c_str());
		g_file = std::fopen(g_filePath.c_str(), "a");
		if (!g_file)
		{
			return;
		}
	}
	const std::time_t t = std::time(nullptr);
	std::tm local {};
#ifdef _WIN32
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	char stamp[32];
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
	std::fprintf(g_file, "[%s] [%s] %s\n", stamp, levelName(level), message.c_str());
	std::fflush(g_file);
}

const char* prefix(Level level)
{
	switch (level)
	{
	case Level::Warning:
		return "[VoiceBridge] Warning: ";
	case Level::Error:
		return "[VoiceBridge] Error: ";
	case Level::Debug:
		return "[VoiceBridge] Debug: ";
	default:
		return "[VoiceBridge] ";
	}
}

void writeNow(Level level, const std::string& message)
{
	const std::string line = prefix(level) + message;
	if (g_core)
	{
		LogLevel ompLevel = LogLevel::Message;
		if (level == Level::Warning)
		{
			ompLevel = LogLevel::Warning;
		}
		else if (level == Level::Error)
		{
			ompLevel = LogLevel::Error;
		}
		else if (level == Level::Debug)
		{
			ompLevel = LogLevel::Debug;
		}
		g_core->logLn(ompLevel, "%s", line.c_str());
		return;
	}
	if (g_printer)
	{
		g_printer("%s", line.c_str());
		return;
	}
	std::fprintf(stderr, "%s\n", line.c_str());
}

void write(Level level, const char* format, va_list args)
{
	char buffer[1024];
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	std::string message(buffer);
	writeFile(level, message);
	if (level == Level::Debug && !g_debug)
	{
		return; // details go to the voice log only, unless voice_debug is on
	}

	if (g_mainKnown && std::this_thread::get_id() != g_mainThread)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_queue.size() < kMaxQueued)
		{
			g_queue.emplace_back(level, std::move(message));
		}
		else
		{
			++g_dropped;
		}
		return;
	}
	writeNow(level, message);
}
}

void LogSetMainThread()
{
	g_mainThread = std::this_thread::get_id();
	g_mainKnown = true;
}

void LogSetCore(ICore* core)
{
	g_core = core;
}

void LogSetPrinter(void (*printer)(const char* format, ...))
{
	g_printer = printer;
}

void LogSetDebug(bool enabled)
{
	g_debug = enabled;
}

bool LogDebugEnabled()
{
	return g_debug;
}

void LogFlush()
{
	std::deque<std::pair<Level, std::string>> pending;
	std::size_t dropped = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_queue.empty() && g_dropped == 0)
		{
			return;
		}
		pending.swap(g_queue);
		dropped = g_dropped;
		g_dropped = 0;
	}
	for (const auto& entry : pending)
	{
		writeNow(entry.first, entry.second);
	}
	if (dropped)
	{
		writeNow(Level::Warning, std::to_string(dropped) + " log line(s) were dropped");
	}
}

void LogInfo(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	write(Level::Info, format, args);
	va_end(args);
}

void LogWarning(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	write(Level::Warning, format, args);
	va_end(args);
}

void LogError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	write(Level::Error, format, args);
	va_end(args);
}

bool LogSetFile(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_fileMutex);
	if (g_file)
	{
		std::fclose(g_file);
		g_file = nullptr;
	}
	g_filePath = path;
	if (!path.empty())
	{
		g_file = std::fopen(path.c_str(), "a");
	}
	return g_file != nullptr;
}

void LogDebug(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	write(Level::Debug, format, args);
	va_end(args);
}
}

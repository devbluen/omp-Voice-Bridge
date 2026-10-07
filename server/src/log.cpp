/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "log.hpp"
#include <core.hpp>
#include <atomic>
#include <cstdio>
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

void LogDebug(const char* format, ...)
{
	if (!g_debug)
	{
		return;
	}
	va_list args;
	va_start(args, format);
	write(Level::Debug, format, args);
	va_end(args);
}
}

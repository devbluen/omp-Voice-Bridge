/*
 *  Voice Bridge client
 */

#include "log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vbc
{
namespace
{
std::mutex g_mutex;
FILE* g_file = nullptr;
}

const std::string& GameDirectory()
{
	static const std::string directory = []
	{
		char path[MAX_PATH] {};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string result(path);
		const auto slash = result.find_last_of("\\/");
		return slash == std::string::npos ? std::string() : result.substr(0, slash + 1);
	}();
	return directory;
}

void LogOpen(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
	{
		g_file = std::fopen(path.c_str(), "w");
	}
}

void LogClose()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file)
	{
		std::fclose(g_file);
		g_file = nullptr;
	}
}

void TryLog(const char* text)
{
	SYSTEMTIME time {};
	GetLocalTime(&time);
	std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_file)
	{
		return;
	}
	std::fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, text);
	std::fflush(g_file);
}

void Log(const char* format, ...)
{
	char message[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	SYSTEMTIME time {};
	GetLocalTime(&time);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
	{
		return;
	}
	std::fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message);
	std::fflush(g_file);
}
}

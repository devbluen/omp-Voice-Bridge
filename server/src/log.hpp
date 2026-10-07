/*
 *  Voice Bridge for open.mp and SA-MP
 */

#pragma once

#include <cstdarg>
#include <string>

struct ICore;

namespace vbs
{
// Thread-safe logging.  Lines written from worker threads are queued and
// flushed by LogFlush on the server thread, because neither SA-MP's logprintf
// nor the open.mp logger may be called from another thread.
void LogSetMainThread();
void LogSetCore(ICore* core);
void LogSetPrinter(void (*printer)(const char* format, ...));
void LogSetDebug(bool enabled);
bool LogDebugEnabled();
void LogFlush();

void LogInfo(const char* format, ...);
void LogWarning(const char* format, ...);
void LogError(const char* format, ...);
void LogDebug(const char* format, ...);
}

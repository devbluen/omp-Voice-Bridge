/*
 *  Voice Bridge client
 */

#pragma once

#include <string>

namespace vbc
{
// Writes to voicebridge.log next to gta_sa.exe.  Thread-safe.
void LogOpen(const std::string& path);
void LogClose();
void Log(const char* format, ...);
// Writes a line only if the log is free: safe inside an exception handler.
void TryLog(const char* text);

// Folder that contains gta_sa.exe (with a trailing slash).
const std::string& GameDirectory();
}

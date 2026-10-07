/*
 *  Voice Bridge client
 */

#include "crash.hpp"
#include "log.hpp"
#include "samp.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace vbc::crash
{
namespace
{
// Each faulting address is reported once: protected samp.dll builds may
// raise and handle exceptions on purpose, a real crash is usually the last
// line before "game closing" (or the end of the log).
constexpr int kMaxReports = 32;
std::atomic<int> g_reports { 0 };
std::atomic<uintptr_t> g_seen[kMaxReports] {};

bool firstTime(uintptr_t address)
{
	for (auto& seen : g_seen)
	{
		uintptr_t expected = 0;
		if (seen.load() == address)
		{
			return false;
		}
		if (seen.compare_exchange_strong(expected, address))
		{
			return true;
		}
		if (expected == address)
		{
			return false;
		}
	}
	return false;
}

bool serious(DWORD code)
{
	switch (code)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_IN_PAGE_ERROR:
	case 0xC0000374: // heap corruption
		return true;
	default:
		return false;
	}
}

// "module.dll+0x1234", or the raw address when it is in no module (code of
// a DLL that was unloaded, a corrupted pointer...).
std::string where(const void* address)
{
	HMODULE module = nullptr;
	char path[MAX_PATH] {};
	char text[MAX_PATH + 32];
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module)
		&& GetModuleFileNameA(module, path, MAX_PATH))
	{
		const char* name = std::strrchr(path, '\\');
		std::snprintf(text, sizeof(text), "%s+0x%X", name ? name + 1 : path,
			static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
		return text;
	}
	std::snprintf(text, sizeof(text), "0x%08X (no module)", static_cast<unsigned>(reinterpret_cast<uintptr_t>(address)));
	return text;
}

bool executable(const void* address)
{
	MEMORY_BASIC_INFORMATION info {};
	if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT || info.Type != MEM_IMAGE)
	{
		return false;
	}
	return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

LONG CALLBACK handler(EXCEPTION_POINTERS* info)
{
	const EXCEPTION_RECORD* record = info->ExceptionRecord;
	if (!serious(record->ExceptionCode) || samp::InSafeRead() || !firstTime(reinterpret_cast<uintptr_t>(record->ExceptionAddress))
		|| g_reports.fetch_add(1) >= kMaxReports)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const CONTEXT* context = info->ContextRecord;
	char line[512];
	std::string text = "exception 0x";
	std::snprintf(line, sizeof(line), "%08X at %s, thread %lu", static_cast<unsigned>(record->ExceptionCode), where(record->ExceptionAddress).c_str(),
		GetCurrentThreadId());
	text += line;
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
	{
		const ULONG_PTR kind = record->ExceptionInformation[0];
		std::snprintf(line, sizeof(line), ", %s 0x%08X", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
			static_cast<unsigned>(record->ExceptionInformation[1]));
		text += line;
	}
	TryLog(text.c_str());

	// Callers: return addresses found on the stack (works without frame
	// pointers, may list a few stale ones).
	std::string stack = "  stack:";
	int found = 0;
	const auto* slot = reinterpret_cast<const uintptr_t*>(context->Esp);
	for (int i = 0; i < 256 && found < 12; ++i)
	{
		uintptr_t value = 0;
		if (!samp::SafeRead(slot + i, &value, sizeof(value)))
		{
			break;
		}
		if (value > 0x10000 && executable(reinterpret_cast<const void*>(value)))
		{
			stack += " " + where(reinterpret_cast<const void*>(value));
			++found;
		}
	}
	TryLog(stack.c_str());
	return EXCEPTION_CONTINUE_SEARCH;
}
}

void Install()
{
	AddVectoredExceptionHandler(1, &handler);
}
}

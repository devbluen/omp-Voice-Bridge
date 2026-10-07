/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "memory.hpp"
#include <cstring>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#else
	#ifndef _GNU_SOURCE
		#define _GNU_SOURCE
	#endif
	#include <link.h>
	#include <sys/mman.h>
	#include <unistd.h>
#endif

namespace vbs::mem
{
#ifdef _WIN32
bool CodeRange(const void* address, uint8_t*& start, std::size_t& size)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCSTR>(address), &module) || !module)
	{
		return false;
	}
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
	{
		if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
		{
			start = base + section->VirtualAddress;
			size = section->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

bool MakeWritable(void* address, std::size_t size)
{
	DWORD previous = 0;
	return VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous) != FALSE;
}
#else
namespace
{
struct SearchState
{
	uintptr_t address;
	uintptr_t start;
	uintptr_t end;
	bool found;
};

int findModule(dl_phdr_info* info, std::size_t, void* data)
{
	auto* state = static_cast<SearchState*>(data);
	uintptr_t low = UINTPTR_MAX;
	uintptr_t high = 0;
	bool contains = false;
	for (int i = 0; i < info->dlpi_phnum; ++i)
	{
		const auto& header = info->dlpi_phdr[i];
		if (header.p_type != PT_LOAD || !(header.p_flags & PF_X))
		{
			continue;
		}
		const uintptr_t begin = info->dlpi_addr + header.p_vaddr;
		const uintptr_t end = begin + header.p_memsz;
		low = begin < low ? begin : low;
		high = end > high ? end : high;
		if (state->address >= begin && state->address < end)
		{
			contains = true;
		}
	}
	if (!contains)
	{
		return 0;
	}
	state->start = low;
	state->end = high;
	state->found = true;
	return 1;
}
}

bool CodeRange(const void* address, uint8_t*& start, std::size_t& size)
{
	SearchState state { reinterpret_cast<uintptr_t>(address), 0, 0, false };
	dl_iterate_phdr(findModule, &state);
	if (!state.found)
	{
		return false;
	}
	start = reinterpret_cast<uint8_t*>(state.start);
	size = state.end - state.start;
	return true;
}

bool MakeWritable(void* address, std::size_t size)
{
	const long pageSize = sysconf(_SC_PAGESIZE);
	const uintptr_t begin = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(pageSize - 1);
	const uintptr_t end = reinterpret_cast<uintptr_t>(address) + size;
	return mprotect(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}
#endif

uint8_t* FindPattern(uint8_t* start, std::size_t size, const char* pattern, const char* mask)
{
	const std::size_t length = std::strlen(mask);
	if (!start || length == 0 || size < length)
	{
		return nullptr;
	}
	for (std::size_t i = 0; i + length <= size; ++i)
	{
		std::size_t j = 0;
		for (; j < length; ++j)
		{
			if (mask[j] == 'x' && start[i + j] != static_cast<uint8_t>(pattern[j]))
			{
				break;
			}
		}
		if (j == length)
		{
			return start + i;
		}
	}
	return nullptr;
}

void* PatchVirtual(void* object, std::size_t index, void* replacement)
{
	void** table = *static_cast<void***>(object);
	void* previous = table[index];
	if (MakeWritable(&table[index], sizeof(void*)))
	{
		table[index] = replacement;
	}
	return previous;
}

bool JumpHook::install(void* target, void* hook)
{
	if (target_ || !target || !MakeWritable(target, sizeof(original_)))
	{
		return false;
	}
	target_ = static_cast<uint8_t*>(target);
	std::memcpy(original_, target_, sizeof(original_));
	const int32_t relative = static_cast<int32_t>(reinterpret_cast<intptr_t>(hook) - reinterpret_cast<intptr_t>(target_) - 5);
	target_[0] = 0xE9;
	std::memcpy(target_ + 1, &relative, sizeof(relative));
	return true;
}

void JumpHook::remove()
{
	if (!target_)
	{
		return;
	}
	std::memcpy(target_, original_, sizeof(original_));
	target_ = nullptr;
}
}

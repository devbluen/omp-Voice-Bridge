/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Minimal x86 memory helpers for the SA-MP server hooks.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace vbs::mem
{
// Executable range of the module that contains `address`.
bool CodeRange(const void* address, uint8_t*& start, std::size_t& size);

// Pattern with '?' as wildcard in `mask`.
uint8_t* FindPattern(uint8_t* start, std::size_t size, const char* pattern, const char* mask);

bool MakeWritable(void* address, std::size_t size);

// Replaces one virtual table slot and returns the previous function.
void* PatchVirtual(void* object, std::size_t index, void* replacement);

// One shot 5 byte jump hook.
class JumpHook
{
public:
	bool install(void* target, void* hook);
	void remove();
	bool installed() const noexcept { return target_ != nullptr; }
	void* target() const noexcept { return target_; }

private:
	uint8_t* target_ = nullptr;
	uint8_t original_[5] {};
};
}

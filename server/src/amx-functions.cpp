/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Portable thunks for the AMX functions this plugin uses.  They forward to
 *  the function table handed over by the loader (SA-MP Load data or
 *  IPawnComponent::getAmxFunctions on open.mp), which share one layout.
 */

#include <amx/amx.h>
#include <plugincommon.h>

void* pAMXFunctions = nullptr;

namespace
{
template <typename Function>
Function entry(int index)
{
	return reinterpret_cast<Function>(static_cast<void**>(pAMXFunctions)[index]);
}
}

int AMXAPI amx_Allot(AMX* amx, int cells, cell* amx_addr, cell** phys_addr)
{
	using Fn = int(AMXAPI*)(AMX*, int, cell*, cell**);
	return entry<Fn>(PLUGIN_AMX_EXPORT_Allot)(amx, cells, amx_addr, phys_addr);
}

int AMXAPI amx_Exec(AMX* amx, cell* retval, int index)
{
	using Fn = int(AMXAPI*)(AMX*, cell*, int);
	return entry<Fn>(PLUGIN_AMX_EXPORT_Exec)(amx, retval, index);
}

int AMXAPI amx_FindPublic(AMX* amx, const char* funcname, int* index)
{
	using Fn = int(AMXAPI*)(AMX*, const char*, int*);
	return entry<Fn>(PLUGIN_AMX_EXPORT_FindPublic)(amx, funcname, index);
}

int AMXAPI amx_GetAddr(AMX* amx, cell amx_addr, cell** phys_addr)
{
	using Fn = int(AMXAPI*)(AMX*, cell, cell**);
	return entry<Fn>(PLUGIN_AMX_EXPORT_GetAddr)(amx, amx_addr, phys_addr);
}

int AMXAPI amx_GetString(char* dest, const cell* source, int use_wchar, size_t size)
{
	using Fn = int(AMXAPI*)(char*, const cell*, int, size_t);
	return entry<Fn>(PLUGIN_AMX_EXPORT_GetString)(dest, source, use_wchar, size);
}

int AMXAPI amx_Push(AMX* amx, cell value)
{
	using Fn = int(AMXAPI*)(AMX*, cell);
	return entry<Fn>(PLUGIN_AMX_EXPORT_Push)(amx, value);
}

int AMXAPI amx_Register(AMX* amx, const AMX_NATIVE_INFO* nativelist, int number)
{
	using Fn = int(AMXAPI*)(AMX*, const AMX_NATIVE_INFO*, int);
	return entry<Fn>(PLUGIN_AMX_EXPORT_Register)(amx, nativelist, number);
}

int AMXAPI amx_Release(AMX* amx, cell amx_addr)
{
	using Fn = int(AMXAPI*)(AMX*, cell);
	return entry<Fn>(PLUGIN_AMX_EXPORT_Release)(amx, amx_addr);
}

int AMXAPI amx_SetString(cell* dest, const char* source, int pack, int use_wchar, size_t size)
{
	using Fn = int(AMXAPI*)(cell*, const char*, int, int, size_t);
	return entry<Fn>(PLUGIN_AMX_EXPORT_SetString)(dest, source, pack, use_wchar, size);
}

int AMXAPI amx_StrLen(const cell* cstring, int* length)
{
	using Fn = int(AMXAPI*)(const cell*, int*);
	return entry<Fn>(PLUGIN_AMX_EXPORT_StrLen)(cstring, length);
}

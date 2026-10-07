/*
 *  Voice Bridge for open.mp and SA-MP
 */

#pragma once

namespace vbs
{
// One binary serves both loaders.  Whichever entry point runs first owns the
// process so a misconfigured server cannot start the voice server twice.
enum class Loader
{
	None,
	Component,
	SampPlugin,
};

inline Loader& LoaderModeStorage()
{
	static Loader mode = Loader::None;
	return mode;
}

inline Loader LoaderMode()
{
	return LoaderModeStorage();
}

inline void SetLoaderMode(Loader mode)
{
	LoaderModeStorage() = mode;
}
}

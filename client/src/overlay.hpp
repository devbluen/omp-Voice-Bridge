/*
 *  Voice Bridge client
 *
 *  Hooks IDirect3DDevice9::Present/Reset to run the voice client on the game
 *  thread every frame and to draw the interface with Dear ImGui.
 */

#pragma once

namespace vbc::overlay
{
// Waits for GTA's Direct3D device (call from a worker thread).
bool Install();
void Shutdown();
bool MenuOpen();
}

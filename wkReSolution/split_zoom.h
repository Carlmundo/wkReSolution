#pragma once
#include <windows.h>
#include <ddraw.h>

namespace SplitZoom {
// Only installs on the verified English Worms 2 1.05 rendering ABI.
bool Install();
void Remove(bool processTerminating = false);
bool Enabled();
// Keep cnc-ddraw's Worms 2 virtual-resolution detector from applying a
// second whole-screen upscale. The independent renderer owns the world
// scaling, so the wrapper must see the physical presentation dimensions.
void SyncPresentationSize(DWORD width, DWORD height);
bool ResizeWorld(DWORD width, DWORD height, bool redraw);
void ChangeUI(int direction);
void ResetUI();
LPDIRECTDRAW DrawingInterface();
}

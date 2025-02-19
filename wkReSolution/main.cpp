
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include "main.h"
#include "hooks.h"
#include "w2res.h"
#include "misc_tools.h"

void LoadConfig()
{
	//Determine if the OS is Windows XP
	BOOL OSWinXP;
	OSVERSIONINFOEX InfoOS;
	ZeroMemory(&InfoOS, sizeof(OSVERSIONINFOEX));
	InfoOS.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	GetVersionEx((LPOSVERSIONINFO)&InfoOS);
	if (InfoOS.dwMajorVersion < 6) {
		OSWinXP = true;
	}
	else {
		OSWinXP = false;
	}

	GetPathUnderModuleA(HINST_THISCOMPONENT, Config, "ReSolution.ini");

	ScreenCX = (SHORT)GetSystemMetrics(SM_CXSCREEN);
	ScreenCY = (SHORT)GetSystemMetrics(SM_CYSCREEN);

	WinMinWidth = GetSystemMetrics(SM_CXMIN) - GetSystemMetrics(SM_CXSIZEFRAME) * 2;

	if (!WWP && Cavern) {
		SWidth = GetPrivateProfileIntA("Resolution", "CavernScreenWidth", -1, Config);
		SHeight = GetPrivateProfileIntA("Resolution", "CavernScreenHeight", -1, Config);
	}
	else {
		SWidth = GetPrivateProfileIntA("Resolution", "ScreenWidth", -1, Config);
		SHeight = GetPrivateProfileIntA("Resolution", "ScreenHeight", -1, Config);
	}

	if (OSWinXP && !UsingCncDdraw) { //Disable features that are not compatible with Windows XP
		AllowResize = 0;
		ProgressiveResize = 0;
		AltEnter = 0;

		AllowZoom = 0;
		UseMouseWheel = 0;
		UseKeyboardZoom = 0;
		UseTouchscreenZoom = 0;
	}
	else {
		AllowResize = GetPrivateProfileIntA("Resizing", "Enable", -1, Config);
		AltEnter = GetPrivateProfileIntA("Resizing", "AltEnter", -1, Config);

		AllowZoom = GetPrivateProfileIntA("Zooming", "Enable", -1, Config);
		UseMouseWheel = GetPrivateProfileIntA("Zooming", "UseMouseWheel", -1, Config);
		UseKeyboardZoom = GetPrivateProfileIntA("Zooming", "UseKeyboardZoom", -1, Config);
		UseTouchscreenZoom = GetPrivateProfileIntA("Zooming", "UseTouchscreenZoom", -1, Config);

		if (!WWP) {
			ProgressiveResize = GetPrivateProfileIntA("Resizing", "ProgressiveUpdate", -1, Config);
		}
	}

	if (SWidth > 32767 || SHeight > 32767 || SWidth == 0 || SHeight == 0)
	{
		WritePrivateProfileIntA("Resolution", "ScreenWidth", SWidth = ScreenCX, Config);
		WritePrivateProfileIntA("Resolution", "ScreenHeight", SHeight = ScreenCY, Config);
	}

	if (AllowResize < 0)
		WritePrivateProfileIntA("Resizing", "Enable", AllowResize = 1, Config);
	if (ProgressiveResize < 0)
		WritePrivateProfileIntA("Resizing", "ProgressiveUpdate", ProgressiveResize = 1, Config);
	if (AltEnter < 0)
		WritePrivateProfileIntA("Resizing", "AltEnter", AltEnter = 1, Config);
	if (AllowZoom < 0)
		WritePrivateProfileIntA("Zooming", "Enable", AllowZoom = 1, Config);
	if (UseMouseWheel < 0)
		WritePrivateProfileIntA("Zooming", "UseMouseWheel", UseMouseWheel = 1, Config);
	if (UseKeyboardZoom < 0)
		WritePrivateProfileIntA("Zooming", "UseKeyboardZoom", UseKeyboardZoom = 1, Config);

	InitializeScreenSize();
}

BOOL APIENTRY DllMain(HMODULE, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		if (!CheckVersion())
		{
			MessageBoxA(NULL,
				"Sorry, but your game version is not compatible with this wkReSolution HD module. "
				"Please check that the game is patched to one of either:\nWWP: 1.01 (EU SP1) or 1.00\nWorms 2: 1.05", "ReSolution error",
				MB_OK | MB_ICONERROR);
			return 1;
		}
		if (!WWP){
			CavernCheck();
		}
		LoadConfig();
		PrepareAddresses();
		if (!WWP){
			PatchW2Mem(SWidth, SHeight);
		}
		InstallHooks();

	}
	else if (dwReason == DLL_PROCESS_DETACH)
	{
		UninstallHooks();
	}

	return 1;
}


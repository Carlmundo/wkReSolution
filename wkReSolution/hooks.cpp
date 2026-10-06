
#include <windows.h>
#include "hooks.h"
#include "w2res.h"
#include "split_zoom.h"

HHOOK wHook, kHook, mHook;
BOOL ModifiedSurfaces;

UINT KeyZoomIn = VK_ADD;
UINT KeyZoomOut = VK_SUBTRACT;
UINT KeyNumpadPlus = VK_ADD;
UINT KeyNumpadMinus = VK_SUBTRACT;
UINT KeyZoomReset = VK_END;

DWORD TWidth, THeight, LastWidth, LastHeight;
DOUBLE DTWidth, DTHeight, DDif;

BOOL UsingCncDdraw = GetProcAddress(GetModuleHandleA("ddraw.dll"), "GameHandlesClose") != NULL;

BOOL DZoom(DOUBLE& dCX, DOUBLE& dCY, DOUBLE dDif, SHORT sDelta)
{
	BOOL result = 0;
	const DOUBLE oldCX = dCX, oldCY = dCY;

	DOUBLE ddCX = dCX + sDelta;
	DOUBLE ddCY = dCY + sDelta * dDif;

	if ((sDelta > 0 && ddCX <= 32767 && ddCY <= 32767) || (sDelta < 0 && ddCX >= WinMinWidth && ddCY > 0))
	{
		//Don't allow zooming less than the initial window size (as this caused graphical glitches)
		DWORD width = 0, height = 0;
		if (!GetWndSize(WormsWnd(), width, height) || !width || !height)
			return FALSE;
		if (ddCX > width) {
			dCX = width;
		}
		else if (ddCX < 240) {
			dCX = 240;
		}
		else {
			dCX = ddCX;
		}
		if (ddCY > height) {
			dCY = height;
		}
		else if (ddCX < 240) {
			dCY = 240 * dDif;
		}
		else {
			dCY = ddCY;
		}
		result = dCX != oldCX || dCY != oldCY;
	}
	return result;
}

BOOL ReNormalizeBuffers()
{
	BOOL result = 0;

	DWORD width = 0, height = 0;
	if (!GetWndSize(WormsWnd(), width, height) || !width || !height)
		return FALSE;

	if (HandleBufferResize(width, height))
	{
		DTWidth = width;
		DTHeight = height;
		DDif = DTHeight / DTWidth;
		result = 1;
	}

	return result;
}

BOOL CleanupSurfaces()
{
	// Split zoom owns its world surface and clears it at the next frame.
	if (SplitZoom::Enabled()) return TRUE;
	BOOL result = 0;

	if (DDObj())
	{
		DDSURFACEDESC prefDesc;
		prefDesc.dwSize = sizeof(DDSURFACEDESC);
		prefDesc.dwFlags = DDSD_CAPS;
		prefDesc.ddsCaps.dwCaps = 0x8A00;
		if (SUCCEEDED(DDObj()->EnumSurfaces(DDENUMSURFACES_DOESEXIST | DDENUMSURFACES_NOMATCH, &prefDesc, NULL, EnumResize)))
			result = TRUE;
	}
	return result;
}

BOOL HandleBufferResize(DWORD nWidth, DWORD nHeight, bool bRedraw)
{
	if (SplitZoom::Enabled())
		return SplitZoom::ResizeWorld(nWidth, nHeight, bRedraw);
	BOOL result = 0;
	if ((UsingCncDdraw || DDObj()) && nWidth <= 32767 && nHeight <= 32767 && nWidth && nHeight)
	{
		TWidth = nWidth;
		THeight = nHeight;
		ModifiedSurfaces = 0;
		DDSURFACEDESC prefDesc;
		prefDesc.dwSize = sizeof(DDSURFACEDESC);
		prefDesc.dwFlags = DDSD_CAPS;
		prefDesc.ddsCaps.dwCaps = 0x8A00;
		if (LastWidth != TWidth || LastHeight != THeight)
			if (UsingCncDdraw || SUCCEEDED(DDObj()->EnumSurfaces(DDENUMSURFACES_DOESEXIST | DDENUMSURFACES_NOMATCH, &prefDesc, NULL, EnumResize)))
		{
			LastWidth = TWidth;
			LastHeight = THeight;
			if (WWP)
			{
				if (IsWindow(T17Wnd))
				{
					RECT WWPRect;
					if (GetClientRect(WormsWnd(), &WWPRect))
						SetWindowPos(T17Wnd, NULL, 0, 0, WWPRect.right, WWPRect.bottom, SWP_NOMOVE | SWP_NOZORDER);
				}
				GetTargetScreenSize(TWidth, THeight);
				SetWWPRenderingDimensions(TargetWidth, TargetHeight, true);
			}
			else
			{
				PatchW2Mem(TWidth, THeight, true);
				W2DS->RenderWidth = TargetWidth;
				W2DS->RenderHeight = TargetHeight;
				if (ModifiedSurfaces && (ProgressiveResize || bRedraw))
					RenderGame(); //Experimental: rerender the scene right after resizing
			}
			result = 1;
		}
	}

	return result;
}

//HACK
HRESULT WINAPI EnumResize(LPDIRECTDRAWSURFACE pSurface, LPDDSURFACEDESC lpSurfaceDesc, LPVOID)
{
	BOOL bRequiredSurface = (!!!(lpSurfaceDesc->dwFlags & DDSD_CKSRCBLT) && lpSurfaceDesc->dwWidth == LastWidth && lpSurfaceDesc->dwHeight == LastHeight);
	BOOL bPrimary          = (!!(lpSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE));

	if (bRequiredSurface && !bPrimary)
	{
		LONG lsz  = sizeof(LONG);
		LONG lmod = 8;
		if ((DWORD)lpSurfaceDesc->lPitch != RoundUp(lpSurfaceDesc->dwWidth, 8))
			lmod  = 2;

		DWORD   dwSurfPtr    = *(PDWORD)((DWORD)pSurface + lsz);
		DWORD   dwDataPtr    = *(PDWORD)dwSurfPtr;
		DWORD   dwInfoPtr    = *(PDWORD)(dwSurfPtr + lsz * 2);
		DWORD   dwOldPitchM  = lpSurfaceDesc->lPitch / RoundUp(lpSurfaceDesc->dwWidth, lmod);
		DWORD   dwNewPitch   = RoundUp(TWidth, lmod) * dwOldPitchM;
		DWORD   dwNewMemSize = dwNewPitch * THeight;
		PDWORD lpSurfMemSize = (PDWORD)(dwDataPtr + 0x10);
		PDWORD lpSurfMemAddr = (PDWORD)(dwDataPtr + 0xA8);
		PWORD  lpDataWidth   = (PWORD)(dwDataPtr + 0xB2);
		PWORD  lpDataHeight  = (PWORD)(dwDataPtr + 0xB0);
		PDWORD lpDataPitch   = (PDWORD)(dwDataPtr + 0xAC);
		PDWORD lpInfoWidth   = (PDWORD)(dwInfoPtr + 0x28);
		PDWORD lpInfoHeight  = (PDWORD)(dwInfoPtr + 0x2C);
		PDWORD lpInfoPitch   = (PDWORD)(dwInfoPtr + 0x50);
		PDWORD lpInfoMemAddr = (PDWORD)(dwInfoPtr + 0x4C);

		HLOCAL MemAlloc = LocalHandle((LPCVOID)(*lpSurfMemAddr - lsz * 2));

		if ((MemAlloc = LocalReAlloc(MemAlloc, dwNewMemSize + lsz * 2, LMEM_MOVEABLE)) != 0)
		{
			PVOID NewMemPtr = LocalLock(MemAlloc);
			if (NewMemPtr)
			{
				__try {
					*lpSurfMemSize = dwNewMemSize;
					*lpSurfMemAddr = (DWORD)NewMemPtr + lsz * 2;
					*lpDataWidth = LOWORD(TWidth);
					*lpDataPitch = dwNewPitch;
					*lpInfoWidth = TWidth;
					*lpInfoPitch = dwNewPitch;
					*lpInfoMemAddr = *lpSurfMemAddr;

					*lpDataHeight = LOWORD(THeight);
					*lpInfoHeight = THeight;

					//cleanup
					memset((PVOID)(*lpSurfMemAddr), 0, dwNewMemSize);

					ModifiedSurfaces++;
				}
				__finally {
					LocalUnlock(MemAlloc);
				}
			}
		}
	}
	return DDENUMRET_OK;
}

HRESULT WINAPI EnumCleanup(LPDIRECTDRAWSURFACE pSurface, LPDDSURFACEDESC lpSurfaceDesc, LPVOID)
{
	BOOL bRequiredSurface = (!!!(lpSurfaceDesc->dwFlags & DDSD_CKSRCBLT) && lpSurfaceDesc->dwWidth == LastWidth && lpSurfaceDesc->dwHeight == LastHeight);
	BOOL bPrimary = (!!(lpSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE));

	if (bRequiredSurface && !bPrimary)
	{
		DWORD  dwSurfPtr = *(PDWORD)((DWORD)pSurface + sizeof(LONG));
		DWORD  dwDataPtr = *(PDWORD)dwSurfPtr;
		PDWORD lpSurfMemSize = (PDWORD)(dwDataPtr + 0x10);
		PDWORD lpSurfMemAddr = (PDWORD)(dwDataPtr + 0xA8);

		HLOCAL MemAlloc = LocalHandle((LPCVOID)(*lpSurfMemAddr - sizeof(LONG)* 2));
		if (LocalLock(MemAlloc))
		{
			__try{
				memset((PVOID)(*lpSurfMemAddr), 0, *lpSurfMemSize);
			}
			__finally{
				LocalUnlock(MemAlloc);
			}
		}
	}
	return DDENUMRET_OK;
}

BOOL WheelZoom(SHORT sDelta)
{
	if (sDelta != 0)
	{
		if (DZoom(DTWidth, DTHeight, DDif, -sDelta))
			return HandleBufferResize((DWORD)DTWidth, (DWORD)DTHeight);
	}
	return 0;
}

// With independent zoom active, Ctrl + wheel adjusts the UI scale. Keep
// partial wheel deltas so high-resolution wheels still move in 10% steps
// once a complete WHEEL_DELTA has accumulated.
int uiWheelRemainder;

void HandleMouseWheel(SHORT sDelta)
{
	if (sDelta == 0)
		return;

	if (UseMouseWheel && KeyPressed(VK_CONTROL) && SplitZoom::Enabled())
	{
		uiWheelRemainder += sDelta;
		const int steps = uiWheelRemainder / WHEEL_DELTA;
		if (steps != 0)
		{
			uiWheelRemainder -= steps * WHEEL_DELTA;
			SplitZoom::ChangeUI(steps);
		}
		return;
	}

	uiWheelRemainder = 0;
	WheelZoom(sDelta);
}

LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && InGame())
	{
		CWPSTRUCT* pwp = (CWPSTRUCT*)lParam;
		if (pwp->message == WM_WINDOWPOSCHANGED)
		{
			if (pwp->hwnd == WormsWnd())
			{
				LPWINDOWPOS lwp = (LPWINDOWPOS)(pwp->lParam);
				if (!!!(lwp->flags & SWP_NOSIZE) && !!!(lwp->flags & SWP_NOCOPYBITS) && !!!(lwp->flags & SWP_NOSENDCHANGING))
				{
					ReNormalizeBuffers();
				}
			}
		}
		else if (pwp->message == WM_MOUSEWHEEL && UseTouchscreenZoom)
		{
			if (!!(pwp->wParam & MK_CONTROL))
			{
				// UseMouseWheel routes this same Ctrl+wheel event through
				// MouseProc. Avoid applying a second world zoom here.
				if (!(UseMouseWheel && SplitZoom::Enabled()))
					WheelZoom(GET_WHEEL_DELTA_WPARAM(pwp->wParam));
			}
		}
	}

	return CallNextHookEx(wHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && InGame())
	{
		if (wParam == WM_MOUSEWHEEL)
		{
			LPMOUSEHOOKSTRUCTEX lpWheelInf = (LPMOUSEHOOKSTRUCTEX)lParam;
			HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(lpWheelInf->mouseData));
		}
		else if (wParam == WM_MBUTTONDOWN)
		{
			if (SplitZoom::Enabled() && KeyPressed(VK_CONTROL))
				SplitZoom::ResetUI();
			else
				ReNormalizeBuffers();
		}
	}
	return CallNextHookEx(mHook, nCode, wParam, lParam);
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	BOOL consumeZoomKey = FALSE;

	if (nCode == HC_ACTION && InGame())
	{
		// Use the existing bindings, with Ctrl selecting the independent UI.
		// Windows key repeat supplies the steps; don't spin to the scale limit.
		if (UseKeyboardZoom && SplitZoom::Enabled() && KeyPressed(VK_CONTROL) &&
			(wParam == KeyZoomIn || wParam == KeyZoomOut || wParam == KeyZoomReset))
		{
			if (!(lParam & INT_MIN))
			{
				if (wParam == KeyZoomReset) SplitZoom::ResetUI();
				else SplitZoom::ChangeUI(wParam == KeyZoomIn ? 1 : -1);
			}
			return 1;
		}
		if (UseKeyboardZoom)
		{
			// Only consume the zoom key when it is also the corresponding
			// Numpad +/- key.
			if ((wParam == KeyZoomIn && KeyZoomIn == KeyNumpadPlus) ||
				(wParam == KeyZoomOut && KeyZoomOut == KeyNumpadMinus))
			{
				consumeZoomKey = TRUE;
			}
		}

		if (!!!(lParam & INT_MIN)) // key is in a held state
		{
			if (UseKeyboardZoom)
			{
				if (wParam == KeyZoomOut)
				{
					do if (DZoom(DTWidth, DTHeight, DDif, 120))
						HandleBufferResize((SHORT)DTWidth, (SHORT)DTHeight);
					else break;
					while (ProgressiveResize && KeyPressed(KeyZoomOut));
				}
				else if (wParam == KeyZoomIn)
				{
					do if (DZoom(DTWidth, DTHeight, DDif, -120))
						HandleBufferResize((SHORT)DTWidth, (SHORT)DTHeight);
					else break;
					while (ProgressiveResize && KeyPressed(KeyZoomIn));
				}
				else if (wParam == KeyZoomReset)
				{
					ReNormalizeBuffers();
				}
			}

			if (AltEnter)
			{
				if (wParam == VK_RETURN)
				{
					if (!!(lParam & 0x20000000) && DDObj()) //Alt is pressed and DD is there
					{
						DWORD width, height;
						GetWndSize(WormsWnd(), width, height);

						if (width >= ScreenCX && height >= ScreenCY)
							DDObj()->SetDisplayMode(AeWidth, AeHeight, 0);
						else
						{
							AeWidth = width;
							AeHeight = height;
							DDObj()->SetDisplayMode(ScreenCX, ScreenCY, 0);
						}

						ReNormalizeBuffers();
					}
				}
			}
		}
	}

	if (consumeZoomKey)
		return 1;

	return CallNextHookEx(kHook, nCode, wParam, lParam);
}

void InstallHooks()
{
	if (AllowResize)
	{
		if (!wHook) wHook = SetWindowsHookEx(WH_CALLWNDPROC, (HOOKPROC)CallWndProc, 0, GetCurrentThreadId());
		if (AltEnter && !kHook) kHook = SetWindowsHookEx(WH_KEYBOARD, (HOOKPROC)KeyboardProc, 0, GetCurrentThreadId());
	}
	if (AllowZoom)
	{
		if (UseTouchscreenZoom && !wHook) wHook = SetWindowsHookEx(WH_CALLWNDPROC, (HOOKPROC)CallWndProc, 0, GetCurrentThreadId());
		if (UseKeyboardZoom && !kHook) kHook = SetWindowsHookEx(WH_KEYBOARD, (HOOKPROC)KeyboardProc, 0, GetCurrentThreadId());
		if (UseMouseWheel   && !mHook) mHook = SetWindowsHookEx(WH_MOUSE, (HOOKPROC)MouseProc, 0, GetCurrentThreadId());
	}
}

void UninstallHooks()
{
	if (wHook) UnhookWindowsHookEx(wHook);
	if (kHook) UnhookWindowsHookEx(kHook);
	if (mHook) UnhookWindowsHookEx(mHook);
}

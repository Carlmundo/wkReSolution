// Worms 2 English 1.05: render the world into its own DirectDraw surface,
// stretch it once, then render screen-space UI at the chosen UI scale.
// All addresses below are RVAs verified against the original 1998 executable.
#include "split_zoom.h"
#include "hooks.h"
#include "w2res.h"
#include "zoom_math.h"
#include <intrin.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>

namespace SplitZoom {
namespace {

template<class T> T& Field(void* p, unsigned offset) {
    return *reinterpret_cast<T*>(static_cast<BYTE*>(p) + offset);
}
DWORD base;
template<class T> T Native(DWORD rva) { return reinterpret_cast<T>(base + rva); }
typedef int (__thiscall* Method)(void*);
typedef void (__thiscall* SceneMethod)(void*, void*, void*);
typedef void (__thiscall* FlipMethod)(void*);
typedef int (__thiscall* SpriteMethod)(void*, int, int, int, int);
typedef int (__thiscall* BitmapMethod)(void*, int, int, void*, int, int, int, int, int);
typedef int (__thiscall* BlitMethod)(void*, int, int, int, int, void*, int, int, void*, int);
typedef int* (__thiscall* SizeMethod)(void*, int*, int*);
typedef int (__thiscall* MouseMethod)(void*, int*, int*, int*);
typedef void (__thiscall* SurfaceSpriteMethod)(void*, int, int, int, int, LPDIRECTDRAWSURFACE, int, int, int);
typedef int (__cdecl* DrawCompare)(const void*, const void*);

bool installed;
// Native code calls back through patched addresses. Publish transient state
// explicitly, including when the DLL is built with MSVC /GL and /LTCG.
volatile bool frameActive, uiRaster, renderBusy, skipPresent;
volatile unsigned directUIDepth;
int uiStep = 10; // tenths; 1.0 is the native UI size in client pixels
double uiCurrentStep = 10.0;
DWORD uiScaleLastTick;
bool uiScaleClockStarted;
LPDIRECTDRAWSURFACE worldSurface;
LPDIRECTDRAW worldOwner;
DWORD worldWidth, worldHeight;
void* frameDisplay;
void* frameGlobal;
int surfaceWidth, surfaceHeight;
double worldX = 1, worldY = 1, uiX = 1, uiY = 1;
ZoomMath::MouseRemainder mouseRemainder;
int mouseContext;
DWORD pendingWidth, pendingHeight;
int pendingUIStep;
unsigned suppressedRedraws;
unsigned suppressedFlips;
void** sceneStorage;
unsigned sceneCapacity;

// Sample UI AND world zoom changes, including the redraws caused by resizing.
// Log only render metadata, never chat text or other user data.
struct ZoomDiagnostic {
    volatile bool recording, queued;
    unsigned sample, remaining;
    int lastStep;
    DWORD lastWidth, lastHeight;
    unsigned scenes, fallback, world, hud, types[11];
    unsigned bitmaps, sprites, blits, scaled, wrongTarget, wrongFormat, wrongFlags;
    unsigned surfaceSprites, surfaceScaled, details;
    int frameStatus;
} diagnostic = {};

void DiagnosticLine(const char* format, ...) {
    char path[MAX_PATH] = {}, line[1024];
    //std::strncpy(path, Config, sizeof(path) - 1);
    char* filename = std::strrchr(path, '\\');
    if (!filename) filename = std::strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    const char logName[] = "wkReSolution-zoom.log";
    if (sizeof(logName) > sizeof(path) - (filename - path)) return;
    std::memcpy(filename, logName, sizeof(logName));
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line) - 3, format, args);
    va_end(args);
    if (length < 0) return;
    unsigned size = static_cast<unsigned>(length);
    if (size > sizeof(line) - 4) size = sizeof(line) - 4;
    line[size++] = '\r'; line[size++] = '\n';
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD folderLength = GetTempPathA(sizeof(path), path);
        if (folderLength && folderLength + sizeof(logName) <= sizeof(path)) {
            std::memcpy(path + folderLength, logName, sizeof(logName));
            file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        OutputDebugStringA("wkReSolution: could not open wkReSolution-zoom.log\n");
        return;
    }
    DWORD written;
    WriteFile(file, line, size, &written, NULL);
    CloseHandle(file);
}

void DiagnosticHooks();

void BeginDiagnostic() {
    if (diagnostic.lastStep != uiStep || diagnostic.lastWidth != TWidth ||
        diagnostic.lastHeight != THeight || !diagnostic.sample) {
        diagnostic.lastStep = uiStep;
        diagnostic.lastWidth = TWidth; diagnostic.lastHeight = THeight;
        diagnostic.remaining = 2;
    }
    const unsigned sample = diagnostic.sample, remaining = diagnostic.remaining;
    const int step = diagnostic.lastStep;
    const DWORD width = diagnostic.lastWidth, height = diagnostic.lastHeight;
    diagnostic = ZoomDiagnostic();
    diagnostic.sample = sample; diagnostic.remaining = remaining; diagnostic.lastStep = step;
    diagnostic.lastWidth = width; diagnostic.lastHeight = height;
    if (remaining && sample < 128) {
        diagnostic.recording = true; diagnostic.remaining--; diagnostic.sample++;
        DiagnosticLine("FRAME %u begin: ui=%d%% world=%lux%lu display=%p global=%p",
                       diagnostic.sample, uiStep * 10, TWidth, THeight, frameDisplay, frameGlobal);
    }
}

void EndDiagnostic() {
    if (!diagnostic.recording) return;
    DiagnosticLine("frame_status=%d screen=%dx%d world_scale=%.4f,%.4f ui_scale=%.4f,%.4f",
                   diagnostic.frameStatus, surfaceWidth, surfaceHeight, worldX, worldY, uiX, uiY);
    DiagnosticLine("scene_calls=%u fallback=%u world_commands=%u hud_commands=%u types[0..10]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                   diagnostic.scenes, diagnostic.fallback, diagnostic.world, diagnostic.hud,
                   diagnostic.types[0], diagnostic.types[1], diagnostic.types[2], diagnostic.types[3],
                   diagnostic.types[4], diagnostic.types[5], diagnostic.types[6], diagnostic.types[7],
                   diagnostic.types[8], diagnostic.types[9], diagnostic.types[10]);
    DiagnosticLine("queued_bitmap_requests=%u queued_sprite_requests=%u raster_blits=%u scaled=%u wrong_target=%u wrong_format=%u wrong_flags=%u surface_sprites=%u surface_scaled=%u",
                   diagnostic.bitmaps, diagnostic.sprites, diagnostic.blits, diagnostic.scaled,
                   diagnostic.wrongTarget, diagnostic.wrongFormat, diagnostic.wrongFlags,
                   diagnostic.surfaceSprites, diagnostic.surfaceScaled);
    DiagnosticHooks();
    DiagnosticLine("redraws_suppressed=%u flips_suppressed=%u", suppressedRedraws, suppressedFlips);
    DiagnosticLine("FRAME %u end", diagnostic.sample);
    diagnostic.recording = false;
}

void Trace(const char* message) { OutputDebugStringA(message); }

struct BitmapImage {
    void* vtable;
    int ownsBuffer;
    BYTE* pixels;
    int bitDepth, pitch, width, height;
    ZoomMath::Rect clip;
};
BitmapImage* volatile chatBitmap;
int chatTextWidth;
static_assert(sizeof(void*) == 4, "This module requires a Win32 build");
static_assert(sizeof(BitmapImage) == 0x2C, "Worms 2 BitmapImage ABI");

ZoomMath::Image Image(BitmapImage* b) {
    ZoomMath::Image i = {b->pixels, b->width, b->height, b->pitch, b->clip};
    return i;
}
ZoomMath::Image Image(const DDSURFACEDESC& d) {
    ZoomMath::Image i = {static_cast<BYTE*>(d.lpSurface), static_cast<int>(d.dwWidth),
        static_cast<int>(d.dwHeight), static_cast<int>(d.lPitch),
        {0, 0, static_cast<int>(d.dwWidth), static_cast<int>(d.dwHeight)}};
    return i;
}

bool UnlockDisplay(void* display) {
    if (!Field<int>(display, 0x444)) return true;
    LPDIRECTDRAWSURFACE back = Field<LPDIRECTDRAWSURFACE>(display, 0x38);
    if (FAILED(back->Unlock(NULL))) return false;
    Field<int>(display, 0x444) = 0;
    return true;
}

void RestoreDisplaySurfaces(void* display) {
    if (!display) return;
    LPDIRECTDRAWSURFACE primary = Field<LPDIRECTDRAWSURFACE>(display, 0x34);
    LPDIRECTDRAWSURFACE back = Field<LPDIRECTDRAWSURFACE>(display, 0x38);
    if (primary) primary->Restore();
    if (back && back != primary) back->Restore();
}

void ReleaseWorld() {
    if (worldSurface) worldSurface->Release();
    if (worldOwner) worldOwner->Release();
    worldSurface = NULL; worldOwner = NULL;
    worldWidth = worldHeight = 0;
}

bool EnsureWorld(LPDIRECTDRAW dd, const DDSURFACEDESC& back) {
    if (worldSurface && (worldOwner != dd || worldWidth != TWidth || worldHeight != THeight))
        ReleaseWorld();
    if (worldSurface) {
        if (worldSurface->IsLost() == DDERR_SURFACELOST && FAILED(worldSurface->Restore()))
            ReleaseWorld();
    }
    if (!worldSurface) {
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.dwWidth = TWidth; d.dwHeight = THeight;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.ddpfPixelFormat = back.ddpfPixelFormat;
        if (FAILED(dd->CreateSurface(&d, &worldSurface, NULL))) return false;
        worldOwner = dd; dd->AddRef();
        worldWidth = TWidth; worldHeight = THeight;
    }
    LPDIRECTDRAWPALETTE palette = NULL;
    if (SUCCEEDED(Field<LPDIRECTDRAWSURFACE>(frameDisplay, 0x38)->GetPalette(&palette))) {
        worldSurface->SetPalette(palette);
        palette->Release();
    }
    return true;
}

bool ClearSurface(LPDIRECTDRAWSURFACE surface) {
    DDBLTFX fx = {}; fx.dwSize = sizeof(fx);
    if (SUCCEEDED(surface->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx)))
        return true;
    DDSURFACEDESC d = {}; d.dwSize = sizeof(d);
    if (FAILED(surface->Lock(NULL, &d, DDLOCK_WAIT, NULL))) return false;
    if (d.ddpfPixelFormat.dwRGBBitCount == 8) {
        for (DWORD y = 0; y < d.dwHeight; ++y)
            std::memset(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch, 0, d.dwWidth);
    }
    surface->Unlock(NULL);
    return d.ddpfPixelFormat.dwRGBBitCount == 8;
}

bool Composite(LPDIRECTDRAWSURFACE back, int chatRows) {
    const int top = ZoomMath::Clamp(ZoomMath::Round(chatRows * worldY), 0, surfaceHeight);
    if (top == surfaceHeight) return true;
    RECT src = {0, chatRows, static_cast<LONG>(TWidth), static_cast<LONG>(THeight)};
    RECT dst = {0, top, surfaceWidth, surfaceHeight};
    if (SUCCEEDED(back->Blt(&dst, worldSurface, &src, DDBLT_WAIT, NULL))) return true;
    // Wrappers/drivers which reject paletted stretching still have a CPU path.
    DDSURFACEDESC a = {}, b = {}; a.dwSize = sizeof(a); b.dwSize = sizeof(b);
    if (FAILED(worldSurface->Lock(NULL, &a, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return false;
    if (FAILED(back->Lock(NULL, &b, DDLOCK_WAIT, NULL))) {
        worldSurface->Unlock(NULL); return false;
    }
    ZoomMath::Image dest = Image(b); dest.clip.top = top;
    const bool result = ZoomMath::Blit(dest, Image(a), 0, 0, TWidth, THeight,
                                     0, 0, worldX, worldY, 0, 0, 0);
    back->Unlock(NULL); worldSurface->Unlock(NULL);
    return result;
}

// Layout callbacks emit positions relative to the viewport centre. Give only
// these callbacks UI dimensions; the camera and simulation retain world units.
class LayoutScope {
    void* global;
    int width, height;
public:
    explicit LayoutScope(void* g) : global(frameActive && g == frameGlobal ? g : NULL), width(0), height(0) {
        if (!global) return;
        width = Field<int>(global, 0x3114); height = Field<int>(global, 0x3118);
        Field<int>(global, 0x3114) = ZoomMath::LogicalExtent(width, worldX, uiX);
        Field<int>(global, 0x3118) = ZoomMath::LogicalExtent(height, worldY, uiY);
        _ReadWriteBarrier();
    }
    ~LayoutScope() {
        if (!global) return;
        _ReadWriteBarrier();
        Field<int>(global, 0x3114) = width; Field<int>(global, 0x3118) = height;
        _ReadWriteBarrier();
    }
};

template<DWORD rva, unsigned globalOffset>
int __fastcall LayoutHook(void* self, void*) {
    LayoutScope scope(Field<void*>(self, globalOffset));
    // Escape menus draw directly; task HUD callbacks queue commands.
    const bool direct = frameActive && globalOffset == 0x5C9C;
    if (direct) ++directUIDepth;
    const int result = Native<Method>(rva)(self);
    if (direct) --directUIDepth;
    return result;
}

// UI calls still use native sprite/frame decoding. Only their final blit is
// scaled. The temporary viewport is in physical back-buffer coordinates.
class RasterScope {
    void* display;
    int saved[8];
    bool previous;
public:
    explicit RasterScope(void* d) : display(frameActive && d == frameDisplay ? d : NULL), previous(uiRaster) {
        if (!display) return;
        std::memcpy(saved, static_cast<BYTE*>(d) + 8, sizeof(saved));
        Field<int>(d, 8) = surfaceWidth; Field<int>(d, 12) = surfaceHeight;
        for (unsigned n = 0; n < 6; ++n) {
            const double scale = n % 2 ? worldY : worldX;
            Field<int>(d, 0x10 + n * 4) = ZoomMath::Round(saved[n + 2] * scale);
        }
        SyncClip();
        uiRaster = true;
        _ReadWriteBarrier();
    }
    void SyncClip() {
        BitmapImage* bitmap = Field<BitmapImage*>(display, 0x448);
        if (bitmap) std::memcpy(&bitmap->clip, static_cast<BYTE*>(display) + 0x10, sizeof(bitmap->clip));
    }
    ~RasterScope() {
        if (!display) return;
        _ReadWriteBarrier();
        uiRaster = previous;
        std::memcpy(static_cast<BYTE*>(display) + 8, saved, sizeof(saved));
        SyncClip();
        _ReadWriteBarrier();
    }
};

int DrawUISprite(void* display, int x, int y, int id, int frame) {
    if (diagnostic.recording && diagnostic.queued) ++diagnostic.sprites;
    RasterScope scope(display);
    return Native<SpriteMethod>(0xCA60)(display, x, y, id, frame);
}

int DrawUIBitmap(void* display, int x, int y, void* bitmap,
                 int left, int top, int right, int bottom, int flags) {
    if (diagnostic.recording && diagnostic.queued) {
        ++diagnostic.bitmaps;
        if (diagnostic.details++ < 4)
            DiagnosticLine("queued_bitmap: xy=%d,%d source=%dx%d bits=%d flags=%08x display=%p",
                           x / 65536, y / 65536, right - left, bottom - top,
                           static_cast<BitmapImage*>(bitmap)->bitDepth, flags, display);
    }
    RasterScope scope(display);
    return Native<BitmapMethod>(0xCF50)(display, x, y, bitmap, left, top, right, bottom, flags);
}

int DrawChat(void* display, BitmapImage* bitmap) {
    // The native chat bitmap is allocated at the original viewport width.
    // Scaling it around the viewport centre pushes its left edge offscreen.
    // Keep the message column at the left and player columns at the right;
    // clip messages to the space available without altering their backing store.
    if (!bitmap || bitmap->bitDepth != 8 || !UnlockDisplay(display)) return 0;
    LPDIRECTDRAWSURFACE back = Field<LPDIRECTDRAWSURFACE>(display, 0x38);
    DDSURFACEDESC d = {}; d.dwSize = sizeof(d);
    if (FAILED(back->Lock(NULL, &d, DDLOCK_WAIT, NULL))) return 0;
    ZoomMath::Rect clip;
    clip.left = ZoomMath::Round(Field<int>(display, 0x10) * worldX);
    clip.top = ZoomMath::Round(Field<int>(display, 0x14) * worldY);
    clip.right = ZoomMath::Round(Field<int>(display, 0x18) * worldX);
    clip.bottom = ZoomMath::Round(Field<int>(display, 0x1C) * worldY);
    ZoomMath::Image target = Image(d); target.clip = clip;
    const ZoomMath::Image source = Image(bitmap);
    const int textWidth = ZoomMath::Clamp(chatTextWidth, 0, source.width);
    const int playersWidth = source.width - textWidth;
    target.clip.right = ZoomMath::Clamp(ZoomMath::Edge(clip.right - playersWidth * uiX), clip.left, clip.right);
    ZoomMath::Blit(target, source, clip.left, clip.bottom - source.height,
                   textWidth, source.height, 0, 0, uiX, uiY, clip.left, clip.bottom, 0);
    target.clip = clip;
    if (playersWidth)
        ZoomMath::Blit(target, source, clip.right - playersWidth, clip.bottom - source.height,
                       playersWidth, source.height, textWidth, 0, uiX, uiY, clip.right, clip.bottom, 0);
    // Reconnect the native border when zooming out leaves spare message space.
    BYTE border = bitmap->pixels[0];
    ZoomMath::Image pixel = {&border, 1, 1, 1, {0, 0, 1, 1}};
    for (int row = 0; row < 2; ++row)
        ZoomMath::Blit(target, pixel, clip.left, clip.bottom - source.height + (row ? source.height - 2 : 0),
                       1, 1, 0, 0, clip.right - clip.left, uiY, clip.left, clip.bottom, 0);
    target.clip.right = ZoomMath::Clamp(ZoomMath::Edge(clip.right - playersWidth * uiX), clip.left, clip.right);
    ZoomMath::Blit(target, pixel, clip.left, clip.bottom - 15, 1, 1, 0, 0,
                   clip.right - clip.left, uiY, clip.left, clip.bottom, 0);
    target.clip = clip;
    for (int side = 0; side < 2; ++side) {
        const int anchor = side ? clip.right : clip.left;
        ZoomMath::Blit(target, pixel, anchor - side, clip.bottom - 1, 1, 1, 0, 0,
                       uiX, source.height * uiY, anchor, clip.bottom, 0);
    }
    back->Unlock(NULL);
    return 1;
}

int __fastcall ChatHook(void* game, void*) {
    if (!frameActive || Field<void*>(game, 0x5C9C) != frameGlobal)
        return Native<Method>(0x46360)(game);
    BitmapImage* bitmap = Field<BitmapImage*>(game, 0x10);
    if (!bitmap || bitmap->bitDepth != 8) return Native<Method>(0x46360)(game);
    // Native chat borders use the bitmap's own coordinates. Its final draw
    // below applies the UI scale; world viewport dimensions are unrelated.
    const int savedWidth = Field<int>(frameGlobal, 0x3114);
    Field<int>(frameGlobal, 0x3114) = bitmap->width;
    chatTextWidth = Field<int>(game, 0x168);
    chatBitmap = bitmap;
    _ReadWriteBarrier();
    const int result = Native<Method>(0x46360)(game);
    _ReadWriteBarrier();
    chatBitmap = NULL;
    Field<int>(frameGlobal, 0x3114) = savedWidth;
    return result;
}

int __fastcall SpriteHook(void* display, void*, int x, int y, int id, int frame) {
    const DWORD caller = reinterpret_cast<DWORD>(_ReturnAddress()) - base;
    if (!frameActive || display != frameDisplay || (!directUIDepth && caller != 0x46952))
        return Native<SpriteMethod>(0xCA60)(display, x, y, id, frame);
    if (caller == 0x46952) {
        // Network indicator is laid out inline in Game::render.
        const int w = Field<int>(frameGlobal, 0x3114), h = Field<int>(frameGlobal, 0x3118);
        x += (w / 2 - ZoomMath::LogicalExtent(w, worldX, uiX) / 2) * 65536;
        y += (h / 2 - ZoomMath::LogicalExtent(h, worldY, uiY) / 2) * 65536;
    }
    return DrawUISprite(display, x, y, id, frame);
}

int __fastcall BitmapHook(void* display, void*, int x, int y, void* bitmap,
                          int left, int top, int right, int bottom, int flags) {
    if (frameActive && display == frameDisplay && bitmap == chatBitmap)
        return DrawChat(display, static_cast<BitmapImage*>(bitmap));
    const DWORD caller = reinterpret_cast<DWORD>(_ReturnAddress()) - base;
    // 0x46A42 is the inline network-delay warning. Worm nameplates reach the
    // same vtable method from GameScene::render and must always pass through.
    if (!frameActive || display != frameDisplay || (!directUIDepth && caller != 0x46A42))
        return Native<BitmapMethod>(0xCF50)(display, x, y, bitmap, left, top, right, bottom, flags);
    return DrawUIBitmap(display, x, y, bitmap, left, top, right, bottom, flags);
}

void __fastcall SurfaceSpriteHook(void* display, void*, int x, int y, int w, int h,
                                 LPDIRECTDRAWSURFACE source, int sx, int sy, int flags) {
    if (diagnostic.recording && diagnostic.queued) ++diagnostic.surfaceSprites;
    if (!uiRaster || display != frameDisplay) {
        Native<SurfaceSpriteMethod>(0xC770)(display, x, y, w, h, source, sx, sy, flags);
        return;
    }
    // Surface-backed sprites (including menu cursors) bypass BitmapImage::blit.
    // Use the exact same transform, clip and colour-key semantics as UI bitmaps.
    if (w <= 0 || h <= 0 || !UnlockDisplay(display)) return;
    LPDIRECTDRAWSURFACE back = Field<LPDIRECTDRAWSURFACE>(display, 0x38);
    DDSURFACEDESC src = {}, dst = {}; src.dwSize = sizeof(src); dst.dwSize = sizeof(dst);
    if (FAILED(source->Lock(NULL, &src, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return;
    if (FAILED(back->Lock(NULL, &dst, DDLOCK_WAIT, NULL))) { source->Unlock(NULL); return; }
    if (src.ddpfPixelFormat.dwRGBBitCount == 8 && dst.ddpfPixelFormat.dwRGBBitCount == 8) {
        if (diagnostic.recording && diagnostic.queued) ++diagnostic.surfaceScaled;
        ZoomMath::Image target = Image(dst);
        std::memcpy(&target.clip, static_cast<BYTE*>(display) + 0x10, sizeof(target.clip));
        ZoomMath::Blit(target, Image(src), x, y, w, h, sx, sy, uiX, uiY,
                       Field<int>(display, 0x20), Field<int>(display, 0x24),
                       (flags & DDBLTFAST_SRCCOLORKEY) ? 1 : 0);
    }
    back->Unlock(NULL); source->Unlock(NULL);
}

int __fastcall BlitHook(void* dest, void*, int x, int y, int w, int h, void* source,
                       int sx, int sy, void* remap, int flags) {
    BitmapImage* a = static_cast<BitmapImage*>(dest);
    BitmapImage* b = static_cast<BitmapImage*>(source);
    if (diagnostic.recording && diagnostic.queued) {
        ++diagnostic.blits;
        if (dest != Field<void*>(frameDisplay, 0x448)) ++diagnostic.wrongTarget;
        if (a->bitDepth != 8 || b->bitDepth != 8) ++diagnostic.wrongFormat;
        if ((flags & 0xFFFF) > 3) ++diagnostic.wrongFlags;
        if (diagnostic.blits <= 3)
            DiagnosticLine("queued_blit: raster=%d target=%p expected=%p bits=%d,%d source=%dx%d output=%.2fx%.2f flags=%08x",
                           static_cast<int>(uiRaster), dest, Field<void*>(frameDisplay, 0x448),
                           a->bitDepth, b->bitDepth, w, h, w * uiX, h * uiY, flags);
    }
    if (uiRaster && dest == Field<void*>(frameDisplay, 0x448) &&
        a->bitDepth == 8 && b->bitDepth == 8 && (flags & 0xFFFF) <= 3) {
        if (diagnostic.recording && diagnostic.queued) ++diagnostic.scaled;
        return ZoomMath::Blit(Image(a), Image(b), x, y, w, h, sx, sy, uiX, uiY,
                             Field<int>(frameDisplay, 0x20), Field<int>(frameDisplay, 0x24),
                             flags, static_cast<BYTE*>(remap)) ? 1 : 0;
    }
    return Native<BlitMethod>(0x3EF30)(dest, x, y, w, h, source, sx, sy, remap, flags);
}

int* __fastcall SizeHook(void* bitmap, void*, int* w, int* h) {
    const DWORD caller = reinterpret_cast<DWORD>(_ReturnAddress()) - base;
    int* result = Native<SizeMethod>(0x3E8D0)(bitmap, w, h);
    if (frameActive && caller == 0x46661)
        *h = ZoomMath::Clamp(ZoomMath::Round(*h * uiY / worldY), 1, static_cast<int>(TargetHeight) - 1);
    return result;
}

int __fastcall MouseHook(void* mouse, void*, int* dx, int* dy, int* buttons) {
    const DWORD caller = reinterpret_cast<DWORD>(_ReturnAddress()) - base;
    const int result = Native<MouseMethod>(0x342C0)(mouse, dx, dy, buttons);
    const int context = caller >= 0x124B0 && caller < 0x12740 ? 1 :
                        caller >= 0x47A40 && caller < 0x47BD0 ? 2 :
                        caller >= 0x47DD0 && caller < 0x48080 ? 3 : 0;
    if (context != mouseContext) { mouseRemainder = ZoomMath::MouseRemainder(); mouseContext = context; }
    if (context) mouseRemainder.Apply(*dx, *dy, uiCurrentStep / 10.0);
    return result;
}

bool SceneQueueLayout(DWORD& countOffset, DWORD& callsOffset) {
    const BYTE* code = Native<const BYTE*>(0xF330);
    // Some game patches enlarge the command arena. Read the live operands,
    // checking both qsort's input and the subsequent rendering loop agree.
    if (std::memcmp(code + 0x05, "\x8B\x81", 2) ||
        std::memcmp(code + 0x17, "\x8D\x8E", 2) ||
        std::memcmp(code + 0x26, "\x8B\x86", 2) ||
        std::memcmp(code + 0x37, "\x8D\x8C\x86", 3)) return false;
    DWORD loopCountOffset, loopCallsOffset;
    std::memcpy(&countOffset, code + 0x07, 4);
    std::memcpy(&callsOffset, code + 0x19, 4);
    std::memcpy(&loopCountOffset, code + 0x28, 4);
    std::memcpy(&loopCallsOffset, code + 0x3A, 4);
    return countOffset == loopCountOffset && callsOffset == loopCallsOffset;
}

bool ReserveScene(unsigned count) {
    if (count < 1) count = 1;
    if (count <= sceneCapacity) return true;
    // Retain the scratch allocation across frames; growing queues must not
    // switch back to combined rendering at the old 1,024-command limit.
    void* memory = std::realloc(sceneStorage, count * 2 * sizeof(void*));
    if (!memory) return false;
    sceneStorage = static_cast<void**>(memory); sceneCapacity = count;
    return true;
}

void __fastcall SceneHook(void* scene, void*, void* display, void* camera) {
    const SceneMethod render = Native<SceneMethod>(0xF330);
    if (diagnostic.recording) ++diagnostic.scenes;
    if (!frameActive || display != frameDisplay) {
        if (diagnostic.recording) {
            diagnostic.fallback |= 1;
            DiagnosticLine("scene_bypass: active=%d display=%p expected=%p", static_cast<int>(frameActive), display, frameDisplay);
        }
        render(scene, display, camera); return;
    }
    DWORD countOffset = 0, callsOffset = 0;
    if (!SceneQueueLayout(countOffset, callsOffset)) {
        if (diagnostic.recording) {
            diagnostic.fallback |= 8;
            DiagnosticLine("scene_bypass: unrecognised live queue instructions");
        }
        render(scene, display, camera); return;
    }
    volatile int& count = Field<int>(scene, countOffset);
    void** calls = &Field<void*>(scene, callsOffset);
    if (diagnostic.recording)
        DiagnosticLine("scene_layout: scene=%p count_offset=0x%lX calls_offset=0x%lX count=%d",
                       scene, countOffset, callsOffset, static_cast<int>(count));
    // Sanity-check corrupt metadata, without imposing the original capacity
    // on a game whose renderer has been patched to support a larger arena.
    if (count < 0 || count > 65536) {
        if (diagnostic.recording) {
            diagnostic.fallback |= 2; DiagnosticLine("scene_bypass: count=%d", static_cast<int>(count));
        }
        render(scene, display, camera); return;
    }
    const int savedCount = count;
    if (!ReserveScene(savedCount)) {
        if (diagnostic.recording) {
            diagnostic.fallback |= 16;
            DiagnosticLine("scene_bypass: queue scratch allocation failed");
        }
        render(scene, display, camera); return;
    }
    void** saved = sceneStorage;
    void** ui = sceneStorage + sceneCapacity;
    std::memcpy(saved, calls, savedCount * sizeof(void*));
    int nWorld = 0, nUI = 0;
    for (int i = 0; i < savedCount; ++i) {
        const int type = Field<int>(saved[i], 0);
        if (diagnostic.recording) ++diagnostic.types[type >= 0 && type < 10 ? type : 10];
        // In the verified executable, bitmap2d/sprite2d are HUD commands.
        // Worms, nameplates and aiming graphics are bitmap3d/sprite3d. Route
        // by the native command type, independently of callback bookkeeping.
        if (type == 1 || type == 3) ui[nUI++] = saved[i];
        else calls[nWorld++] = saved[i];
    }
    LPDIRECTDRAWSURFACE& back = Field<LPDIRECTDRAWSURFACE>(display, 0x38);
    LPDIRECTDRAWSURFACE realBack = back;
    const int chatRows = Field<int>(display, 0x14);
    if (!UnlockDisplay(display)) {
        if (diagnostic.recording) diagnostic.fallback |= 4;
        std::memcpy(calls, saved, savedCount * sizeof(void*));
        render(scene, display, camera); return;
    }
    back = worldSurface; count = nWorld; uiRaster = false;
    _ReadWriteBarrier();
    render(scene, display, camera);
    _ReadWriteBarrier();
    UnlockDisplay(display);
    back = realBack;
    _ReadWriteBarrier();
    if (!Composite(realBack, chatRows)) Trace("wkReSolution: world composite failed\n");
    if (diagnostic.recording) { diagnostic.world += nWorld; diagnostic.hud += nUI; }
    // Render the native scene exactly once. Submit only the 2D HUD commands
    // here; replaying GameScene::render can replay world primitives as well.
    std::qsort(ui, nUI, sizeof(void*), Native<DrawCompare>(0x10100));
    diagnostic.queued = true;
    for (int i = nUI - 1; i >= 0; --i) {
        void* command = ui[i];
        if (Field<int>(command, 0) == 1)
            DrawUIBitmap(display, Field<int>(command, 8), Field<int>(command, 12),
                         Field<void*>(command, 16), Field<int>(command, 20), Field<int>(command, 24),
                         Field<int>(command, 28), Field<int>(command, 32), Field<int>(command, 36));
        else
            DrawUISprite(display, Field<int>(command, 8), Field<int>(command, 12),
                         Field<int>(command, 16), Field<int>(command, 20));
    }
    diagnostic.queued = false;
    std::memcpy(calls, saved, savedCount * sizeof(void*)); count = savedCount;
    _ReadWriteBarrier();
}

void ApplyPendingZoom() {
    if (pendingWidth) {
        const DWORD width = pendingWidth, height = pendingHeight;
        pendingWidth = pendingHeight = 0;
        TWidth = LastWidth = width; THeight = LastHeight = height;
        PatchW2Mem(width, height, true);
    }
    if (pendingUIStep) {
        uiStep = pendingUIStep; pendingUIStep = 0;
        mouseRemainder = ZoomMath::MouseRemainder();
    }
}

void AdvanceUIScale() {
    const DWORD now = GetTickCount();
    if (!uiScaleClockStarted) {
        uiScaleLastTick = now;
        uiScaleClockStarted = true;
        return;
    }
    const DWORD elapsed = now - uiScaleLastTick;
    uiScaleLastTick = now;
    uiCurrentStep = ZoomMath::EaseToward(uiCurrentStep, uiStep, elapsed);
}

int __fastcall FrameHook(void* game, void*) {
    const Method render = Native<Method>(0x46630);
    // Progressive resizing may request a render from inside a window message.
    if (frameActive) return 0;
    ApplyPendingZoom();
    // The native renderer deliberately preserves the last frame while video
    // output is suspended (for example during a display transition).
    if (*Native<int*>(0x79460)) return render(game);
    frameGlobal = Field<void*>(game, 0x5C9C);
    frameDisplay = Field<void*>(frameGlobal, 4);
    BeginDiagnostic();
    LPDIRECTDRAWSURFACE back = Field<LPDIRECTDRAWSURFACE>(frameDisplay, 0x38);
    LPDIRECTDRAW dd = Field<LPDIRECTDRAW>(frameDisplay, 0x30);
    DDSURFACEDESC d = {}; d.dwSize = sizeof(d);
    if (!back || !dd || !TWidth || !THeight || FAILED(back->GetSurfaceDesc(&d)) ||
        d.ddpfPixelFormat.dwRGBBitCount != 8 || !d.dwWidth || !d.dwHeight)
    {
        diagnostic.frameStatus = 1;
        // A focus transition can invalidate the wrapper's primary/back
        // surfaces between RenderGame's lost-surface check and this hook.
        // Do not render natively here: that would display one combined-scale
        // frame. Let the next RenderGame call perform the normal recovery.
        RestoreDisplaySurfaces(frameDisplay);
        skipPresent = true;
        EndDiagnostic();
        return 0;
    }
    if (!UnlockDisplay(frameDisplay) || !EnsureWorld(dd, d) ||
        !ClearSurface(worldSurface) || !ClearSurface(back)) {
        // Keep the game usable if the driver cannot supply the extra surface.
        // Suppress this frame instead of drawing with the native combined
        // renderer. The latter is visible as a one-frame whole-screen zoom
        // after Alt+Tab when the wrapper is restoring its surfaces.
        RestoreDisplaySurfaces(frameDisplay);
        skipPresent = true;
        diagnostic.frameStatus = 2; EndDiagnostic();
        return 0;
    }
    surfaceWidth = d.dwWidth; surfaceHeight = d.dwHeight;
    // cnc-ddraw's [worms2] vhack reads W2DS->RenderWidth/Height from a
    // separate render thread. Independent zoom already composites the world
    // into this physical back buffer, so leaving low world dimensions there
    // would make the wrapper stretch the completed frame a second time.
    SyncPresentationSize(surfaceWidth, surfaceHeight);
    AdvanceUIScale();
    DWORD clientW = d.dwWidth, clientH = d.dwHeight;
    GetWndSize(WormsWnd(), clientW, clientH);
    if (!clientW || !clientH) { clientW = d.dwWidth; clientH = d.dwHeight; }
    worldX = double(d.dwWidth) / TWidth; worldY = double(d.dwHeight) / THeight;
    uiX = (uiCurrentStep / 10.0) * d.dwWidth / clientW;
    uiY = (uiCurrentStep / 10.0) * d.dwHeight / clientH;
    Field<int>(frameDisplay, 8) = TWidth; Field<int>(frameDisplay, 12) = THeight;
    chatBitmap = NULL; directUIDepth = 0; uiRaster = false;
    frameActive = true;
    _ReadWriteBarrier();
    const int result = render(game);
    _ReadWriteBarrier();
    frameActive = false;
    // DD_Game::render calls Flip after this hook returns. Its source rectangle
    // must cover the completed physical buffer, even if an earlier resize
    // left virtual world dimensions in DD_Display.
    Field<int>(frameDisplay, 8) = surfaceWidth; Field<int>(frameDisplay, 12) = surfaceHeight;
    EndDiagnostic();
    return result;
}

int __fastcall PresentHook(void* game, void*) {
    // Guard DD_Game::render as a whole, including preparation and the final
    // flip. Returning early from FrameHook alone still lets its caller flip
    // a partially rendered frame using the temporary world dimensions.
    if (renderBusy || frameActive) { ++suppressedRedraws; return 0; }
    skipPresent = false;
    renderBusy = true;
    _ReadWriteBarrier();
    const int result = Native<Method>(0x34830)(game);
    _ReadWriteBarrier();
    renderBusy = false;
    return result;
}

void __fastcall FlipHook(void* display, void*) {
    if (skipPresent && display == frameDisplay) {
        skipPresent = false;
        ++suppressedFlips;
        return;
    }
    Native<FlipMethod>(0xC580)(display);
}

struct Patch {
    DWORD rva, original;
    const void* hook;
    bool call;
    BYTE saved[5], replacement[5];
    unsigned Size() const { return call ? 5 : 4; }
};
#define CALL(site, target, hook) {site, target, reinterpret_cast<const void*>(hook), true, {}, {}}
#define SLOT(site, target, hook) {site, target, reinterpret_cast<const void*>(hook), false, {}, {}}
// Each record contains an original destination, not merely an opcode check.
Patch patches[] = {
    CALL(0x348B1, 0x46630, FrameHook),
    SLOT(0x6B9B8, 0x34830, PresentHook),
    SLOT(0x6B32C, 0xC580, FlipHook),
    CALL(0x468F0, 0xF330, SceneHook),
    CALL(0x7F34, 0x7690, (LayoutHook<0x7690, 0x74>)),
    CALL(0x7F42, 0x9330, (LayoutHook<0x9330, 0x74>)),
    CALL(0x28715, 0x27E70, (LayoutHook<0x27E70, 0x74>)),
    CALL(0x2870E, 0x28220, (LayoutHook<0x28220, 0x74>)),
    CALL(0x28707, 0x28340, (LayoutHook<0x28340, 0x74>)),
    CALL(0x39CF2, 0x3A500, (LayoutHook<0x3A500, 0x74>)),
    CALL(0x5B2AA, 0x5B560, (LayoutHook<0x5B560, 0x74>)),
    CALL(0x466C6, 0x46360, ChatHook),
    CALL(0x46A44, 0x472A0, (LayoutHook<0x472A0, 0x5C9C>)),
    SLOT(0x6B310, 0xCA60, SpriteHook),
    SLOT(0x6B314, 0xCF50, BitmapHook),
    CALL(0xCF0F, 0xC770, SurfaceSpriteHook),
    CALL(0xCF3E, 0xC770, SurfaceSpriteHook),
    SLOT(0x6B068, 0x3EF30, BlitHook),
    SLOT(0x6B558, 0x3EF30, BlitHook),
    SLOT(0x6BB70, 0x3EF30, BlitHook),
    SLOT(0x6B034, 0x3E8D0, SizeHook),
    SLOT(0x6B524, 0x3E8D0, SizeHook),
    SLOT(0x6BB3C, 0x3E8D0, SizeHook),
    SLOT(0x6B99C, 0x342C0, MouseHook)
};
#undef CALL
#undef SLOT

void DiagnosticHooks() {
    unsigned matches = 0;
    for (const Patch& patch : patches) {
        const BYTE* code = Native<const BYTE*>(patch.rva);
        DWORD target;
        std::memcpy(&target, code + (patch.call ? 1 : 0), sizeof(target));
        if (patch.call) target += base + patch.rva + 5;
        const DWORD expected = reinterpret_cast<DWORD>(patch.hook);
        if ((!patch.call || code[0] == 0xE8) && target == expected) ++matches;
        else DiagnosticLine("hook_mismatch: rva=%08lx opcode=%02x target=%08lx expected=%08lx",
                            patch.rva, code[0], target, expected);
    }
    DiagnosticLine("hooks_matching=%u/%u scene_entry=%02x %02x %02x %02x %02x",
                   matches, static_cast<unsigned>(sizeof(patches) / sizeof(patches[0])),
                   *Native<BYTE*>(0xF330), *Native<BYTE*>(0xF331), *Native<BYTE*>(0xF332),
                   *Native<BYTE*>(0xF333), *Native<BYTE*>(0xF334));
}

bool Write(void* dest, const void* source, unsigned size) {
    DWORD previous;
    if (!VirtualProtect(dest, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(dest, source, size);
    FlushInstructionCache(GetCurrentProcess(), dest, size);
    DWORD unused;
    VirtualProtect(dest, size, previous, &unused);
    return true; // The bytes were written even if restoring page protection failed.
}

void Redraw() {
    if (!renderBusy && !frameActive && RenderGame && DrawingInterface() &&
        *Native<void**>(0x799AC)) RenderGame();
}

} // namespace

bool Install() {
    if (installed) return true;
    if (Version != W2_15_EN || !AllowZoom ||
        !GetPrivateProfileIntA("Zooming", "SeparateUI", 1, Config)) return false;
    base = reinterpret_cast<DWORD>(GetModuleHandleA(NULL));
    // Check every site before making any changes. Other builds/modifications
    // retain the project's legacy zoom rather than receiving guessed offsets.
    for (Patch& p : patches) {
        BYTE expected[5] = {};
        DWORD target = p.call ? p.original - p.rva - 5 : base + p.original;
        if (p.call) expected[0] = 0xE8;
        std::memcpy(expected + (p.call ? 1 : 0), &target, 4);
        if (std::memcmp(Native<void*>(p.rva), expected, p.Size())) {
            Trace("wkReSolution: separate UI disabled; rendering hook signature mismatch\n");
            return false;
        }
        std::memcpy(p.saved, expected, p.Size());
        target = reinterpret_cast<DWORD>(p.hook);
        if (p.call) { p.replacement[0] = 0xE8; target -= base + p.rva + 5; }
        std::memcpy(p.replacement + (p.call ? 1 : 0), &target, 4);
    }
    unsigned done = 0;
    for (Patch& p : patches) {
        if (!Write(Native<void*>(p.rva), p.replacement, p.Size())) {
            while (done) { Patch& undo = patches[--done]; Write(Native<void*>(undo.rva), undo.saved, undo.Size()); }
            Trace("wkReSolution: separate UI hooks could not be installed\n");
            return false;
        }
        ++done;
    }
    installed = true;
    DiagnosticLine("--- wkReSolution independent zoom v6: installed, base=%08lx ---", base);
    Trace("wkReSolution: independent world/UI zoom enabled\n");
    return true;
}

void Remove(bool processTerminating) {
    if (!installed) return;
    for (Patch& p : patches)
        if (!std::memcmp(Native<void*>(p.rva), p.replacement, p.Size()))
            Write(Native<void*>(p.rva), p.saved, p.Size());
    installed = false;
    // During process teardown Windows reclaims these objects. Calling into
    // DirectDraw from DllMain at that point can deadlock on its worker threads.
    if (!processTerminating) {
        ReleaseWorld();
        std::free(sceneStorage); sceneStorage = NULL; sceneCapacity = 0;
    }
}

bool Enabled() { return installed; }

void SyncPresentationSize(DWORD width, DWORD height) {
    if (!installed || !width || !height || !pW2DS || !*pW2DS) return;
    // Use interlocked stores so the wrapper cannot observe a torn dimension.
    // Update width first: any mixed pair fails the wrapper's low-resolution
    // test because at least one dimension is already physical-sized.
    InterlockedExchange(reinterpret_cast<LONG*>(&W2DS->RenderWidth), static_cast<LONG>(width));
    InterlockedExchange(reinterpret_cast<LONG*>(&W2DS->RenderHeight), static_cast<LONG>(height));
    _ReadWriteBarrier();
}

LPDIRECTDRAW DrawingInterface() {
    if (!installed) return NULL;
    void* display = *Native<void**>(0x799B4);
    return display ? Field<LPDIRECTDRAW>(display, 0x30) : NULL;
}

bool ResizeWorld(DWORD width, DWORD height, bool redraw) {
    if (!installed || !width || !height || width > 32767 || height > 32767) return false;
    if (renderBusy || frameActive) {
        pendingWidth = width; pendingHeight = height;
        return true;
    }
    pendingWidth = pendingHeight = 0;
    TWidth = LastWidth = width; THeight = LastHeight = height;
    PatchW2Mem(width, height, true);
    if (redraw || ProgressiveResize) Redraw();
    return true;
}

void ChangeUI(int direction) {
    if (!installed) return;
    const int current = pendingUIStep ? pendingUIStep : uiStep;
    const int next = ZoomMath::Clamp(current + direction, 5, 30);
    if (next == current) return;
    if (renderBusy || frameActive) { pendingUIStep = next; return; }
    pendingUIStep = 0;
    uiStep = next; mouseRemainder = ZoomMath::MouseRemainder();
    Redraw();
}

void ResetUI() {
    if (!installed) return;
    if (renderBusy || frameActive) { pendingUIStep = 10; return; }
    pendingUIStep = 0;
    uiStep = 10; mouseRemainder = ZoomMath::MouseRemainder();
    Redraw();
}

} // namespace SplitZoom

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
void** sceneStorage;
unsigned sceneCapacity;

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
    RasterScope scope(display);
    return Native<SpriteMethod>(0xCA60)(display, x, y, id, frame);
}

int DrawUIBitmap(void* display, int x, int y, void* bitmap,
                 int left, int top, int right, int bottom, int flags) {
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
    if (uiRaster && dest == Field<void*>(frameDisplay, 0x448) &&
        a->bitDepth == 8 && b->bitDepth == 8 && (flags & 0xFFFF) <= 3) {
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
    if (!frameActive || display != frameDisplay) {
        render(scene, display, camera); return;
    }
    DWORD countOffset = 0, callsOffset = 0;
    if (!SceneQueueLayout(countOffset, callsOffset)) {
        render(scene, display, camera); return;
    }
    volatile int& count = Field<int>(scene, countOffset);
    void** calls = &Field<void*>(scene, callsOffset);
    // Sanity-check corrupt metadata, without imposing the original capacity
    // on a game whose renderer has been patched to support a larger arena.
    if (count < 0 || count > 65536) {
        render(scene, display, camera); return;
    }
    const int savedCount = count;
    if (!ReserveScene(savedCount)) {
        render(scene, display, camera); return;
    }
    void** saved = sceneStorage;
    void** ui = sceneStorage + sceneCapacity;
    std::memcpy(saved, calls, savedCount * sizeof(void*));
    int nWorld = 0, nUI = 0;
    for (int i = 0; i < savedCount; ++i) {
        const int type = Field<int>(saved[i], 0);
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
    Composite(realBack, chatRows);
    // Render the native scene exactly once. Submit only the 2D HUD commands
    // here; replaying GameScene::render can replay world primitives as well.
    std::qsort(ui, nUI, sizeof(void*), Native<DrawCompare>(0x10100));
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
    LPDIRECTDRAWSURFACE back = Field<LPDIRECTDRAWSURFACE>(frameDisplay, 0x38);
    LPDIRECTDRAW dd = Field<LPDIRECTDRAW>(frameDisplay, 0x30);
    DDSURFACEDESC d = {}; d.dwSize = sizeof(d);
    if (!back || !dd || !TWidth || !THeight || FAILED(back->GetSurfaceDesc(&d)) ||
        d.ddpfPixelFormat.dwRGBBitCount != 8 || !d.dwWidth || !d.dwHeight)
    {
        // A focus transition can invalidate the wrapper's primary/back
        // surfaces between RenderGame's lost-surface check and this hook.
        // Do not render natively here: that would display one combined-scale
        // frame. Let the next RenderGame call perform the normal recovery.
        RestoreDisplaySurfaces(frameDisplay);
        skipPresent = true;
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
    return result;
}

int __fastcall PresentHook(void* game, void*) {
    // Guard DD_Game::render as a whole, including preparation and the final
    // flip. Returning early from FrameHook alone still lets its caller flip
    // a partially rendered frame using the temporary world dimensions.
    if (renderBusy || frameActive) return 0;
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
            return false;
        }
        ++done;
    }
    installed = true;
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

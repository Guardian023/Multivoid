// ui/overlay_seams.cpp -- the engine seams the overlay draws and resizes through. Install() AOB-resolves
// four engine-private functions and MinHooks them: the two per-RHI present choke points (the draw
// seam, FD3D11Viewport::PresentChecked and FD3D12Viewport::PresentInternal) and the two per-RHI
// viewport Resize functions (the render-target release bracket). The present detours hand each
// frame to imgui_overlay::OverlayFrame; the resize detours bracket the engine's own resize with the
// backend's release and recreate. No DXGI vtable entry is patched, so RivaTuner has nothing of ours
// to unlink and OBS's game capture sees our pixels (docs/ui.md).

#include "overlay_seams.h"

#include "ui/overlay_backend.h"
#include "overlay_backend_internal.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/scan_ledger.h"  // the draw seam and the resize bracket, each found or missed
#include "ue_wrap/core/sig_scan.h"     // FindPattern -- the engine seam probes
#include "ue_wrap/core/sdk_profile.h"  // the four seam signatures

#include <windows.h>
#include <dxgi.h>

#include <cstdint>

namespace ui::overlay_seams {
namespace {

// Read `viewport + off`, SEH-guarded: a drifted offset must not fault the render thread.
IDXGISwapChain* RawViewportSwapChain(void* viewport, size_t off) {
    if (!viewport) return nullptr;
    __try {
        return *reinterpret_cast<IDXGISwapChain**>(reinterpret_cast<uint8_t*>(viewport) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The guard covers the QueryInterface, not only the load: QI is a vtable dispatch through the very
// pointer this distrusts. A recook that moves the swapchain field leaves the AOB matching and a
// non-null value at the old offset, so the dispatch is where a drifted offset faults.
bool QiIsSwapChain(IDXGISwapChain* sc) {
    __try {
        IDXGISwapChain* probe = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain),
                                      reinterpret_cast<void**>(&probe))) || !probe)
            return false;
        probe->Release();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The swapchain, re-read every call and QI-validated on change: a fullscreen transition replaces
// the object, so a cached pointer would draw into an abandoned swapchain; and QueryInterface is a
// COM call on a ~120 Hz path proving an offset, which drifts between builds, not between frames.
// Fails closed: an unprovable pointer yields null, complained about once per pointer.
IDXGISwapChain* ValidatedSwapChain(void* viewport, size_t off, const char* which,
                                   IDXGISwapChain*& lastOk) {
    IDXGISwapChain* sc = RawViewportSwapChain(viewport, off);
    if (!sc) return nullptr;
    if (sc == lastOk) return sc;
    if (!QiIsSwapChain(sc)) {
        static const void* sComplainedAbout = nullptr;
        if (sComplainedAbout != sc) {
            sComplainedAbout = sc;
            UE_LOGE("imgui_overlay: %s -- the pointer at viewport+0x%zX (%p) does not "
                    "QueryInterface as IDXGISwapChain. The offset has drifted for this "
                    "build; the overlay draws NOTHING rather than through an unidentified "
                    "pointer -- re-derive per docs/versioning.md.",
                    which, off, static_cast<void*>(sc));
            ue_wrap::log::Flush();
        }
        return nullptr;
    }
    lastOk = sc;
    return sc;
}

// The draw seam. An inline patch on IDXGISwapChain::Present is the byte region RivaTuner restores
// (the overlay died after a frame) and sits below where OBS copies the backbuffer;
// FD3D11Viewport::PresentChecked and FD3D12Viewport::PresentInternal are the engine's own
// once-per-frame callers of that Present, upstream of every external hook, found by AOB (the price
// every core hook here pays; docs/versioning.md covers a recook). The draw is unconditional
// (PresentChecked's non-presenting exits would be a site list to mirror; a wasted draw is
// harmless). The return is forwarded as a register-width integer: the DX12 seam tail-jumps into
// Present, whose HRESULT return disagrees with UE's bool declaration, and forwarding RAX byte-exact
// needs no verdict.
using EnginePresentFn = uintptr_t(__fastcall*)(void* viewport, int32_t syncInterval);
EnginePresentFn g_d3d11PresentTrampoline = nullptr;
EnginePresentFn g_d3d12PresentTrampoline = nullptr;
// One proven-pointer cache per RHI, shared by its present seam and its resize bracket: they read
// the same field of the same viewport.
IDXGISwapChain* g_lastOkSc11 = nullptr;
IDXGISwapChain* g_lastOkSc12 = nullptr;

uintptr_t __fastcall D3D11PresentCheckedDetour(void* viewport, int32_t syncInterval) {
    if (IDXGISwapChain* sc = ValidatedSwapChain(viewport, ue_wrap::profile::kD3D11Viewport_SwapChain,
                                                "FD3D11Viewport::PresentChecked",
                                                g_lastOkSc11))
    {
        overlay_backend::SelectRhi(overlay_backend::Rhi::Dx11);
        imgui_overlay::OverlayFrame(sc);
    }
    return g_d3d11PresentTrampoline(viewport, syncInterval);
}

uintptr_t __fastcall D3D12PresentInternalDetour(void* viewport, int32_t syncInterval) {
    if (IDXGISwapChain* sc = ValidatedSwapChain(viewport, ue_wrap::profile::kD3D12Viewport_SwapChain,
                                                "FD3D12Viewport::PresentInternal",
                                                g_lastOkSc12))
    {
        overlay_backend::SelectRhi(overlay_backend::Rhi::Dx12);
        imgui_overlay::OverlayFrame(sc);
    }
    return g_d3d12PresentTrampoline(viewport, syncInterval);
}

// The resize bracket, on the engine's own seam. IDXGISwapChain::ResizeBuffers fails unless every
// back-buffer reference is released first, and UE turns that failure into a Fatal; our render
// target view is such a reference, and a release bracket on the DXGI function itself is unlinked
// by RivaTuner (measured: the second resize with it armed killed the game). FD3D11Viewport::Resize
// and FD3D12Viewport::ResizeInternal are the engine's private callers, which nothing else patches.
// The swapchain is re-read after the original, never cached: a fullscreen transition can replace
// it. Two signatures: the DX12 seam takes only `this` and reads the extent from a member.
using D3D11ResizeFn = void(__fastcall*)(void* viewport, uint32_t sizeX, uint32_t sizeY,
                                        uint8_t bFullscreen, int32_t pixelFormat);
using D3D12ResizeInternalFn = void(__fastcall*)(void* viewport);
D3D11ResizeFn         g_d3d11ResizeTrampoline = nullptr;
D3D12ResizeInternalFn g_d3d12ResizeTrampoline = nullptr;

// The half both seams share: re-derive the render target after the engine resized. The call to the
// original stays with the caller (the two take different arguments), and `sizeNote` is
// pre-formatted because only one seam has any size to report.
void EngineResizeBracket(const char* which, size_t scOff, IDXGISwapChain*& lastOk,
                         void* viewport, const char* sizeNote) {
    IDXGISwapChain* sc = ValidatedSwapChain(viewport, scOff, which, lastOk);
    if (sc && imgui_overlay::ImguiReady()) {
        overlay_backend::OnResizeRecreate(sc);
        // Resizes are rare, so one line each is not spam, and it is the one line that says the
        // bracket ran, with or without RivaTuner in the process.
        UE_LOGI("imgui_overlay: %s%s -- render target rebuilt on %s", which, sizeNote,
                overlay_backend::Kind() ? overlay_backend::Kind() : "?");
        ue_wrap::log::Flush();
    }
    // A null `sc` needs no line: ValidatedSwapChain complained once about the pointer that caused
    // it.
}

void __fastcall D3D11ResizeDetour(void* viewport, uint32_t sizeX, uint32_t sizeY,
                                  uint8_t bFullscreen, int32_t pixelFormat) {
    overlay_backend::OnResizeRelease();   // the back buffers are about to be recreated
    g_d3d11ResizeTrampoline(viewport, sizeX, sizeY, bFullscreen, pixelFormat);
    char note[96];
    _snprintf_s(note, sizeof(note), _TRUNCATE, " (%ux%u fullscreen=%u fmt=%d)", sizeX, sizeY,
                bFullscreen, pixelFormat);
    EngineResizeBracket("FD3D11Viewport::Resize", ue_wrap::profile::kD3D11Viewport_SwapChain,
                        g_lastOkSc11, viewport, note);
}

void __fastcall D3D12ResizeDetour(void* viewport) {
    overlay_backend::OnResizeRelease();
    g_d3d12ResizeTrampoline(viewport);
    // No size in the line: ResizeInternal takes only `this`, so four numbers here would be register
    // residue.
    EngineResizeBracket("FD3D12Viewport::ResizeInternal",
                        ue_wrap::profile::kD3D12Viewport_SwapChain, g_lastOkSc12, viewport, "");
}

}  // namespace

bool Install(int& armed) {
    // Both RHIs' seams are installed unconditionally: which one the game presents with is unknown
    // at Init, and the other signature simply does not resolve in a single-RHI build. A stale
    // signature is not fatal: the overlay keeps drawing and says loudly that resizes are
    // unprotected.
    struct Seam { const char* name; const char* sig; void* detour; void** tramp; bool draws; };
    const Seam seams[] = {
        {"FD3D11Viewport::PresentChecked", ue_wrap::profile::kSigD3D11ViewportPresentChecked,
         reinterpret_cast<void*>(&D3D11PresentCheckedDetour),
         reinterpret_cast<void**>(&g_d3d11PresentTrampoline), true},
        {"FD3D12Viewport::PresentInternal", ue_wrap::profile::kSigD3D12ViewportPresentInternal,
         reinterpret_cast<void*>(&D3D12PresentInternalDetour),
         reinterpret_cast<void**>(&g_d3d12PresentTrampoline), true},
        {"FD3D11Viewport::Resize", ue_wrap::profile::kSigD3D11ViewportResize,
         reinterpret_cast<void*>(&D3D11ResizeDetour),
         reinterpret_cast<void**>(&g_d3d11ResizeTrampoline), false},
        {"FD3D12Viewport::ResizeInternal", ue_wrap::profile::kSigD3D12ViewportResizeInternal,
         reinterpret_cast<void*>(&D3D12ResizeDetour),
         reinterpret_cast<void**>(&g_d3d12ResizeTrampoline), false},
    };
    int drawArmed = 0;
    // Each requirement is met by either RHI's signature, so the scan ledger's two rows are decided once
    // the loop has tried all four.
    bool drawFound = false, resizeFound = false;
    for (const Seam& sm : seams) {
        const uintptr_t at = ue_wrap::FindPattern(sm.sig);
        if (at) (sm.draws ? drawFound : resizeFound) = true;
        if (!at) {
            // Expected for the RHI this cook does not contain; zero draw seams is checked after
            // the loop.
            UE_LOGW("imgui_overlay: %s signature NOT found -- that seam is ABSENT this "
                    "run.", sm.name);
            continue;
        }
        if (ue_wrap::hook::Install(reinterpret_cast<void*>(at), sm.detour, sm.tramp)) {
            ++armed;
            if (sm.draws) ++drawArmed;
            UE_LOGI("imgui_overlay: %s %s armed @%p (image+0x%llX)", sm.name,
                    sm.draws ? "DRAW seam" : "resize bracket",
                    reinterpret_cast<void*>(at),
                    static_cast<unsigned long long>(
                        at - reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr))));
        } else {
            UE_LOGE("imgui_overlay: %s resolved but MinHook Install FAILED", sm.name);
        }
    }
    ue_wrap::scan_ledger::Record(ue_wrap::scan_ledger::Scan::OverlayDraw, drawFound);
    ue_wrap::scan_ledger::Record(ue_wrap::scan_ledger::Scan::OverlayResize, resizeFound);
    // Fail closed on the draw seam only: with none there is no overlay at all, so this returns
    // false. The window messages are routed either way, and the input swallow in
    // HandleWindowMessage sits behind g_imguiReady, so no key is taken for a UI that cannot
    // appear. A missing resize bracket is a degradation, reported above.
    if (drawArmed == 0) {
        UE_LOGE("imgui_overlay: NO DRAW SEAM. Neither FD3D11Viewport::PresentChecked nor "
                "FD3D12Viewport::PresentInternal resolved, so the overlay cannot draw and "
                "is DISABLED for this run. This is what a game recook looks like from "
                "here -- re-derive the signatures per docs/versioning.md.");
        ue_wrap::log::Flush();
        return false;
    }
    return true;
}

}  // namespace ui::overlay_seams

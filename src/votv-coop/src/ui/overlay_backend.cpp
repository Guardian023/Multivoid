// ui/overlay_backend.cpp -- the RHI dispatcher behind ui/overlay_backend.h.
//
// Owns exactly one concept: WHICH per-RHI implementation is live. The engine seam that fired
// (the D3D11 or the D3D12 present detour) names it through SelectRhi before the frame runs;
// every entry below forwards to that RHI's ops table. DX12 additionally has to capture its
// presenting command queue first (a few frames -- see overlay_backend_dx12_capture.cpp).

#include "ui/overlay_backend.h"

#include "overlay_backend_internal.h"

#include <windows.h>
#include <wincodec.h>  // WIC: the skins-browser preview decode (PNG/BMP -> BGRA)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ui::overlay_backend {
namespace {

const BackendOps* g_ops = nullptr;  // render-thread-owned (all writers run in the Present detour)

// The dx halves take a FrameTarget; until the seams fill one, the swapchain is wrapped here.
FrameTarget Tgt(IDXGISwapChain* sc) { return {Rhi::None, nullptr, sc, nullptr}; }

}  // namespace

void SelectRhi(Rhi rhi) {
    switch (rhi) {
        case Rhi::Dx11: g_ops = &dx11::Ops(); break;
        case Rhi::Dx12: g_ops = &dx12::Ops(); break;
        default:        g_ops = nullptr; break;
    }
}

const char* Kind() { return g_ops ? g_ops->name : nullptr; }

bool CaptureDevice(IDXGISwapChain* sc) {
    return g_ops && g_ops->capture(Tgt(sc));  // true only once the presenting queue is CONFIRMED
}

void AbandonCapture() {
    if (g_ops) g_ops->abandon();
}

bool InitRenderer(IDXGISwapChain* sc) {
    return g_ops && g_ops->init(Tgt(sc));
}

void NewFrame() {
    if (g_ops) g_ops->newFrame();
}

void InvalidateDeviceObjects() {
    if (g_ops) g_ops->invalidate();
}

void EnsureTarget(IDXGISwapChain* sc) {
    if (g_ops) g_ops->ensureTarget(Tgt(sc));
}

void RenderDrawData(IDXGISwapChain* sc) {
    if (g_ops) g_ops->render(Tgt(sc));
}

// The resize pair is the DX resize bracket's mechanism: a half without one leaves it null.
void OnResizeRelease() {
    if (g_ops && g_ops->resizeRelease) g_ops->resizeRelease();
}

void OnResizeRecreate(IDXGISwapChain* sc) {
    if (g_ops && g_ops->resizeRecreate) g_ops->resizeRecreate(Tgt(sc));
}

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    return g_ops ? g_ops->textureFromFile(path, outW, outH) : nullptr;
}

void DestroyTexture(void* id) {
    if (g_ops) g_ops->destroyTexture(id);
}

void InstallCreationProbe() { dx12_capture::InstallCreationProbe(); }

// RULE 2: there is deliberately no Shutdown() here, and nothing needs one. InitRenderer's
// failure paths call ReleaseRendererState(); at process exit the OS reclaims what that released;
// and the one thing a dying process actually needs -- stopping new detour entries -- is
// hook::Shutdown's blanket disable.

namespace detail {

bool DecodeImageFileBgra(const wchar_t* path, std::vector<uint8_t>& outBgra,
                         unsigned& outW, unsigned& outH) {
    if (!path) return false;
    static bool s_comTried = false;
    if (!s_comTried) {
        s_comTried = true;
        // S_FALSE (already init) and RPC_E_CHANGED_MODE (STA already active on
        // this thread) both leave COM usable for CoCreateInstance below.
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    IWICImagingFactory* fac = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&fac))) || !fac)
        return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = false;
    do {
        if (FAILED(fac->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &dec)) || !dec)
            break;
        if (FAILED(dec->GetFrame(0, &frame)) || !frame) break;
        if (FAILED(fac->CreateFormatConverter(&conv)) || !conv) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            break;
        if (FAILED(conv->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096) break;
        outBgra.assign(static_cast<size_t>(w) * h * 4, 0);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(outBgra.size()),
                                    outBgra.data())))
            break;
        ok = true;
    } while (false);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    fac->Release();
    if (ok) { outW = w; outH = h; }
    return ok;
}

}  // namespace detail

}  // namespace ui::overlay_backend

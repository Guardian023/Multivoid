// ui/overlay_backend.h -- the RHI-specific half of the ImGui overlay.
//
// imgui_overlay.cpp owns the window-message handler and surface compositing (the platform layer);
// this interface owns everything that touches a concrete graphics device: device capture off the
// game's swapchain, the ImGui renderer backend, the render target, and texture creation for UI
// surfaces. One implementation per RHI -- overlay_backend_dx11.cpp and overlay_backend_dx12.cpp,
// both shipped -- selected by the engine seam that fired (SelectRhi, before each frame), not
// guessed from the swapchain. No RHI types cross this boundary: a FrameTarget carries what one
// frame draws into.

#pragma once

#include <cstdint>

namespace ui::overlay_backend {

enum class Rhi : uint8_t;
struct FrameTarget;  // what one frame draws into; defined for the backends (overlay_backend_internal.h)

// The engine seam that fired names its RHI before it runs the frame; every entry below forwards
// to that RHI's backend. Render thread only.
void SelectRhi(Rhi rhi);

// The selected RHI's name -- "DX11" / "DX12" -- nullptr until a seam selected one. Feeds the RHI
// indicator (F1 menu line + the bring-up log).
const char* Kind();

// First-present bring-up, step 1: capture the device/context off the target (the DX backends:
// the game's swapchain). false = the capture failed or is still pending (DX12 confirms its
// presenting queue over a few frames) -- the overlay stays down and Present passes through.
bool CaptureDevice(const FrameTarget& t);

// Unwind arm for a failure between CaptureDevice and InitRenderer: release
// everything CaptureDevice acquired.
void AbandonCapture();

// Bring-up, step 2 (ImGui context + Win32 backend are live): init the ImGui
// renderer backend and create the render target. false = caller unwinds.
bool InitRenderer(const FrameTarget& t);

// Per-frame, render thread, BEFORE ImGui::NewFrame.
void NewFrame();

// Drop the renderer's device objects so the next NewFrame lazily re-creates
// them (the font-atlas rescale path).
void InvalidateDeviceObjects();

// Re-create the render target if a resize dropped it (cheap no-op otherwise).
void EnsureTarget(const FrameTarget& t);

// Draw ImGui's current draw data into the target's backbuffer.
void RenderDrawData(const FrameTarget& t);

// ResizeBuffers bracket, called only from the DX resize detours: release backbuffer-derived state
// BEFORE the game's resize runs; re-derive it after a successful resize. A backend without a
// bracket leaves both null.
void OnResizeRelease();
void OnResizeRecreate(const FrameTarget& t);

// Decode an image file (PNG/BMP/JPG via WIC) into a device texture usable as
// an ImTextureID. Render thread only. The caller caches the result and MUST
// return it through DestroyTexture when done (a dropped id leaks the texture).
void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH);

// Release a texture returned by CreateTextureFromImageFile. Render thread
// only. DX11: releases the SRV (the texture rides its refcount). null is a
// no-op.
void DestroyTexture(void* id);

// Install the swapchain-creation timing probe (the DX12 stage-1 measurement:
// does our boot precede the game's swapchain creation, and what queue arrives
// as CreateSwapChain*'s pDevice). Called once from imgui_overlay::Init;
// log-only, no behavior change on any RHI.
void InstallCreationProbe();


}  // namespace ui::overlay_backend

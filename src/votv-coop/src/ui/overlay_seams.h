// ui/overlay_seams.h -- TU-private seam between imgui_overlay.cpp (the host: bring-up, the frame, the
// window messages) and overlay_seams.cpp (the engine seams that deliver each present and each resize).
// Nothing else includes this.

#pragma once

namespace ui::overlay_backend { struct FrameTarget; }

namespace ui::imgui_overlay {

// One frame of overlay work; the target arrives from whichever present seam fired.
void OverlayFrame(const overlay_backend::FrameTarget& t);

// ImGui and the RHI backend are live (the resize bracket acts only then).
bool ImguiReady();

}  // namespace ui::imgui_overlay

namespace ui::overlay_seams {

// AOB-resolve and MinHook the present and resize seams; `armed` receives how many of the `total`
// seams installed. false = no draw seam resolved, so the overlay cannot draw this run.
bool Install(int& armed, int& total);

}  // namespace ui::overlay_seams

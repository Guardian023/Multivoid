// coop/dev/vulkan_path_probe.h -- [dev] which route the game's Vulkan renderer takes to present.
//
// Under the Vulkan RHI the overlay's two present seams (the D3D11 and D3D12 viewports) never run,
// so a render half needs the loader's own entry points. This probe answers, from log lines tagged
// [VK-PATH]: when vulkan-1.dll is mapped against our boot (and, if later, the hooks installed as it
// maps, to see the instance, the device and its queues being created), whether the renderer reaches the device
// functions through the loader's exports, vkGetInstanceProcAddr or vkGetDeviceProcAddr, whether a
// detour on the address it really calls sees one present per frame, and what the swapchain, the
// device and the presenting queue are. Armed by vulkan_path_probe=1 (env VOTVCOOP_VULKAN_PATH_PROBE);
// when off nothing is installed. The module is found with GetModuleHandleW and its exports with
// GetProcAddress, so the DLL gains no import. Observation only: no argument is altered.
#pragma once

namespace coop::dev::vulkan_path_probe {

// The earliest point our code runs (harness::Start): one config read when off. When on, logs whether
// vulkan-1.dll is mapped yet and installs the detours at once if so; otherwise installs a one-shot
// detour on KernelBase!LoadLibraryExW that installs them on the loading thread as the loader returns
// and then lifts itself. Call once, before the pump's thread exists.
void Start();

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off. Prints the
// one-second summary for the first ten seconds of presents and a DONE line.
void Tick();

}  // namespace coop::dev::vulkan_path_probe

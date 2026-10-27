// coop/dev/vulkan_rhi_probe.h -- [dev] what ue_wrap/engine/vulkan_rhi reads from the game's Vulkan
// renderer, checked against the Vulkan API. Armed by vulkan_rhi_probe=1 (env VOTVCOOP_VULKAN_RHI_PROBE);
// when off nothing is resolved or hooked.
//
// At the first tick with vulkan-1.dll mapped it resolves the Present signature, the pixel-format table
// and the loader slots of vkQueuePresentKHR, vkDestroySwapchainKHR and vkQueueSubmit, hooks the
// viewport's Present (pass-through), and reads the viewport on each call. Once a viewport has been read
// it swaps the present and submit slots for pass-through counters (calling threads, queues), prints a
// per-second summary for 10 s and a final DONE line. The format and image count read are
// compared with what vkGetPhysicalDeviceSurfaceFormatsKHR and vkGetSwapchainImagesKHR answer. The slots
// are restored by Shutdown. Observation only: no argument or result is altered.
#pragma once

namespace coop::dev::vulkan_rhi_probe {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off. Game thread.
void Tick();

// Puts the swapped slots back. Called from the shutdown path before the hooks are lifted; idempotent,
// and a latched atomic read when nothing was swapped.
void Shutdown();

}  // namespace coop::dev::vulkan_rhi_probe

// coop/config/config_registry_rows_dev_vulkan.inc -- the [dev] rows of the Vulkan overlay arc: the present-path
// probe. Included by config_registry_rows_dev.inc after its last row, as _dev_nb.inc is, so each row keeps its
// place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(vulkan_path_probe, "vulkan_path_probe", "dev", false, "VOTVCOOP_VULKAN_PATH_PROBE",
    "Probe (run the game with the Vulkan renderer): when vulkan-1.dll is mapped against our boot (hooks "
    "go in at the earliest point, or as the library maps), which entry points the renderer queries and by "
    "which route, the instance, physical device, device and queue it creates, whether a detour on the "
    "address it calls sees one present per frame, and the swapchain behind it. Lines tagged [VK-PATH], "
    "a per-second summary for the first 10 s of presents, then DONE. Observation only; installs "
    "detours on the Vulkan loader, and on KernelBase LoadLibraryExW until the library maps, while on.")

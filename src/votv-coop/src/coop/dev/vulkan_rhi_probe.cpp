// coop/dev/vulkan_rhi_probe.cpp -- [dev] the Vulkan renderer's engine reads and slots. See the header.
//
// The Present detour and the two slot replacements run on the renderer's threads: they touch atomics,
// fixed arrays and the rate-limited log alone, with no allocation. The setup, the slot swap and the
// summary run on the game thread, from Tick.
#include "coop/dev/vulkan_rhi_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sig_scan.h"
#include "ue_wrap/engine/vulkan_rhi.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coop::dev::vulkan_rhi_probe {
namespace {

namespace VK = ue_wrap::vulkan_rhi;

using Clock    = std::chrono::steady_clock;
using Handle   = VK::Handle;
using VkResult = std::int32_t;

// VkPresentInfoKHR and VkSurfaceFormatKHR as the Vulkan specification lays them out on a 64-bit target.
struct PresentInfo {
    std::uint32_t        sType;
    const void*          pNext;
    std::uint32_t        waitSemaphoreCount;
    const Handle*        pWaitSemaphores;
    std::uint32_t        swapchainCount;
    const Handle*        pSwapchains;
    const std::uint32_t* pImageIndices;
    VkResult*            pResults;
};
static_assert(sizeof(PresentInfo) == 64 && offsetof(PresentInfo, swapchainCount) == 32 &&
              offsetof(PresentInfo, pSwapchains) == 40);

struct SurfaceFormat {
    std::int32_t format;
    std::int32_t colorSpace;
};
static_assert(sizeof(SurfaceFormat) == 8);

using ViewportPresentFn = bool(__fastcall*)(void* self, void* ctx, void* cmd, void* queue, void* presentQueue,
                                            bool lockToVsync);
using QueuePresentFn    = VkResult (*)(Handle queue, const PresentInfo* info);
using QueueSubmitFn     = VkResult (*)(Handle queue, std::uint32_t count, const void* submits, Handle fence);
using SurfaceFormatsFn  = VkResult (*)(Handle physical, Handle surface, std::uint32_t* count, SurfaceFormat* out);
using SwapchainImagesFn = VkResult (*)(Handle device, Handle swapchain, std::uint32_t* count, Handle* images);

constexpr std::uint64_t kLogFirst  = 3;
constexpr std::uint64_t kLogEvery  = 300;
constexpr int           kSeconds   = 10;
constexpr int           kSubmitRows = 8;
constexpr std::uint32_t kMaxFormats = 64;
constexpr VkResult      kVkIncomplete = 5;

const char* const kSlotNames[3] = {"vkQueuePresentKHR", "vkDestroySwapchainKHR", "vkQueueSubmit"};
enum : int { kSlotPresent = 0, kSlotDestroy = 1, kSlotSubmit = 2 };

// ---- state ----------------------------------------------------------------------------------

ViewportPresentFn g_vpTramp = nullptr;
std::atomic<std::uint64_t> g_vpCount{0};

std::atomic<bool>          g_readClaimed{false};  // the first successful read is kept whole
std::atomic<bool>          g_readReady{false};
VK::ViewportState          g_first{};
std::atomic<Handle>        g_readQueue{0};        // the latest read, for the replacements' comparisons
std::atomic<Handle>        g_readSwapchain{0};
std::atomic<std::int32_t>  g_fmt{-1};
std::atomic<std::int32_t>  g_imgs{-1};
std::atomic<int>           g_fmtOk{-1};           // 1 / 0, or -1: not checked
std::atomic<int>           g_imgOk{-1};

std::uintptr_t g_slot[3]  = {};
void*          g_saved[3] = {};                   // written before the slot swap, read by the replacements
bool           g_swappedSlot[3] = {};
std::atomic<bool> g_swapped{false};

std::atomic<std::uint64_t> g_presCount{0};
std::atomic<std::uint32_t> g_presTid{0};
std::atomic<std::uint64_t> g_presQueueBad{0};
std::atomic<std::uint64_t> g_presSwapBad{0};
std::atomic<std::uint64_t> g_submitCount{0};
// Calls on the present queue in flight right now (presents and submits), and the entries that found another
// already inside: the queue is externally synchronized (Vulkan spec), so a non-zero overlap is a race.
std::atomic<int>           g_queueInside{0};
std::atomic<std::uint64_t> g_queueOverlap{0};
std::atomic<std::uint64_t> g_foreignSubmits{0};  // present-queue submits from a thread other than the presenter

struct SubmitRow {
    std::atomic<std::uint32_t> tid{0};
    std::atomic<Handle>        queue{0};
    std::atomic<std::uint64_t> count{0};
};
SubmitRow                  g_submit[kSubmitRows];
std::atomic<std::uint64_t> g_submitOverflow{0};

unsigned long long U(std::uint64_t v) { return static_cast<unsigned long long>(v); }
std::uint32_t Tid() { return static_cast<std::uint32_t>(::GetCurrentThreadId()); }
bool Logged(std::uint64_t n) { return n <= kLogFirst || n % kLogEvery == 0; }

// ---- the checks against the Vulkan API --------------------------------------------------------

// fmtOk: the read VkFormat is among the surface's formats; imgOk: the read image count is the swapchain's.
void CheckAgainstVulkan(const VK::ViewportState& st, int& fmtOk, int& imgOk) {
    fmtOk = imgOk = -1;
    const auto formats = reinterpret_cast<SurfaceFormatsFn>(
        VK::GetInstanceProcAddr(st.vkInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    if (formats) {
        std::uint32_t count = 0;
        SurfaceFormat list[kMaxFormats];
        if (formats(st.vkPhysicalDevice, st.vkSurface, &count, nullptr) == 0) {
            std::uint32_t n = count < kMaxFormats ? count : kMaxFormats;
            const VkResult r = formats(st.vkPhysicalDevice, st.vkSurface, &n, list);
            if (r == 0 || r == kVkIncomplete) {
                fmtOk = 0;
                for (std::uint32_t i = 0; i < n; ++i)
                    if (list[i].format == st.vkFormat) fmtOk = 1;
            }
        }
    }
    const auto images = reinterpret_cast<SwapchainImagesFn>(
        VK::GetDeviceProcAddr(st.vkInstance, st.vkDevice, "vkGetSwapchainImagesKHR"));
    if (images) {
        std::uint32_t count = 0;
        if (images(st.vkDevice, st.vkSwapchain, &count, nullptr) == 0) imgOk = (count == st.imageCount) ? 1 : 0;
    }
}

void LogViewport(std::uint64_t n, const VK::ViewportState& st) {
    int fmtOk = -1, imgOk = -1;
    CheckAgainstVulkan(st, fmtOk, imgOk);
    g_fmt.store(st.vkFormat, std::memory_order_relaxed);
    g_imgs.store(static_cast<std::int32_t>(st.imageCount), std::memory_order_relaxed);
    g_fmtOk.store(fmtOk, std::memory_order_relaxed);
    g_imgOk.store(imgOk, std::memory_order_relaxed);
    UE_LOGI("[VK-PATH] viewport n=%llu vp=%p dev=%p rhi=%p swapchainObj=%p queueObj=%p hwnd=0x%llx "
            "instance=0x%llx physical=0x%llx device=0x%llx queue=0x%llx family=%u index=%u "
            "swapchain=0x%llx surface=0x%llx size=%ux%u internal=%ux%u images=%u pf=%d vkFormat=%d acquired=%d "
            "raw0=0x%08x raw1=0x%08x fmtOk=%d imgOk=%d tid=%lu",
            U(n), reinterpret_cast<void*>(st.viewport), reinterpret_cast<void*>(st.device),
            reinterpret_cast<void*>(st.rhi), reinterpret_cast<void*>(st.swapChain),
            reinterpret_cast<void*>(st.presentQueue), U(st.hwnd), U(st.vkInstance), U(st.vkPhysicalDevice),
            U(st.vkDevice), U(st.vkQueue), st.queueFamily, st.queueIndex, U(st.vkSwapchain), U(st.vkSurface),
            st.sizeX, st.sizeY, st.internalW, st.internalH, st.imageCount, st.pixelFormat, st.vkFormat,
            st.acquiredImage, st.swapChainRaw0, st.swapChainRaw1, fmtOk, imgOk, static_cast<unsigned long>(Tid()));
}

// ---- the detours ------------------------------------------------------------------------------

bool __fastcall OnViewportPresent(void* self, void* ctx, void* cmd, void* queue, void* presentQueue,
                                  bool lockToVsync) {
    const std::uint64_t n = g_vpCount.fetch_add(1, std::memory_order_relaxed) + 1;
    VK::ViewportState st;
    if (VK::ReadViewport(self, st)) {
        g_readQueue.store(st.vkQueue, std::memory_order_relaxed);
        g_readSwapchain.store(st.vkSwapchain, std::memory_order_relaxed);
        if (!g_readClaimed.exchange(true)) {
            g_first = st;
            g_readReady.store(true, std::memory_order_release);
        }
        if (Logged(n)) LogViewport(n, st);
    } else if (Logged(n)) {
        UE_LOGW("[VK-PATH] viewport n=%llu vp=%p: the read was refused (fault or implausible value)", U(n), self);
    }
    return g_vpTramp(self, ctx, cmd, queue, presentQueue, lockToVsync);
}

VkResult OnQueuePresent(Handle queue, const PresentInfo* info) {
    const std::uint64_t n = g_presCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint32_t tid = Tid();
    g_presTid.store(tid, std::memory_order_relaxed);
    const bool queueOk = queue == g_readQueue.load(std::memory_order_relaxed);
    const bool swapOk  = info && info->swapchainCount > 0 && info->pSwapchains &&
                         info->pSwapchains[0] == g_readSwapchain.load(std::memory_order_relaxed);
    if (!queueOk) g_presQueueBad.fetch_add(1, std::memory_order_relaxed);
    if (!swapOk) g_presSwapBad.fetch_add(1, std::memory_order_relaxed);
    if (queueOk && g_queueInside.fetch_add(1, std::memory_order_acq_rel) > 0)
        g_queueOverlap.fetch_add(1, std::memory_order_relaxed);
    if (Logged(n))
        UE_LOGI("[VK-PATH] vkQueuePresentKHR n=%llu tid=%lu queue=0x%llx queueIsPresentQueue=%d swapchain0IsRead=%d",
                U(n), static_cast<unsigned long>(tid), U(queue), queueOk ? 1 : 0, swapOk ? 1 : 0);
    const VkResult r = reinterpret_cast<QueuePresentFn>(g_saved[kSlotPresent])(queue, info);
    if (queueOk) g_queueInside.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}

// The row of (thread, queue), claimed on first sight. A row's thread is set by that thread alone.
SubmitRow* SubmitRowOf(std::uint32_t tid, Handle queue) {
    for (SubmitRow& r : g_submit)
        if (r.tid.load(std::memory_order_acquire) == tid && r.queue.load(std::memory_order_acquire) == queue) return &r;
    for (SubmitRow& r : g_submit) {
        std::uint32_t unclaimed = 0;
        if (r.tid.compare_exchange_strong(unclaimed, tid, std::memory_order_acq_rel)) {
            r.queue.store(queue, std::memory_order_release);
            return &r;
        }
    }
    return nullptr;
}

VkResult OnQueueSubmit(Handle queue, std::uint32_t count, const void* submits, Handle fence) {
    g_submitCount.fetch_add(1, std::memory_order_relaxed);
    if (SubmitRow* row = SubmitRowOf(Tid(), queue)) row->count.fetch_add(1, std::memory_order_relaxed);
    else g_submitOverflow.fetch_add(1, std::memory_order_relaxed);
    const bool onPresentQueue = queue != 0 && queue == g_readQueue.load(std::memory_order_relaxed);
    if (!onPresentQueue) return reinterpret_cast<QueueSubmitFn>(g_saved[kSlotSubmit])(queue, count, submits, fence);
    const std::uint32_t presTid = g_presTid.load(std::memory_order_relaxed);
    if (presTid != 0 && Tid() != presTid) {
        const std::uint64_t f = g_foreignSubmits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (Logged(f))
            UE_LOGI("[VK-PATH] foreign submit n=%llu tid=%lu at present n=%llu", U(f), static_cast<unsigned long>(Tid()),
                    U(g_presCount.load(std::memory_order_relaxed)));
    }
    if (g_queueInside.fetch_add(1, std::memory_order_acq_rel) > 0)
        g_queueOverlap.fetch_add(1, std::memory_order_relaxed);
    const VkResult r = reinterpret_cast<QueueSubmitFn>(g_saved[kSlotSubmit])(queue, count, submits, fence);
    g_queueInside.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}

// ---- setup, slot swap, summary ------------------------------------------------------------------

std::uintptr_t Rva(std::uintptr_t addr) {
    std::uintptr_t base = 0;
    size_t size = 0;
    ue_wrap::MainModuleRange(base, size);
    return addr ? addr - base : 0;
}

enum class Stage { WaitModule, WaitRead, Summary, Idle };
Stage g_stage = Stage::WaitModule;

// Resolves the three signatures' targets and slots and hooks the viewport's Present. Game thread.
void Setup() {
    const std::uintptr_t present = VK::ResolvePresent();
    const std::uintptr_t table   = VK::ResolvePixelFormatTable();
    UE_LOGI("[VK-PATH] sig FVulkanViewport::Present rva=0x%llx, GPixelFormats rva=0x%llx", U(Rva(present)),
            U(Rva(table)));
    for (int i = 0; i < 3; ++i) {
        g_slot[i] = VK::ResolveLoaderSlot(kSlotNames[i]);
        UE_LOGI("[VK-PATH] slot %s rva=0x%llx", kSlotNames[i], U(Rva(g_slot[i])));
    }
    if (!present || !table) {
        UE_LOGE("[VK-PATH] RHI FAIL: a signature did not resolve -- nothing hooked");
        g_stage = Stage::Idle;
        return;
    }
    if (!ue_wrap::hook::Init() ||
        !ue_wrap::hook::Install(reinterpret_cast<void*>(present), reinterpret_cast<void*>(&OnViewportPresent),
                                reinterpret_cast<void**>(&g_vpTramp))) {
        UE_LOGE("[VK-PATH] RHI FAIL: the detour on FVulkanViewport::Present did not install");
        g_stage = Stage::Idle;
        return;
    }
    UE_LOGI("[VK-PATH] hook FVulkanViewport::Present installed (pass-through)");
    g_stage = Stage::WaitRead;
}

void LogSlotValues(HMODULE mod, Handle instance) {
    for (int i = 0; i < 3; ++i) {
        if (!g_slot[i]) continue;
        const void* value = *reinterpret_cast<void* const volatile*>(g_slot[i]);
        const void* exported = reinterpret_cast<const void*>(::GetProcAddress(mod, kSlotNames[i]));
        const void* viaInstance = reinterpret_cast<const void*>(VK::GetInstanceProcAddr(instance, kSlotNames[i]));
        UE_LOGI("[VK-PATH] slot %s rva=0x%llx value==export:%d value==instanceProc:%d", kSlotNames[i],
                U(Rva(g_slot[i])), value == exported ? 1 : 0, value == viaInstance ? 1 : 0);
    }
}

// Swaps one slot: the saved pointer is stored before the exchange, so a replacement never sees null.
void SwapOne(int i, void* replacement) {
    if (!g_slot[i]) return;
    g_saved[i] = *reinterpret_cast<void* const volatile*>(g_slot[i]);
    void* old = VK::SwapSlot(g_slot[i], replacement);
    g_swappedSlot[i] = true;
    if (old != g_saved[i]) UE_LOGW("[VK-PATH] slot %s changed under the swap (saved=%p old=%p)", kSlotNames[i], g_saved[i], old);
}

Clock::time_point g_start{}, g_next{};
int           g_printed = 0;
std::uint64_t g_prevPres = 0, g_prevVp = 0, g_prevSubmit = 0;

void SwapAndStart() {
    SwapOne(kSlotPresent, reinterpret_cast<void*>(&OnQueuePresent));
    SwapOne(kSlotSubmit, reinterpret_cast<void*>(&OnQueueSubmit));
    g_swapped.store(true, std::memory_order_release);
    g_start = Clock::now();
    g_next  = g_start + std::chrono::seconds(1);
    UE_LOGI("[VK-PATH] slots swapped: present=%d submit=%d", g_swappedSlot[kSlotPresent] ? 1 : 0,
            g_swappedSlot[kSlotSubmit] ? 1 : 0);
    g_stage = Stage::Summary;
}

void PrintSecond() {
    const std::uint64_t pres = g_presCount.load(), vp = g_vpCount.load(), sub = g_submitCount.load();
    UE_LOGI("[VK-PATH] RHI t=%d viewportPresents=%llu/s queuePresents=%llu/s submits=%llu/s queueMismatch=%llu "
            "swapchainMismatch=%llu submitRowsFull=%llu",
            g_printed + 1, U(vp - g_prevVp), U(pres - g_prevPres), U(sub - g_prevSubmit),
            U(g_presQueueBad.load()), U(g_presSwapBad.load()), U(g_submitOverflow.load()));
    g_prevPres = pres;
    g_prevVp = vp;
    g_prevSubmit = sub;
}

void PrintDone() {
    const Handle presentQueue = g_readQueue.load();
    const std::uint32_t presentTid = g_presTid.load();
    char threads[192];
    size_t at = 0;
    threads[0] = '\0';
    int onQueue = 0, onPresentTid = 0;
    for (const SubmitRow& r : g_submit) {
        const std::uint32_t tid = r.tid.load();
        if (!tid || r.queue.load() != presentQueue) continue;
        ++onQueue;
        if (tid == presentTid) ++onPresentTid;
        if (at + 32 < sizeof threads)
            at += static_cast<size_t>(std::snprintf(threads + at, sizeof threads - at, "%s%lu:%llu", at ? "," : "",
                                                    static_cast<unsigned long>(tid), U(r.count.load())));
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - g_start).count();
    const double perSecond = seconds > 0.0 ? static_cast<double>(g_presCount.load()) / seconds : 0.0;
    UE_LOGI("[VK-PATH] RHI DONE presents/s=%.1f submitThreads=%s presentTid=%lu sameThread=%d fmt=%d imgs=%d "
            "fmtOk=%d imgOk=%d queueOverlap=%llu foreignSubmits=%llu",
            perSecond, at ? threads : "none", static_cast<unsigned long>(presentTid),
            (onQueue > 0 && onQueue == onPresentTid) ? 1 : 0, g_fmt.load(), g_imgs.load(), g_fmtOk.load(),
            g_imgOk.load(), U(g_queueOverlap.load()), U(g_foreignSubmits.load()));
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::vulkan_rhi_probe);
    if (!s_on) return;
    switch (g_stage) {
        case Stage::WaitModule:
            if (::GetModuleHandleW(L"vulkan-1.dll")) Setup();
            return;
        case Stage::WaitRead:
            if (!g_readReady.load(std::memory_order_acquire)) return;
            LogSlotValues(::GetModuleHandleW(L"vulkan-1.dll"), g_first.vkInstance);
            SwapAndStart();
            return;
        case Stage::Summary:
            if (Clock::now() < g_next) return;
            g_next += std::chrono::seconds(1);
            PrintSecond();
            if (++g_printed < kSeconds) return;
            PrintDone();
            g_next = Clock::now() + std::chrono::seconds(30);
            g_stage = Stage::Idle;
            return;
        case Stage::Idle:
            // The engine's foreign submits come late (a save), so the race counters keep reporting after DONE.
            if (Clock::now() < g_next) return;
            g_next += std::chrono::seconds(30);
            UE_LOGI("[VK-PATH] RHI totals presents=%llu queueOverlap=%llu foreignSubmits=%llu",
                    U(g_presCount.load()), U(g_queueOverlap.load()), U(g_foreignSubmits.load()));
            return;
    }
}

void Shutdown() {
    if (!g_swapped.exchange(false, std::memory_order_acq_rel)) return;
    for (int i = 0; i < 3; ++i)
        if (g_swappedSlot[i]) VK::RestoreSlot(g_slot[i], g_saved[i]);
    UE_LOGI("[VK-PATH] slots restored: present=%d submit=%d", g_swappedSlot[kSlotPresent] ? 1 : 0,
            g_swappedSlot[kSlotSubmit] ? 1 : 0);
}

}  // namespace coop::dev::vulkan_rhi_probe

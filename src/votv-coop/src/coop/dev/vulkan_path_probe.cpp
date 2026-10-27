// coop/dev/vulkan_path_probe.cpp -- [dev] the game's Vulkan present path. See the header.
//
// The structs below are the few the probe reads, declared at the layout the Vulkan specification
// gives them on a 64-bit target (VkPresentInfoKHR, VkSwapchainCreateInfoKHR, VkDeviceCreateInfo,
// VkDeviceQueueCreateInfo); every handle is an opaque 64-bit value. No SDK header is included and
// vulkan-1.dll is reached with GetModuleHandleW and GetProcAddress only.
//
// A detour may fire on any thread. The present detours touch atomics and the rate-limited log
// alone, with no allocation; the summary lines are printed from the game thread's tick.
#include "coop/dev/vulkan_path_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/input/game_window_proc.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace coop::dev::vulkan_path_probe {
namespace {

using Clock    = std::chrono::steady_clock;
using Handle   = std::uint64_t;
using VkResult = std::int32_t;
using PfnVoid  = void (*)();

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
              offsetof(PresentInfo, pSwapchains) == 40 && offsetof(PresentInfo, pImageIndices) == 48);

struct SwapchainCreateInfo {
    std::uint32_t        sType;
    const void*          pNext;
    std::uint32_t        flags;
    Handle               surface;
    std::uint32_t        minImageCount;
    std::int32_t         imageFormat;
    std::int32_t         imageColorSpace;
    std::uint32_t        width;
    std::uint32_t        height;
    std::uint32_t        imageArrayLayers;
    std::uint32_t        imageUsage;
    std::int32_t         imageSharingMode;
    std::uint32_t        queueFamilyIndexCount;
    const std::uint32_t* pQueueFamilyIndices;
    std::uint32_t        preTransform;
    std::uint32_t        compositeAlpha;
    std::int32_t         presentMode;
    std::uint32_t        clipped;
    Handle               oldSwapchain;
};
static_assert(sizeof(SwapchainCreateInfo) == 104 && offsetof(SwapchainCreateInfo, surface) == 24 &&
              offsetof(SwapchainCreateInfo, width) == 44 && offsetof(SwapchainCreateInfo, imageUsage) == 56 &&
              offsetof(SwapchainCreateInfo, presentMode) == 88 && offsetof(SwapchainCreateInfo, oldSwapchain) == 96);

struct DeviceQueueCreateInfo {
    std::uint32_t sType;
    const void*   pNext;
    std::uint32_t flags;
    std::uint32_t queueFamilyIndex;
    std::uint32_t queueCount;
    const float*  pQueuePriorities;
};
static_assert(sizeof(DeviceQueueCreateInfo) == 40 && offsetof(DeviceQueueCreateInfo, queueFamilyIndex) == 20);

struct DeviceCreateInfo {
    std::uint32_t                sType;
    const void*                  pNext;
    std::uint32_t                flags;
    std::uint32_t                queueCreateInfoCount;
    const DeviceQueueCreateInfo* pQueueCreateInfos;
    std::uint32_t                enabledLayerCount;
    const char* const*           ppEnabledLayerNames;
    std::uint32_t                enabledExtensionCount;
    const char* const*           ppEnabledExtensionNames;
    const void*                  pEnabledFeatures;
};
static_assert(sizeof(DeviceCreateInfo) == 72 && offsetof(DeviceCreateInfo, pQueueCreateInfos) == 24 &&
              offsetof(DeviceCreateInfo, enabledExtensionCount) == 48);

constexpr std::uint32_t kUsageColorAttachment = 0x10;  // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT

using GetProcAddrFn     = PfnVoid (*)(Handle, const char*);
using CreateDeviceFn    = VkResult (*)(Handle, const DeviceCreateInfo*, const void*, Handle*);
using GetDeviceQueueFn  = void (*)(Handle, std::uint32_t, std::uint32_t, Handle*);
using CreateSwapchainFn = VkResult (*)(Handle, const SwapchainCreateInfo*, const void*, Handle*);
using PresentFn         = VkResult (*)(Handle, const PresentInfo*);

// The entries the design needs; the first two get detours on every address the renderer is handed.
constexpr const char* kQueried[] = {
    "vkQueuePresentKHR",     "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR",
    "vkAcquireNextImageKHR", "vkCreateDevice",       "vkGetDeviceQueue",      "vkQueueSubmit"};
constexpr int kQueriedCount = static_cast<int>(sizeof(kQueried) / sizeof(kQueried[0]));
constexpr int kQPresent = 0, kQCreateSwapchain = 1;

enum Route : int { kRouteExport = 0, kRouteInstance = 1, kRouteDevice = 2 };
constexpr const char* kRouteName[] = {"export", "instance", "device"};

constexpr int kSlots = 4;  // distinct addresses hooked per entry
constexpr int kMaxQueues = 16;
constexpr int kSummarySeconds = 10;
constexpr std::uint64_t kLogEvery = 300;
constexpr std::uint64_t kLogFirst = 5;

// ---- state ----------------------------------------------------------------------------------

std::mutex g_mu;  // the slot tables and the swapchain facts; never held across a Vulkan call or an install
void*      g_export[kQueriedCount] = {};
std::atomic<bool>          g_seen[2][kQueriedCount];  // [instance|device][entry]: first answer logged
std::atomic<std::uint64_t> g_queries[2];

struct Family {
    void* target[kSlots] = {};
    int   route[kSlots]  = {};
    int   used           = 0;
};
Family g_fam[2];  // indexed by kQPresent, kQCreateSwapchain

PresentFn         g_presentTramp[kSlots];
CreateSwapchainFn g_createTramp[kSlots];
CreateDeviceFn    g_createDeviceTramp;
GetDeviceQueueFn  g_queueTramp;
GetProcAddrFn     g_instanceTramp;
GetProcAddrFn     g_deviceTramp;

struct PresentStat {
    std::atomic<std::uint64_t> count{0};
    std::atomic<Handle>        queue{0};
    std::atomic<std::uint32_t> swapchains{0};
};
PresentStat g_pres[kSlots];

struct QueueRow {
    Handle        queue;
    Handle        device;
    std::uint32_t family;
    std::uint32_t index;
};
QueueRow         g_queues[kMaxQueues];
std::atomic<int> g_queueCount{0};

struct SwapchainFacts {
    bool          seen   = false;
    std::int32_t  format = 0;
    std::uint32_t width = 0, height = 0, minImages = 0, usage = 0;
};
SwapchainFacts             g_sc;
std::atomic<std::uint32_t> g_scCreates{0};
std::atomic<bool>          g_deviceSeen{false};

bool          g_installed = false;
std::uint64_t g_ticks     = 0;

unsigned long long U(Handle h) { return static_cast<unsigned long long>(h); }

int IsGame() { return ue_wrap::game_thread::IsGameThread() ? 1 : 0; }

// ---- the queue table ------------------------------------------------------------------------

// The family and index the queue handle was fetched with, or -1/-1 when its fetch was not seen.
void FindQueue(Handle queue, int* family, int* index) {
    *family = -1;
    *index  = -1;
    const int n = g_queueCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_queues[i].queue != queue) continue;
        *family = static_cast<int>(g_queues[i].family);
        *index  = static_cast<int>(g_queues[i].index);
        return;
    }
}

// ---- the present detours --------------------------------------------------------------------

void LogPresent(int slot, std::uint64_t n, Handle queue, const PresentInfo* info) {
    int family = -1, index = -1;
    FindQueue(queue, &family, &index);
    const std::uint32_t count = info ? info->swapchainCount : 0;
    const Handle sc0 = (info && count > 0 && info->pSwapchains) ? info->pSwapchains[0] : 0;
    const Handle sc1 = (info && count > 1 && info->pSwapchains) ? info->pSwapchains[1] : 0;
    const std::uint32_t image =
        (info && count > 0 && info->pImageIndices) ? info->pImageIndices[0] : 0xFFFFFFFFu;
    UE_LOGI("[VK-PATH] present slot=%d route=%s n=%llu queue=0x%llx family=%d index=%d swapchains=%u "
            "sc0=0x%llx sc1=0x%llx image=%u tid=%lu game_thread=%d",
            slot, kRouteName[g_fam[kQPresent].route[slot]], static_cast<unsigned long long>(n), U(queue),
            family, index, count, U(sc0), U(sc1), image, static_cast<unsigned long>(::GetCurrentThreadId()),
            IsGame());
}

template <int I>
VkResult OnPresent(Handle queue, const PresentInfo* info) {
    PresentStat& st = g_pres[I];
    const std::uint64_t n = st.count.fetch_add(1, std::memory_order_relaxed) + 1;
    st.queue.store(queue, std::memory_order_relaxed);
    st.swapchains.store(info ? info->swapchainCount : 0, std::memory_order_relaxed);
    if (n <= kLogFirst || n % kLogEvery == 0) LogPresent(I, n, queue, info);
    return g_presentTramp[I](queue, info);
}

// ---- the swapchain detours ------------------------------------------------------------------

void NoteSwapchain(int slot, Handle device, const SwapchainCreateInfo* ci, VkResult result, Handle made) {
    if (!ci) return;
    const std::uint32_t nth = g_scCreates.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_sc.seen      = true;
        g_sc.format    = ci->imageFormat;
        g_sc.width     = ci->width;
        g_sc.height    = ci->height;
        g_sc.minImages = ci->minImageCount;
        g_sc.usage     = ci->imageUsage;
    }
    UE_LOGI("[VK-PATH] swapchain create #%u slot=%d route=%s result=%d device=0x%llx handle=0x%llx "
            "old=0x%llx surface=0x%llx fmt=%d colorspace=%d extent=%ux%u minImages=%u usage=0x%x "
            "color_attachment=%s present_mode=%d sharing=%d layers=%u tid=%lu game_thread=%d",
            nth, slot, kRouteName[g_fam[kQCreateSwapchain].route[slot]], result, U(device), U(made),
            U(ci->oldSwapchain), U(ci->surface), ci->imageFormat, ci->imageColorSpace, ci->width, ci->height,
            ci->minImageCount, ci->imageUsage, (ci->imageUsage & kUsageColorAttachment) ? "yes" : "no",
            ci->presentMode, ci->imageSharingMode, ci->imageArrayLayers,
            static_cast<unsigned long>(::GetCurrentThreadId()), IsGame());
}

template <int I>
VkResult OnCreateSwapchain(Handle device, const SwapchainCreateInfo* ci, const void* alloc, Handle* out) {
    const VkResult r = g_createTramp[I](device, ci, alloc, out);
    NoteSwapchain(I, device, ci, r, (r == 0 && out) ? *out : 0);
    return r;
}

// ---- the device detours ---------------------------------------------------------------------

VkResult OnCreateDevice(Handle physical, const DeviceCreateInfo* ci, const void* alloc, Handle* out) {
    if (ci) {
        UE_LOGI("[VK-PATH] vkCreateDevice queue_infos=%u extensions=%u layers=%u tid=%lu game_thread=%d",
                ci->queueCreateInfoCount, ci->enabledExtensionCount, ci->enabledLayerCount,
                static_cast<unsigned long>(::GetCurrentThreadId()), IsGame());
        for (std::uint32_t i = 0; ci->pQueueCreateInfos && i < ci->queueCreateInfoCount && i < kMaxQueues; ++i)
            UE_LOGI("[VK-PATH] vkCreateDevice queue family=%u count=%u", ci->pQueueCreateInfos[i].queueFamilyIndex,
                    ci->pQueueCreateInfos[i].queueCount);
        for (std::uint32_t i = 0; ci->ppEnabledExtensionNames && i < ci->enabledExtensionCount && i < 128; ++i)
            UE_LOGI("[VK-PATH] vkCreateDevice extension[%u]=%s", i,
                    ci->ppEnabledExtensionNames[i] ? ci->ppEnabledExtensionNames[i] : "(null)");
    }
    const VkResult r = g_createDeviceTramp(physical, ci, alloc, out);
    g_deviceSeen.store(true, std::memory_order_relaxed);
    UE_LOGI("[VK-PATH] vkCreateDevice result=%d device=0x%llx", r, U((r == 0 && out) ? *out : 0));
    return r;
}

void OnGetDeviceQueue(Handle device, std::uint32_t family, std::uint32_t index, Handle* out) {
    g_queueTramp(device, family, index, out);
    const Handle queue = out ? *out : 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const int n = g_queueCount.load(std::memory_order_relaxed);
        if (n < kMaxQueues) {
            g_queues[n] = QueueRow{queue, device, family, index};
            g_queueCount.store(n + 1, std::memory_order_release);
        }
    }
    UE_LOGI("[VK-PATH] vkGetDeviceQueue device=0x%llx family=%u index=%u queue=0x%llx", U(device), family, index,
            U(queue));
}

// ---- hooking an address ---------------------------------------------------------------------

void* const kPresentDetours[kSlots] = {
    reinterpret_cast<void*>(&OnPresent<0>), reinterpret_cast<void*>(&OnPresent<1>),
    reinterpret_cast<void*>(&OnPresent<2>), reinterpret_cast<void*>(&OnPresent<3>)};
void* const kCreateDetours[kSlots] = {
    reinterpret_cast<void*>(&OnCreateSwapchain<0>), reinterpret_cast<void*>(&OnCreateSwapchain<1>),
    reinterpret_cast<void*>(&OnCreateSwapchain<2>), reinterpret_cast<void*>(&OnCreateSwapchain<3>)};

// Hook `target` once for entry `q` (kQPresent or kQCreateSwapchain), each distinct address on a slot
// of its own so the trampolines never mix. A second sight of an address is silent. Any thread.
void HookFamily(int q, void* target, int route) {
    int slot = -1;
    bool full = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Family& f = g_fam[q];
        for (int i = 0; i < f.used; ++i)
            if (f.target[i] == target) return;
        if (f.used >= kSlots) {
            full = true;
        } else {
            slot = f.used++;
            f.target[slot] = target;
            f.route[slot]  = route;
        }
    }
    if (full) {
        UE_LOGW("[VK-PATH] hook %s: no free slot for %p (route=%s)", kQueried[q], target, kRouteName[route]);
        return;
    }
    void* detour = q == kQPresent ? kPresentDetours[slot] : kCreateDetours[slot];
    void** tramp = q == kQPresent ? reinterpret_cast<void**>(&g_presentTramp[slot])
                                  : reinterpret_cast<void**>(&g_createTramp[slot]);
    const bool ok = ue_wrap::hook::Install(target, detour, tramp);
    UE_LOGI("[VK-PATH] hook %s slot=%d route=%s target=%p %s", kQueried[q], slot, kRouteName[route], target,
            ok ? "installed" : "FAILED");
}

// A name the renderer asked the loader for, and what it was told. Any thread.
void OnProcQuery(int via, const char* name, PfnVoid answer) {
    const int side = via == kRouteInstance ? 0 : 1;
    g_queries[side].fetch_add(1, std::memory_order_relaxed);
    if (!name || name[0] != 'v' || name[1] != 'k') return;
    for (int i = 0; i < kQueriedCount; ++i) {
        if (std::strcmp(name, kQueried[i]) != 0) continue;
        void* ptr = reinterpret_cast<void*>(answer);
        const bool same = ptr == g_export[i];
        if (!g_seen[side][i].exchange(true))
            UE_LOGI("[VK-PATH] proc-addr via=%s name=%s answer=%p export=%p %s tid=%lu game_thread=%d",
                    kRouteName[via], name, ptr, g_export[i], same ? "same-as-export" : "DIFFERS-from-export",
                    static_cast<unsigned long>(::GetCurrentThreadId()), IsGame());
        if (ptr && !same && (i == kQPresent || i == kQCreateSwapchain)) HookFamily(i, ptr, via);
        return;
    }
}

PfnVoid OnGetInstanceProcAddr(Handle instance, const char* name) {
    const PfnVoid p = g_instanceTramp(instance, name);
    OnProcQuery(kRouteInstance, name, p);
    return p;
}

PfnVoid OnGetDeviceProcAddr(Handle device, const char* name) {
    const PfnVoid p = g_deviceTramp(device, name);
    OnProcQuery(kRouteDevice, name, p);
    return p;
}

void HookExport(HMODULE mod, const char* name, void* detour, void** tramp) {
    void* addr = reinterpret_cast<void*>(::GetProcAddress(mod, name));
    if (!addr) {
        UE_LOGW("[VK-PATH] export %s missing", name);
        return;
    }
    const bool ok = ue_wrap::hook::Install(addr, detour, tramp);
    UE_LOGI("[VK-PATH] hook export %s target=%p %s", name, addr, ok ? "installed" : "FAILED");
}

void InstallAll(HMODULE mod) {
    g_installed = true;  // latched first: a failed install is not retried every tick
    UE_LOGI("[VK-PATH] vulkan-1.dll mapped at %p, first seen on probe tick %llu, game window subclassed=%d", mod,
            static_cast<unsigned long long>(g_ticks), coop::input::game_window_proc::Hwnd() ? 1 : 0);
    if (!ue_wrap::hook::Init()) {
        UE_LOGE("[VK-PATH] hook engine unavailable -- probe idle");
        return;
    }
    for (int i = 0; i < kQueriedCount; ++i) {
        g_export[i] = reinterpret_cast<void*>(::GetProcAddress(mod, kQueried[i]));
        UE_LOGI("[VK-PATH] export %s=%p", kQueried[i], g_export[i]);
    }
    if (g_export[kQPresent]) HookFamily(kQPresent, g_export[kQPresent], kRouteExport);
    if (g_export[kQCreateSwapchain]) HookFamily(kQCreateSwapchain, g_export[kQCreateSwapchain], kRouteExport);
    HookExport(mod, "vkCreateDevice", reinterpret_cast<void*>(&OnCreateDevice),
               reinterpret_cast<void**>(&g_createDeviceTramp));
    HookExport(mod, "vkGetDeviceQueue", reinterpret_cast<void*>(&OnGetDeviceQueue),
               reinterpret_cast<void**>(&g_queueTramp));
    HookExport(mod, "vkGetDeviceProcAddr", reinterpret_cast<void*>(&OnGetDeviceProcAddr),
               reinterpret_cast<void**>(&g_deviceTramp));
    HookExport(mod, "vkGetInstanceProcAddr", reinterpret_cast<void*>(&OnGetInstanceProcAddr),
               reinterpret_cast<void**>(&g_instanceTramp));
}

// ---- the summary ----------------------------------------------------------------------------

struct SummaryState {
    bool              started = false;
    bool              done    = false;
    int               printed = 0;
    Clock::time_point next{};
    std::uint64_t     prev[kSlots] = {};
};
SummaryState g_sum;

void Summary() {
    if (g_sum.done) return;
    std::uint64_t counts[kSlots];
    std::uint64_t total = 0;
    for (int i = 0; i < kSlots; ++i) {
        counts[i] = g_pres[i].count.load(std::memory_order_relaxed);
        total += counts[i];
    }
    const Clock::time_point now = Clock::now();
    if (!g_sum.started) {
        if (total == 0) return;
        g_sum.started = true;
        g_sum.next    = now + std::chrono::seconds(1);
        for (int i = 0; i < kSlots; ++i) g_sum.prev[i] = counts[i];
        UE_LOGI("[VK-PATH] first present seen on probe tick %llu, device create seen=%d, swapchain creates seen=%u",
                static_cast<unsigned long long>(g_ticks), g_deviceSeen.load() ? 1 : 0, g_scCreates.load());
        return;
    }
    if (now < g_sum.next) return;
    g_sum.next += std::chrono::seconds(1);

    SwapchainFacts sc;
    int routes[kSlots];
    {
        std::lock_guard<std::mutex> lk(g_mu);
        sc = g_sc;
        for (int i = 0; i < kSlots; ++i) routes[i] = g_fam[kQPresent].route[i];
    }
    for (int i = 0; i < kSlots; ++i) {
        if (counts[i] == 0) continue;
        const std::uint64_t perSecond = counts[i] - g_sum.prev[i];
        g_sum.prev[i] = counts[i];
        if (sc.seen) {
            UE_LOGI("[VK-PATH] presents=%llu/s route=%s queue=0x%llx swapchains=%u fmt=%d extent=%ux%u "
                    "images=%u usage=0x%x",
                    static_cast<unsigned long long>(perSecond), kRouteName[routes[i]], U(g_pres[i].queue.load()),
                    g_pres[i].swapchains.load(), sc.format, sc.width, sc.height, sc.minImages, sc.usage);
        } else {
            UE_LOGI("[VK-PATH] presents=%llu/s route=%s queue=0x%llx swapchains=%u fmt=unseen extent=unseen "
                    "images=unseen usage=unseen",
                    static_cast<unsigned long long>(perSecond), kRouteName[routes[i]], U(g_pres[i].queue.load()),
                    g_pres[i].swapchains.load());
        }
    }
    if (++g_sum.printed < kSummarySeconds) return;
    g_sum.done = true;
    UE_LOGI("[VK-PATH] DONE presents_total=%llu swapchain_creates=%u device_create_seen=%d queues_seen=%d "
            "instance_proc_queries=%llu device_proc_queries=%llu",
            static_cast<unsigned long long>(total), g_scCreates.load(), g_deviceSeen.load() ? 1 : 0,
            g_queueCount.load(), static_cast<unsigned long long>(g_queries[0].load()),
            static_cast<unsigned long long>(g_queries[1].load()));
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::vulkan_path_probe);
    if (!s_on) return;
    ++g_ticks;
    if (!g_installed) {
        HMODULE mod = ::GetModuleHandleW(L"vulkan-1.dll");
        if (!mod) return;
        InstallAll(mod);
    }
    Summary();
}

}  // namespace coop::dev::vulkan_path_probe

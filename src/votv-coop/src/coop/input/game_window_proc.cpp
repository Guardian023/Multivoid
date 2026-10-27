// coop/input/game_window_proc.cpp -- see coop/input/game_window_proc.h.

#include "coop/input/game_window_proc.h"

#include "coop/session/shutdown.h"
#include "ue_wrap/core/log.h"
#include "ui/imgui_overlay.h"

#include <windows.h>

#include <atomic>
#include <cwchar>

namespace coop::input::game_window_proc {
namespace {

// The window and the procedure it had before ours. Atomics, so the window procedure reads them
// without a lock: a mutex there would contend with every message of the UI thread. Tick is the
// only writer, on the game thread.
std::atomic<HWND>    g_hwnd{nullptr};
std::atomic<WNDPROC> g_origProc{nullptr};

// Tick's pacing while no window is held; the game thread only.
ULONGLONG g_nextFindMs = 0;
constexpr ULONGLONG kFindEveryMs = 250;

// The best candidate among the top-level visible windows of class UnrealWindow that belong to
// THIS process, by client area. UE4.27 destroys and recreates the main game window during world
// load (the splash-to-level transition applies the user settings); most runs have only one such
// window at a time, so the pick is unambiguous.
struct EnumCtx { DWORD pid; HWND best; long long bestArea; };
BOOL CALLBACK EnumCb(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (!::IsWindowVisible(h)) return TRUE;
    if (::GetWindow(h, GW_OWNER) != nullptr) return TRUE;  // popup/owned
    wchar_t cls[64] = {};
    if (::GetClassNameW(h, cls, 64) <= 0) return TRUE;
    if (::wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    RECT r{};
    if (!::GetClientRect(h, &r)) return TRUE;
    const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}
HWND FindBestUnrealWindow() {
    EnumCtx ctx{::GetCurrentProcessId(), nullptr, 0};
    ::EnumWindows(&EnumCb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// The overlay is consulted first and the close handling second: with the close handler installed
// at boot and the overlay's hook on the first present, the overlay sat on top of the close
// handler in a running game. Nothing the overlay consumes is a close message (it takes keys, the
// mouse and the cursor), so a close signal reaches the shutdown handler either way.
LRESULT CALLBACK GameWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT swallowed = 0;
    if (ui::imgui_overlay::HandleWindowMessage(hwnd, msg, wp, lp, swallowed)) return swallowed;

    coop::shutdown::OnWindowMessage(hwnd, msg, wp);

    WNDPROC orig = g_origProc.load(std::memory_order_acquire);
    const LRESULT result = orig ? ::CallWindowProcW(orig, hwnd, msg, wp, lp)
                                : ::DefWindowProcW(hwnd, msg, wp, lp);

    coop::shutdown::OnWindowMessageForwarded(msg, result);
    return result;
}

void Subclass(HWND best) {
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWindowProc)));
    if (!orig) {
        UE_LOGW("game_window_proc: SetWindowLongPtrW failed on HWND=%p err=%lu", best,
                ::GetLastError());
        return;
    }
    // origProc first: our procedure is already the one called for `best`, and it forwards to
    // whatever origProc holds when a message arrives between the two stores.
    g_origProc.store(orig, std::memory_order_release);
    g_hwnd.store(best, std::memory_order_release);

    wchar_t title[128] = {};
    ::GetWindowTextW(best, title, 128);
    RECT r{};
    ::GetWindowRect(best, &r);
    UE_LOGI("game_window_proc: subclassed HWND=%p title='%ls' rect=(%ld,%ld %ldx%ld) origProc=%p",
            best, title, r.left, r.top, r.right - r.left, r.bottom - r.top, orig);
}

}  // namespace

void Tick() {
    HWND held = g_hwnd.load(std::memory_order_acquire);
    if (held) {
        if (::IsWindow(held)) return;
        // A destroyed window takes its subclass with it, so there is nothing to restore.
        UE_LOGI("game_window_proc: HWND=%p was destroyed -- looking for its successor", held);
        g_hwnd.store(nullptr, std::memory_order_release);
        g_nextFindMs = 0;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now < g_nextFindMs) return;
    g_nextFindMs = now + kFindEveryMs;
    if (HWND best = FindBestUnrealWindow()) Subclass(best);
}

void* Hwnd() {
    return g_hwnd.load(std::memory_order_acquire);
}

}  // namespace coop::input::game_window_proc

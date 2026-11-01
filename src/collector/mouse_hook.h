#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <thread>
#include "core/counters.h"

namespace mouse {
struct HookInput { HWND foreground{}; DWORD timestamp{}; uint16_t flags{}; int16_t wheel{}; POINT position{}; };
// The callback does no allocation, application lookup, disk access or counting.
// Its dedicated thread forwards button events through a bounded SPSC queue.
class MouseHook {
public:
    explicit MouseHook(HWND target, UINT message) : thread_([this, target, message] { Run(target, message); }) {}
    ~MouseHook() { Stop(); }
    void Stop() {
        while (thread_.joinable() && !ready_.load(std::memory_order_acquire)) std::this_thread::yield();
        if (auto id = threadId_.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
        if (thread_.joinable()) thread_.join();
    }
    bool Active() const { return active_.load(); }
    DWORD Error() const { return error_.load(); }
    unsigned Dropped() const { return dropped_.load(); }
    bool Pop(HookInput& event) {
        auto read = read_.load(std::memory_order_relaxed);
        if (read == written_.load(std::memory_order_acquire)) return false;
        event = queue_[read]; read_.store((read + 1) % queue_.size(), std::memory_order_release); return true;
    }
private:
    static LRESULT CALLBACK Callback(int code, WPARAM message, LPARAM data) {
        if (code >= 0 && current_) {
            uint16_t flags = 0;
            const auto& input = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            if (!(input.flags & LLMHF_INJECTED)) switch (message) {
            case WM_LBUTTONDOWN: flags = kLeftDown; break;
            case WM_RBUTTONDOWN: flags = kRightDown; break;
            case WM_MBUTTONDOWN: flags = kMiddleDown; break;
            case WM_XBUTTONDOWN: flags = HIWORD(input.mouseData) == XBUTTON1 ? kX1Down : kX2Down; break;
            }
            if (flags) {
                auto* self = current_;
                const auto write = self->written_.load(std::memory_order_relaxed);
                const auto next = (write + 1) % self->queue_.size();
                if (next != self->read_.load(std::memory_order_acquire)) {
                    self->queue_[write] = {GetForegroundWindow(), input.time, flags,
                        static_cast<int16_t>((flags & (kWheel | kHWheel)) ? HIWORD(input.mouseData) : 0), input.pt};
                    self->written_.store(next, std::memory_order_release);
                    PostMessageW(self->target_, self->message_, 0, 0);
                } else ++self->dropped_;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    void Run(HWND target, UINT message) {
        current_ = this; target_ = target; message_ = message;
        MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        auto hook = SetWindowsHookExW(WH_MOUSE_LL, Callback, GetModuleHandleW(nullptr), 0);
        if (!hook) error_ = GetLastError();
        active_ = hook != nullptr; threadId_ = GetCurrentThreadId(); ready_.store(true, std::memory_order_release);
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (hook) UnhookWindowsHookEx(hook);
        active_ = false; threadId_ = 0; current_ = nullptr;
    }
    inline static thread_local MouseHook* current_ = nullptr;
    std::array<HookInput, 4096> queue_{};
    std::atomic<size_t> read_{0}, written_{0};
    std::atomic<DWORD> threadId_{0}, error_{0};
    std::atomic<unsigned> dropped_{0};
    std::atomic<bool> ready_{false}, active_{false};
    HWND target_{}; UINT message_{};
    std::thread thread_;
};
} // namespace mouse

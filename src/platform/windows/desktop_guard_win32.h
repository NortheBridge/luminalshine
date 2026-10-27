/**
 * @file src/platform/windows/desktop_guard_win32.h
 * @brief Message handling shared by the interactive desktop guard and its Windows tests.
 */
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <cstddef>
#include <optional>

namespace platf::desktop_guard {
  inline constexpr DWORD cover_extended_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

  inline DWORD wait_for_work(HANDLE wake_event, DWORD timeout = INFINITE) {
    // MWMO_INPUTAVAILABLE can report queued input ahead of a signaled event.
    // Give configuration/shutdown priority even under continuous window events.
    const DWORD signal = WaitForSingleObject(wake_event, 0);
    if (signal != WAIT_TIMEOUT) {
      return signal;
    }
    // USER32 calls may mark queued messages as observed without removing them.
    // They must still wake the worker, including the initial window sweep.
    return MsgWaitForMultipleObjectsEx(1, &wake_event, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }

  inline bool drain_messages(std::size_t limit = 256) {
    MSG msg {};
    for (std::size_t count = 0; count < limit && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++count) {
      if (msg.message == WM_QUIT) {
        return false;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return true;
  }

  inline std::optional<LRESULT> cover_input_result(UINT message) {
    switch (message) {
      case WM_NCHITTEST:
        // The recovery display is intentionally covered. Do not forward hover
        // or clicks to hidden applications, or let their cursors light it up.
        return HTCLIENT;
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATEANDEAT;
      case WM_SETCURSOR:
        SetCursor(nullptr);
        return TRUE;
      default:
        return std::nullopt;
    }
  }
}  // namespace platf::desktop_guard
#endif

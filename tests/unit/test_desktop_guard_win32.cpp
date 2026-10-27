/**
 * @file tests/unit/test_desktop_guard_win32.cpp
 * @brief Native USER32 regression tests for the exact-display desktop guard.
 */
#ifdef _WIN32
#include "src/platform/windows/desktop_guard_win32.h"

#include <gtest/gtest.h>

namespace {
  constexpr UINT route_message = WM_APP + 71;

  class DesktopGuardWin32: public testing::Test {
  protected:
    HANDLE wake {};
    HWND window {};
    unsigned routed {};

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
      if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      }
      if (const auto result = platf::desktop_guard::cover_input_result(message)) {
        return *result;
      }
      if (message == route_message) {
        auto *self = reinterpret_cast<DesktopGuardWin32 *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        ++self->routed;
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void SetUp() override {
      wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      ASSERT_NE(wake, nullptr);
      WNDCLASSW wc {};
      wc.lpfnWndProc = procedure;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"LuminalShineDesktopGuardTest";
      ASSERT_NE(RegisterClassW(&wc), 0);
      window = CreateWindowExW(platf::desktop_guard::cover_extended_style,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, wc.hInstance, this);
      ASSERT_NE(window, nullptr);
      ASSERT_TRUE(platf::desktop_guard::drain_messages());
    }

    void TearDown() override {
      if (window) {
        DestroyWindow(window);
      }
      UnregisterClassW(L"LuminalShineDesktopGuardTest", GetModuleHandleW(nullptr));
      if (wake) {
        CloseHandle(wake);
      }
      platf::desktop_guard::drain_messages();
    }
  };

  TEST_F(DesktopGuardWin32, PreviouslyObservedRouteMessagesStillWakeWorker) {
    ASSERT_TRUE(PostMessageW(window, route_message, 0, 0));
    MSG pending {};
    ASSERT_TRUE(PeekMessageW(&pending, window, route_message, route_message, PM_NOREMOVE));
    // Match USER32 marking input as seen during configuration work. The old
    // MsgWaitForMultipleObjects call sleeps despite this pending route request.
    GetQueueStatus(QS_ALLINPUT);
    EXPECT_EQ(platf::desktop_guard::wait_for_work(wake, 0), WAIT_OBJECT_0 + 1);
    EXPECT_TRUE(platf::desktop_guard::drain_messages());
    EXPECT_EQ(routed, 1u);
  }

  TEST_F(DesktopGuardWin32, ConfigurationWakeAlsoDrainsRoutingWork) {
    ASSERT_TRUE(PostMessageW(window, route_message, 0, 0));
    ASSERT_TRUE(SetEvent(wake));
    EXPECT_EQ(platf::desktop_guard::wait_for_work(wake, 0), WAIT_OBJECT_0);
    ASSERT_TRUE(ResetEvent(wake));
    EXPECT_TRUE(platf::desktop_guard::drain_messages());
    EXPECT_EQ(routed, 1u);
  }

  TEST_F(DesktopGuardWin32, BoundedDrainLeavesRemainingWorkWakeable) {
    for (unsigned count = 0; count < 300; ++count) {
      ASSERT_TRUE(PostMessageW(window, route_message, 0, 0));
    }
    EXPECT_TRUE(platf::desktop_guard::drain_messages());
    EXPECT_GT(routed, 0u);
    EXPECT_LE(routed, 256u);
    EXPECT_EQ(platf::desktop_guard::wait_for_work(wake, 0), WAIT_OBJECT_0 + 1);
    EXPECT_TRUE(platf::desktop_guard::drain_messages());
    EXPECT_EQ(routed, 300u);
  }

  TEST_F(DesktopGuardWin32, QuitTerminatesMessageDrain) {
    PostQuitMessage(0);
    EXPECT_FALSE(platf::desktop_guard::drain_messages());
  }

  TEST_F(DesktopGuardWin32, CoverConsumesHitTestsAndDoesNotActivateOnClick) {
    EXPECT_EQ(SendMessageW(window, WM_NCHITTEST, 0, 0), HTCLIENT);
    EXPECT_EQ(SendMessageW(window, WM_MOUSEACTIVATE, 0, 0), MA_NOACTIVATEANDEAT);
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    EXPECT_EQ(style & WS_EX_TRANSPARENT, 0);
    EXPECT_NE(style & WS_EX_NOACTIVATE, 0);
  }

  TEST_F(DesktopGuardWin32, HoveringCoverHidesCursor) {
    const HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_ARROW));
    EXPECT_EQ(SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)), TRUE);
    EXPECT_EQ(GetCursor(), nullptr);
    SetCursor(previous);
  }
}  // namespace
#endif

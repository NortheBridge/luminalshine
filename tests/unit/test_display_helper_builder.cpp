/**
 * @file tests/unit/test_display_helper_builder.cpp
 * @brief Regression coverage for provenance-preserving helper requests.
 */
#include "src/display_helper_builder.h"
#include "src/rtsp.h"

#include <gtest/gtest.h>

namespace {
  using display_helper_integration::DisplayApplyBuilder;

  TEST(DisplayHelperBuilder, CopiesExactVirtualIdentityVerbatim) {
    rtsp_stream::launch_session_t session {};
    session.virtual_display = true;
    session.virtual_display_device_id = R"(\\?\DISPLAY#NBF5001#client-a)";

    const auto request = DisplayApplyBuilder {}.set_session(session).build();

    ASSERT_TRUE(request.exact_virtual_display_device_id);
    EXPECT_EQ(*request.exact_virtual_display_device_id, session.virtual_display_device_id);
  }

  TEST(DisplayHelperBuilder, DoesNotInventExactVirtualIdentity) {
    EXPECT_FALSE(DisplayApplyBuilder {}.build().exact_virtual_display_device_id);

    rtsp_stream::launch_session_t physical_session {};
    physical_session.virtual_display = false;
    physical_session.virtual_display_device_id = "stale-vgd";
    EXPECT_FALSE(DisplayApplyBuilder {}.set_session(physical_session).build().exact_virtual_display_device_id);

    rtsp_stream::launch_session_t unnamed_virtual_session {};
    unnamed_virtual_session.virtual_display = true;
    EXPECT_FALSE(DisplayApplyBuilder {}.set_session(unnamed_virtual_session).build().exact_virtual_display_device_id);
  }
}  // namespace

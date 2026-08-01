/**
 * @file tests/unit/test_virtual_display_client_label.cpp
 * @brief The EDID product-name ceiling, and why matching a client label
 *        against a monitor name cannot be plain equality.
 *
 * A virtual-display driver can only publish a client's label through the
 * EDID display-product-name descriptor, which holds 13 bytes. Anything
 * longer reaches Windows truncated, so the host's "find the monitor this
 * client's session created" resolver compared a 13-character monitor name
 * against the full label and never matched. The failure was silent and
 * length-dependent: 'macOS' (5) worked, so the resolver looked healthy,
 * while 'LG C2 83" OLED webOS' (20) and 'XBOXONE Series X' (16) could
 * never resolve — and for those clients an arrived-but-inactive monitor
 * was reported missing and destroyed, on a loop.
 *
 * Pure predicate, no device I/O, so this behaves identically on CI and on
 * driver dev boxes.
 */
#ifdef _WIN32

  // standard includes
  #include <string>

  // lib includes
  #include <gtest/gtest.h>

  // local includes
  #include "../tests_common.h"

  #include "src/platform/windows/virtual_display.h"

namespace {

  // The three real client labels this box streams to, and what the driver
  // can actually publish for each.
  constexpr const char *kLgLabel = "LG C2 83\" OLED webOS";  // 20 chars
  constexpr const char *kLgPublished = "LG C2 83\" OLE";  // 13 chars
  constexpr const char *kXboxLabel = "XBOXONE Series X";  // 16 chars
  constexpr const char *kXboxPublished = "XBOXONE Serie";  // 13 chars
  constexpr const char *kMacLabel = "macOS";  // 5 chars, fits

}  // namespace

TEST(VirtualDisplayClientLabel, TruncatedLabelsMatchTheirClient) {
  EXPECT_TRUE(VDISPLAY::is_truncated_client_label(kLgPublished, kLgLabel));
  EXPECT_TRUE(VDISPLAY::is_truncated_client_label(kXboxPublished, kXboxLabel));
}

TEST(VirtualDisplayClientLabel, MatchIsCaseInsensitiveLikeTheExactComparison) {
  EXPECT_TRUE(VDISPLAY::is_truncated_client_label("lg c2 83\" ole", kLgLabel));
}

TEST(VirtualDisplayClientLabel, LabelsThatFitAreNeverTreatedAsTruncated) {
  // 'macOS' is published in full, so equality already decides it and a
  // prefix match must not add a second, weaker answer.
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label(kMacLabel, kMacLabel));
  // Exactly at the ceiling, published in full: still not a truncation.
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label("XBOXONE Serie", "XBOXONE Serie"));
}

TEST(VirtualDisplayClientLabel, OnlyASaturatedMonitorNameCanMatchAsAPrefix) {
  // A short monitor name that happens to prefix a long client label was
  // published in full and genuinely belongs to a different client.
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label("LG", kLgLabel));
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label("LG C2 83\" OL", kLgLabel));
}

TEST(VirtualDisplayClientLabel, DifferentClientsDoNotCrossMatch) {
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label(kXboxPublished, kLgLabel));
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label(kLgPublished, kXboxLabel));
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label(kLgPublished, kMacLabel));
}

TEST(VirtualDisplayClientLabel, EmptyInputsNeverMatch) {
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label("", kLgLabel));
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label(kLgPublished, ""));
  EXPECT_FALSE(VDISPLAY::is_truncated_client_label("", ""));
}

TEST(VirtualDisplayClientLabel, CeilingMatchesTheEdidDescriptorSize) {
  // If this ever changes, every truncated match silently stops working —
  // the number is fixed by the EDID structure, not by a driver choice.
  EXPECT_EQ(VDISPLAY::kEdidProductNameCapacity, 13u);
  EXPECT_EQ(std::string(kLgPublished).size(), VDISPLAY::kEdidProductNameCapacity);
  EXPECT_EQ(std::string(kXboxPublished).size(), VDISPLAY::kEdidProductNameCapacity);
}

#endif  // _WIN32

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "vgd_transition.h"

namespace platf::video_worker {
  // Fixed-size sideband payload; no native pointers or process-local clocks.
  struct capture_status_t {
    char kind[16];
    char display[64];
  };
  static_assert(sizeof(capture_status_t) == 80);
  static_assert(std::is_trivially_copyable_v<capture_status_t>);

  inline bool valid_capture_kind(std::string_view kind) {
    return kind == vgd_transition::kCaptureKindVgdRing ||
           kind == vgd_transition::kCaptureKindWgc ||
           kind == vgd_transition::kCaptureKindDda;
  }

  inline std::optional<capture_status_t> capture_status_update(
    const vgd_transition::capture_note &note,
    std::uint64_t sent_sequence
  ) {
    if (note.sequence == 0 || note.sequence == sent_sequence || !valid_capture_kind(note.kind)) {
      return std::nullopt;
    }
    capture_status_t status {};
    if (note.display.size() >= sizeof(status.display) || note.display.find('\0') != std::string::npos) {
      return std::nullopt;
    }
    std::copy(note.kind.begin(), note.kind.end(), status.kind);
    std::copy(note.display.begin(), note.display.end(), status.display);
    return status;
  }

  inline std::optional<vgd_transition::capture_note> decode_capture_status(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != sizeof(capture_status_t)) {
      return std::nullopt;
    }
    capture_status_t status {};
    std::memcpy(&status, bytes.data(), sizeof(status));
    const auto kind_end = std::find(std::begin(status.kind), std::end(status.kind), '\0');
    const auto display_end = std::find(std::begin(status.display), std::end(status.display), '\0');
    if (kind_end == std::end(status.kind) || display_end == std::end(status.display)) {
      return std::nullopt;
    }
    vgd_transition::capture_note note {};
    note.kind.assign(status.kind, kind_end);
    note.display.assign(status.display, display_end);
    if (!valid_capture_kind(note.kind)) {
      return std::nullopt;
    }
    return note;
  }
}

#pragma once

#include <atomic>
#include <ffnvcodec/nvEncodeAPI.h>

namespace nvenc {
  // A transport failure must not become a claim that the GPU lacks AV1.
  // Keep a process-lifetime compatibility choice; a restart retries D3D12.
  class av1_compatibility {
  public:
    bool note_lock_failure(int codec, bool produced_frame, NVENCSTATUS status) {
      if (codec != 2 || produced_frame || status != NV_ENC_ERR_INVALID_PARAM) {
        return false;
      }
      return !d3d11_.exchange(true);
    }

    bool use_d3d11(int codec, bool parent_requested = false) const {
      return codec == 2 && (parent_requested || d3d11_.load());
    }

  private:
    std::atomic_bool d3d11_ {false};
  };

  inline av1_compatibility &av1_compatibility_state() {
    static av1_compatibility state;
    return state;
  }
}

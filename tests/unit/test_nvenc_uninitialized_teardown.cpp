/**
 * @file tests/unit/test_nvenc_uninitialized_teardown.cpp
 * @brief Teardown of an NVENC session that was opened but never initialized.
 *
 * Regression coverage for issue #147. nvenc_base::create_encoder() opens an
 * encode session before it knows whether the GPU supports the requested
 * codec. When a capability check then rejects the request -- AV1 on Ampere,
 * HEVC on pre-Kepler parts, 10-bit on Pascal, an oversized resolution, or the
 * YUV444 rejection the encoder probe provokes deliberately on every run —
 * create_encoder returns early with the session open and *nothing registered
 * against it*.
 *
 * The general destroy_encoder() sequence is invalid for that state: it calls
 * nvEncUnregisterAsyncEvent() on a handle that never registered an async
 * event. async_event_handle is created by the nvenc_d3d11 constructor, which
 * nvenc_d3d12 inherits, so this fired on the native D3D12 path too. Some
 * NVIDIA driver branches answer that invalid sequence with a non-C++
 * exception; because ~FailGuard is noexcept it became std::terminate and took
 * the whole process down instead of just failing the probe.
 *
 * These tests pin the distinction between the three teardowns and need no
 * GPU: the NVENC entry points are stubbed and only record that they ran.
 */

// standard includes
#include <cstdint>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/nvenc/nvenc_base.h"

namespace {

  /// Recorded NVENC entry-point traffic for one test.
  struct call_log_t {
    int destroy_encoder = 0;
    int unregister_async_event = 0;
    int unregister_resource = 0;
  };

  call_log_t g_calls;

  NVENCSTATUS NVENCAPI stub_destroy_encoder(void *) {
    ++g_calls.destroy_encoder;
    return NV_ENC_SUCCESS;
  }

  NVENCSTATUS NVENCAPI stub_unregister_async_event(void *, NV_ENC_EVENT_PARAMS *) {
    ++g_calls.unregister_async_event;
    return NV_ENC_SUCCESS;
  }

  NVENCSTATUS NVENCAPI stub_unregister_resource(void *, NV_ENC_REGISTERED_PTR) {
    ++g_calls.unregister_resource;
    return NV_ENC_SUCCESS;
  }

  /// A minimal concrete nvenc_base that talks to the stubs above.
  class stub_nvenc: public nvenc::nvenc_base {
  public:
    stub_nvenc():
        nvenc_base(NV_ENC_DEVICE_TYPE_DIRECTX) {
      auto fns = std::make_shared<NV_ENCODE_API_FUNCTION_LIST>();
      *fns = {};
      fns->nvEncDestroyEncoder = stub_destroy_encoder;
      fns->nvEncUnregisterAsyncEvent = stub_unregister_async_event;
      fns->nvEncUnregisterResource = stub_unregister_resource;
      nvenc = std::move(fns);
    }

    /// Put the object into the state create_encoder() leaves behind when a
    /// capability check rejects the request before NvEncInitializeEncoder.
    void simulate_opened_session() {
      encoder = reinterpret_cast<void *>(0xDEADBEEF);
      async_event_handle = reinterpret_cast<void *>(0xFEEDFACE);
      encoder_initialized = false;
    }

    /// Put the object into the state a live, fully-registered session has.
    void simulate_live_session() {
      simulate_opened_session();
      encoder_initialized = true;
    }

    bool is_initialized() const {
      return encoder_initialized;
    }

    bool has_encoder() const {
      return encoder != nullptr;
    }

    using nvenc_base::cleanup_rejected_initialize;
    using nvenc_base::destroy_encoder;
    using nvenc_base::discard_uninitialized_session;

  private:
    bool init_library(uint32_t) override {
      return true;
    }

    bool create_and_register_input_buffer() override {
      return true;
    }
  };

  class NvencUninitializedTeardown: public ::testing::Test {
  protected:
    void SetUp() override {
      g_calls = {};
    }
  };

  TEST_F(NvencUninitializedTeardown, DiscardSkipsTheUnregisterCallsAndClosesTheHandle) {
    // The whole point: an opened-but-uninitialized session must be closed
    // with nvEncDestroyEncoder() alone. Unregistering an async event that was
    // never registered is what the driver faulted on.
    stub_nvenc enc;
    enc.simulate_opened_session();

    enc.discard_uninitialized_session();

    EXPECT_EQ(g_calls.unregister_async_event, 0);
    EXPECT_EQ(g_calls.unregister_resource, 0);
    EXPECT_EQ(g_calls.destroy_encoder, 1);
    EXPECT_FALSE(enc.has_encoder());
    EXPECT_FALSE(enc.is_initialized());
  }

  TEST_F(NvencUninitializedTeardown, DiscardIsIdempotent) {
    // create_encoder's fail_guard can run after a path that already cleaned
    // up; a second pass must not double-destroy the handle.
    stub_nvenc enc;
    enc.simulate_opened_session();

    enc.discard_uninitialized_session();
    enc.discard_uninitialized_session();

    EXPECT_EQ(g_calls.destroy_encoder, 1);
  }

  TEST_F(NvencUninitializedTeardown, LiveSessionStillGetsTheFullTeardown) {
    // The fix must not weaken cleanup for a session that really did register
    // an async event -- that would leak driver resources.
    stub_nvenc enc;
    enc.simulate_live_session();

    enc.destroy_encoder();

    EXPECT_EQ(g_calls.unregister_async_event, 1);
    EXPECT_EQ(g_calls.destroy_encoder, 1);
    EXPECT_FALSE(enc.has_encoder());
    EXPECT_FALSE(enc.is_initialized());
  }

  TEST_F(NvencUninitializedTeardown, RejectedInitializeRemainsASeparatePath) {
    // cleanup_rejected_initialize() handles a session whose
    // NvEncInitializeEncoder was called *and rejected*. The D3D12 backend
    // overrides it to disable itself for the rest of the process, so the
    // uninitialized case must never be routed through it — doing so would
    // drop every host to the D3D11 transport the first time the encoder
    // probe made its routine YUV444 attempt.
    stub_nvenc enc;
    enc.simulate_opened_session();

    enc.cleanup_rejected_initialize();

    EXPECT_EQ(g_calls.unregister_async_event, 0);
    EXPECT_EQ(g_calls.destroy_encoder, 1);
    EXPECT_FALSE(enc.has_encoder());
  }

  TEST_F(NvencUninitializedTeardown, TeardownOfANeverOpenedSessionIsANoOp) {
    stub_nvenc enc;

    enc.discard_uninitialized_session();

    EXPECT_EQ(g_calls.destroy_encoder, 0);
    EXPECT_EQ(g_calls.unregister_async_event, 0);
  }

}  // namespace

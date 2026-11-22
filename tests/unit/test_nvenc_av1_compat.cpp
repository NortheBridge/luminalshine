#include <gtest/gtest.h>
#include "src/nvenc/nvenc_av1_compat.h"

TEST(NvencAv1Compat, FailedFirstFrameSelectsCompatibilityForRetry) {
  nvenc::av1_compatibility state;
  EXPECT_FALSE(state.use_d3d11(2));
  EXPECT_TRUE(state.note_lock_failure(2, false, NV_ENC_ERR_INVALID_PARAM));
  EXPECT_TRUE(state.use_d3d11(2));
  EXPECT_FALSE(state.note_lock_failure(2, false, NV_ENC_ERR_INVALID_PARAM));
  EXPECT_FALSE(state.use_d3d11(0));
  EXPECT_FALSE(state.use_d3d11(1));
}

TEST(NvencAv1Compat, WorkerHonorsParentSelectionWithoutItsOwnFailure) {
  nvenc::av1_compatibility parent, worker;
  parent.note_lock_failure(2, false, NV_ENC_ERR_INVALID_PARAM);
  const bool transmitted = parent.use_d3d11(2);
  EXPECT_FALSE(worker.use_d3d11(2));
  EXPECT_TRUE(worker.use_d3d11(2, transmitted));
  EXPECT_FALSE(worker.use_d3d11(0, transmitted));
  EXPECT_FALSE(worker.use_d3d11(1, transmitted));
}

TEST(NvencAv1Compat, UnrelatedErrorsAndSuccessfulSessionsDoNotChangeTransport) {
  nvenc::av1_compatibility state;
  EXPECT_FALSE(state.note_lock_failure(0, false, NV_ENC_ERR_INVALID_PARAM));
  EXPECT_FALSE(state.note_lock_failure(1, false, NV_ENC_ERR_INVALID_PARAM));
  EXPECT_FALSE(state.note_lock_failure(2, true, NV_ENC_ERR_INVALID_PARAM));
  for (auto status : {NV_ENC_SUCCESS, NV_ENC_ERR_LOCK_BUSY, NV_ENC_ERR_DEVICE_NOT_EXIST,
                      NV_ENC_ERR_GENERIC, NV_ENC_ERR_INVALID_VERSION}) {
    EXPECT_FALSE(state.note_lock_failure(2, false, status));
  }
  EXPECT_FALSE(state.use_d3d11(2));
}

TEST(NvencAv1Compat, NewProcessRetriesNativeTransport) {
  nvenc::av1_compatibility previous;
  previous.note_lock_failure(2, false, NV_ENC_ERR_INVALID_PARAM);
  nvenc::av1_compatibility restarted;
  EXPECT_FALSE(restarted.use_d3d11(2));
}

#pragma once

#include <cstdint>
#include <string>

#include "src/thread_safe.h"
#include "src/video.h"

namespace platf::video_worker {
  bool is_child_process();
  void set_child_pipe(std::string pipe_name, std::uint32_t parent_pid);
  int run_child();

  /** Run capture+encode in a crash-isolated child process. */
  bool capture(safe::mail_t mail, video::config_t config, void *channel_data);
}

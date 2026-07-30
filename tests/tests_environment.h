/**
 * @file tests/tests_environment.h
 * @brief Declarations for SunshineEnvironment.
 */
#pragma once
#include "tests_common.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <src/config.h>

struct SunshineEnvironment: testing::Environment {
  void SetUp() override {
    mail::man = std::make_shared<safe::mail_raw_t>();
    deinit_log = logging::init_single_file(0, "test_sunshine.log");

    redirect_runtime_state();
  }

  void TearDown() override {
    deinit_log = {};
    mail::man = {};

    if (!test_env::scratch_dir().empty()) {
      std::error_code ec;
      std::filesystem::remove_all(test_env::scratch_dir(), ec);
      test_env::scratch_dir().clear();
    }
  }

  std::unique_ptr<logging::deinit_t> deinit_log;

private:
  /**
   * Point every runtime state file the host can write at a private temp
   * directory, and publish that directory as test_env::scratch_dir().
   *
   * The defaults for file_state, luminalshine_file_state and credentials_file
   * are RELATIVE paths (src/config.cpp), and the test binary must run from the
   * repo root because the integration suites read src/config.cpp and
   * docs/configuration.md through relative paths. Without this redirect, any
   * test that reaches save_state() — every successful pairing — writes
   * sunshine_state.json into the source tree. That file was also tracked by
   * git, so `git status` came back dirty after every test run and a real dev
   * run's pairings could be committed by accident.
   */
  void redirect_runtime_state() {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) {
      BOOST_LOG(warning) << "Tests: no temp directory available (" << ec.message()
                         << "); runtime state files will fall back to their defaults.";
      return;
    }

    // Find an unused directory rather than trusting a fixed name, so two test
    // binaries running concurrently cannot delete each other's state.
    for (int attempt = 0; attempt < 256; ++attempt) {
      const auto candidate = base / ("luminalshine-tests-" + std::to_string(attempt));
      std::error_code mk;
      if (std::filesystem::create_directory(candidate, mk)) {
        test_env::scratch_dir() = candidate;
        break;
      }
    }

    if (test_env::scratch_dir().empty()) {
      BOOST_LOG(warning) << "Tests: could not create a scratch directory under " << base.string()
                         << "; runtime state files will fall back to their defaults.";
      return;
    }

    const auto &dir = test_env::scratch_dir();
    config::nvhttp.file_state = (dir / "sunshine_state.json").string();
    config::nvhttp.luminalshine_file_state = (dir / "luminalshine_state.json").string();
    config::sunshine.credentials_file = (dir / "luminalshine_credentials.json").string();

    BOOST_LOG(tests) << "Tests: runtime state redirected to " << dir.string();
  }
};

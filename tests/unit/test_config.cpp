// standard includes
#include <filesystem>
#include <fstream>
#include <string>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"
#include "src/config.h"

// Regression test for the recurring admin-lockout bug: a hot config apply
// (config::apply_config_now) resets the runtime config structs to defaults
// and must preserve the ENTIRE in-RAM credential record — the config file
// carries no credentials, so any field dropped here is lost until the next
// service restart. When password_kdf was dropped, verification downgraded
// to legacy SHA-256 against an Argon2id hash and every login rejected the
// valid admin password. Hot applies run on every stream start (per-app
// runtime overrides), so this lockout appeared "randomly" mid-uptime.
TEST(ConfigApply, PreservesInRamCredentialRecord) {
  namespace fs = std::filesystem;
  const auto conf_path = fs::temp_directory_path() / "luminalshine_test_apply.conf";
  {
    std::ofstream out(conf_path);
    out << "min_log_level = 2\n";
  }
  const auto saved_config_file = config::sunshine.config_file;
  config::sunshine.config_file = conf_path.string();

  config::sunshine.username = "test-admin";
  config::sunshine.password = "argon2id-hash-placeholder";
  config::sunshine.salt = "0123456789abcdef";
  config::sunshine.password_kdf = "argon2id";
  config::sunshine.argon2_m_cost_kib = 32768;
  config::sunshine.argon2_t_cost = 4;
  config::sunshine.argon2_parallel = 2;

  config::apply_config_now();

  EXPECT_EQ(config::sunshine.username, "test-admin");
  EXPECT_EQ(config::sunshine.password, "argon2id-hash-placeholder");
  EXPECT_EQ(config::sunshine.salt, "0123456789abcdef");
  EXPECT_EQ(config::sunshine.password_kdf, "argon2id");
  EXPECT_EQ(config::sunshine.argon2_m_cost_kib, 32768u);
  EXPECT_EQ(config::sunshine.argon2_t_cost, 4u);
  EXPECT_EQ(config::sunshine.argon2_parallel, 2u);

  config::sunshine.config_file = saved_config_file;
  std::error_code ec;
  fs::remove(conf_path, ec);
}

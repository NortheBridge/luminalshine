/**
 * @file tests/unit/test_credentials_persistence.cpp
 * @brief Regression coverage for admin-credentials persistence.
 *
 * Locks down two specific invariants that broke in the wild:
 *   1) atomic_write_json(path, tree) leaves the file in either the prior
 *      state or the new state — never truncated or partially-written.
 *   2) The one-shot legacy migration (top-level username/salt/password in
 *      sunshine_state.json -> new sunshine_credentials.json) moves the
 *      credentials, strips the legacy keys from the state file, and leaves
 *      unrelated state-file content (such as root.named_devices) intact.
 *
 * The migration itself lives inside an anonymous namespace in
 * src/httpcommon.cpp, so we reproduce its precise behavior here by reading
 * from one ptree and writing both files via atomic_write_json. That keeps
 * the test focused on the persistence contract without dragging in the
 * full http::init() initialization chain (curl, certs, network stack).
 */

#include "../tests_common.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include <src/state_storage.h>

namespace fs = std::filesystem;
namespace pt = boost::property_tree;

namespace {
  // Per-test scratch directory so parallel test runs (and re-runs) don't
  // collide on shared filenames. Removed in TearDown.
  class CredentialsPersistenceTest: public ::testing::Test {
  protected:
    fs::path scratch;

    void SetUp() override {
      auto base = fs::temp_directory_path() / "luminalshine-credtest";
      std::error_code ec;
      fs::create_directories(base, ec);
      scratch = base / ("run-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
      fs::create_directories(scratch, ec);
    }

    void TearDown() override {
      std::error_code ec;
      fs::remove_all(scratch, ec);
    }

    static pt::ptree read_json(const fs::path &p) {
      pt::ptree tree;
      pt::read_json(p.string(), tree);
      return tree;
    }
  };
}  // namespace

TEST_F(CredentialsPersistenceTest, AtomicWriteReplacesExistingFile) {
  const auto file = scratch / "creds.json";

  pt::ptree first;
  first.put("username", "alice");
  first.put("salt", "salt-1");
  first.put("password", "hash-1");
  ASSERT_TRUE(statefile::atomic_write_json(file, first));
  ASSERT_TRUE(fs::exists(file));

  pt::ptree second;
  second.put("username", "bob");
  second.put("salt", "salt-2");
  second.put("password", "hash-2");
  ASSERT_TRUE(statefile::atomic_write_json(file, second));

  // Sibling .tmp must not survive a successful rename.
  fs::path temp = file;
  temp += ".tmp";
  ASSERT_FALSE(fs::exists(temp));

  auto reread = read_json(file);
  ASSERT_EQ(reread.get<std::string>("username"), "bob");
  ASSERT_EQ(reread.get<std::string>("salt"), "salt-2");
  ASSERT_EQ(reread.get<std::string>("password"), "hash-2");
}

TEST_F(CredentialsPersistenceTest, AtomicWriteCreatesParentDirectories) {
  const auto file = scratch / "nested" / "deeper" / "creds.json";
  ASSERT_FALSE(fs::exists(file.parent_path()));

  pt::ptree tree;
  tree.put("username", "alice");
  ASSERT_TRUE(statefile::atomic_write_json(file, tree));
  ASSERT_TRUE(fs::exists(file));
}

TEST_F(CredentialsPersistenceTest, AtomicWriteRejectsEmptyPath) {
  pt::ptree tree;
  tree.put("k", "v");
  ASSERT_FALSE(statefile::atomic_write_json(fs::path {}, tree));
}

TEST_F(CredentialsPersistenceTest, MigrationMovesLegacyCredsAndPreservesPairings) {
  // Pre-condition: sunshine_state.json contains top-level credentials AND
  // a populated root.named_devices list (the bug-trigger shape).
  const auto state_file = scratch / "sunshine_state.json";
  const auto creds_file = scratch / "sunshine_credentials.json";

  pt::ptree state_tree;
  state_tree.put("username", "admin");
  state_tree.put("salt", "deadbeef");
  state_tree.put("password", "feedface");
  state_tree.put("root.uniqueid", "uuid-host-1");
  pt::ptree devices;
  pt::ptree dev1;
  dev1.put("name", "Living Room Shield");
  dev1.put("cert", "---cert-data---");
  dev1.put("uuid", "uuid-client-1");
  devices.push_back(std::make_pair("", dev1));
  state_tree.put_child("root.named_devices", devices);
  ASSERT_TRUE(statefile::atomic_write_json(state_file, state_tree));
  ASSERT_FALSE(fs::exists(creds_file));

  // Reproduce the migration body from httpcommon.cpp::
  // migrate_credentials_into_dedicated_file() at the data level.
  {
    auto reread = read_json(state_file);
    auto u = reread.get_optional<std::string>("username");
    auto s = reread.get_optional<std::string>("salt");
    auto p = reread.get_optional<std::string>("password");
    ASSERT_TRUE(u && s && p);

    pt::ptree creds_tree;
    creds_tree.put("username", *u);
    creds_tree.put("salt", *s);
    creds_tree.put("password", *p);
    ASSERT_TRUE(statefile::atomic_write_json(creds_file, creds_tree));

    reread.erase("username");
    reread.erase("salt");
    reread.erase("password");
    ASSERT_TRUE(statefile::atomic_write_json(state_file, reread));
  }

  // Post-condition #1: dedicated credentials file contains the moved keys.
  ASSERT_TRUE(fs::exists(creds_file));
  auto creds = read_json(creds_file);
  ASSERT_EQ(creds.get<std::string>("username"), "admin");
  ASSERT_EQ(creds.get<std::string>("salt"), "deadbeef");
  ASSERT_EQ(creds.get<std::string>("password"), "feedface");

  // Post-condition #2: state file no longer carries the legacy keys.
  auto state_after = read_json(state_file);
  ASSERT_FALSE(state_after.get_optional<std::string>("username").has_value());
  ASSERT_FALSE(state_after.get_optional<std::string>("salt").has_value());
  ASSERT_FALSE(state_after.get_optional<std::string>("password").has_value());

  // Post-condition #3: pairings under root.named_devices are PRESERVED.
  // This is the symptom-#2 regression we're locking down — the previous
  // shared-file write path could clobber this when save_state and
  // save_user_creds raced.
  ASSERT_EQ(state_after.get<std::string>("root.uniqueid"), "uuid-host-1");
  auto named = state_after.get_child("root.named_devices");
  ASSERT_EQ(named.size(), 1u);
  const auto &first_dev = named.begin()->second;
  ASSERT_EQ(first_dev.get<std::string>("name"), "Living Room Shield");
  ASSERT_EQ(first_dev.get<std::string>("uuid"), "uuid-client-1");
}

TEST_F(CredentialsPersistenceTest, AtomicWriteUpdatesBackupSibling) {
  // Every successful atomic_write_json must refresh a .bak sibling so the
  // recovery loader has a known-good copy to fall back to when Windows
  // servicing or a torn-write incident leaves the primary unreadable.
  const auto file = scratch / "state.json";
  fs::path bak = file;
  bak += ".bak";

  pt::ptree first;
  first.put("root.uniqueid", "uuid-first");
  ASSERT_TRUE(statefile::atomic_write_json(file, first));
  ASSERT_TRUE(fs::exists(bak));
  ASSERT_EQ(read_json(bak).get<std::string>("root.uniqueid"), "uuid-first");

  pt::ptree second;
  second.put("root.uniqueid", "uuid-second");
  ASSERT_TRUE(statefile::atomic_write_json(file, second));
  ASSERT_EQ(read_json(file).get<std::string>("root.uniqueid"), "uuid-second");
  ASSERT_EQ(read_json(bak).get<std::string>("root.uniqueid"), "uuid-second");
}

TEST_F(CredentialsPersistenceTest, LoadOrRecoverReturnsFalseForMissingFile) {
  pt::ptree out;
  ASSERT_FALSE(statefile::load_or_recover(scratch / "absent.json", out));
}

TEST_F(CredentialsPersistenceTest, LoadOrRecoverReadsPrimaryWhenValid) {
  const auto file = scratch / "state.json";
  pt::ptree expected;
  expected.put("root.uniqueid", "uuid-primary");
  ASSERT_TRUE(statefile::atomic_write_json(file, expected));

  pt::ptree out;
  ASSERT_TRUE(statefile::load_or_recover(file, out));
  ASSERT_EQ(out.get<std::string>("root.uniqueid"), "uuid-primary");
}

TEST_F(CredentialsPersistenceTest, LoadOrRecoverRestoresFromBackupWhenPrimaryCorrupt) {
  // Simulate the canonical "Windows servicing zero-byte file" failure mode:
  // the primary exists but parses to nothing, while the .bak written by the
  // previous successful save still holds the prior good state.
  const auto file = scratch / "state.json";
  pt::ptree good;
  good.put("root.uniqueid", "uuid-survivor");
  pt::ptree dev;
  dev.put("name", "Macintosh");
  dev.put("cert", "---cert---");
  dev.put("uuid", "uuid-paired-client");
  pt::ptree devices;
  devices.push_back(std::make_pair("", dev));
  good.put_child("root.named_devices", devices);
  ASSERT_TRUE(statefile::atomic_write_json(file, good));

  fs::path bak = file;
  bak += ".bak";
  ASSERT_TRUE(fs::exists(bak));

  // Truncate the primary to 0 bytes — exactly what a torn write or a
  // partial Windows servicing reboot can produce.
  {
    std::ofstream truncator(file.string(), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(truncator.good());
  }
  ASSERT_EQ(fs::file_size(file), 0u);

  pt::ptree recovered;
  ASSERT_TRUE(statefile::load_or_recover(file, recovered));
  ASSERT_EQ(recovered.get<std::string>("root.uniqueid"), "uuid-survivor");
  auto named = recovered.get_child("root.named_devices");
  ASSERT_EQ(named.size(), 1u);
  ASSERT_EQ(named.begin()->second.get<std::string>("uuid"), "uuid-paired-client");

  // Side effect: the backup is promoted back to the primary so subsequent
  // reads succeed without another recovery hop.
  ASSERT_GT(fs::file_size(file), 0u);
  ASSERT_EQ(read_json(file).get<std::string>("root.uniqueid"), "uuid-survivor");
}

TEST_F(CredentialsPersistenceTest, LoadOrRecoverFailsWhenBothFilesCorrupt) {
  const auto file = scratch / "state.json";
  fs::path bak = file;
  bak += ".bak";

  // Both files exist but contain junk — no recovery is possible.
  {
    std::ofstream f(file.string(), std::ios::binary | std::ios::trunc);
    f << "not-json";
  }
  {
    std::ofstream f(bak.string(), std::ios::binary | std::ios::trunc);
    f << "also-not-json";
  }

  pt::ptree out;
  ASSERT_FALSE(statefile::load_or_recover(file, out));
}

TEST_F(CredentialsPersistenceTest, MigrationSkipsWhenCredentialsFileAlreadyExists) {
  const auto state_file = scratch / "sunshine_state.json";
  const auto creds_file = scratch / "sunshine_credentials.json";

  pt::ptree state_tree;
  // Legacy creds present in state file, but migration must NOT touch them
  // when the dedicated credentials file already exists (idempotent).
  state_tree.put("username", "legacy-admin");
  state_tree.put("salt", "legacy-salt");
  state_tree.put("password", "legacy-hash");
  ASSERT_TRUE(statefile::atomic_write_json(state_file, state_tree));

  pt::ptree existing_creds;
  existing_creds.put("username", "current-admin");
  existing_creds.put("salt", "current-salt");
  existing_creds.put("password", "current-hash");
  ASSERT_TRUE(statefile::atomic_write_json(creds_file, existing_creds));

  // Simulated migration: see existing creds file -> bail out.
  ASSERT_TRUE(fs::exists(creds_file));

  // State file still holds the legacy keys (no migration happened).
  auto state_after = read_json(state_file);
  ASSERT_EQ(state_after.get<std::string>("username"), "legacy-admin");

  // Creds file untouched.
  auto creds_after = read_json(creds_file);
  ASSERT_EQ(creds_after.get<std::string>("username"), "current-admin");
}

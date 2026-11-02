/**
 * @file tests/unit/test_confighttp_crash_dumps.cpp
 * @brief Test the crash dump discovery behind the Web UI crash bundle (src/confighttp_playnite.cpp).
 */
#include "../tests_common.h"

#ifdef _WIN32
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <src/confighttp_crash_dumps.h>
  #include <string>
  #include <string_view>
  #include <vector>

// clang-format off
  #include <windows.h>
  #include <dbghelp.h>
// clang-format on

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  // The types the crash handler writes (see kDumpType in crash_handler.cpp).
  constexpr std::uint64_t kStackOnlyFlags = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
  constexpr std::uintmax_t kMiB = 1024 * 1024;

  void set_age(const fs::path &path, std::chrono::minutes age) {
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
  }

  /// Write a file that starts with a real minidump header carrying @p flags.
  void write_dump(const fs::path &path, std::uint64_t flags, std::chrono::minutes age) {
    MINIDUMP_HEADER header {};
    header.Signature = MINIDUMP_SIGNATURE;
    header.Version = MINIDUMP_VERSION;
    header.Flags = flags;
    {
      std::ofstream out {path, std::ios::binary};
      out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    }
    set_age(path, age);
  }

  /// Write a zero-filled file of @p size bytes. It has no minidump header.
  void write_blob(const fs::path &path, std::uintmax_t size, std::chrono::minutes age) {
    {
      std::ofstream out {path, std::ios::binary};
    }
    fs::resize_file(path, size);
    set_age(path, age);
  }

  /// Write @p text as UTF-16LE with a byte order mark, the way Windows Error Reporting writes Report.wer.
  void write_wer_report(const fs::path &path, std::wstring_view text) {
    std::ofstream out {path, std::ios::binary};
    out.put(static_cast<char>(0xFF));
    out.put(static_cast<char>(0xFE));
    for (const wchar_t ch : text) {
      out.put(static_cast<char>(ch & 0xFF));
      out.put(static_cast<char>((ch >> 8) & 0xFF));
    }
  }

  class CrashBundleDumpDiscovery: public testing::Test {
  protected:
    void SetUp() override {
      if (test_env::scratch_dir().empty()) {
        GTEST_SKIP() << "test scratch directory is unavailable";
      }
      // Separate parents: Windows paths are case-insensitive, so crashdumps
      // and CrashDumps would otherwise be the same folder.
      root = test_env::scratch_dir() / "crash_bundle_dump_discovery";
      std::error_code ec;
      fs::remove_all(root, ec);
      handler_dir = root / "appdata" / "crashdumps";
      local_dir = root / "localdumps" / "CrashDumps";
      wer_dir = root / "wer" / "ReportArchive";
      fs::create_directories(handler_dir);
      fs::create_directories(local_dir);
      fs::create_directories(wer_dir);
    }

    void TearDown() override {
      if (root.empty()) {
        return;
      }
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    std::vector<confighttp::CrashDumpInfo> find_dumps() const {
      return confighttp::find_recent_crash_dumps({handler_dir}, {local_dir}, {wer_dir}, std::chrono::hours {24 * 7});
    }

    fs::path root;
    fs::path handler_dir;
    fs::path local_dir;
    fs::path wer_dir;
  };

  TEST_F(CrashBundleDumpDiscovery, HostProcessComesFromCrashHandlerDumps) {
    const auto handler_dump = handler_dir / "luminalshine-20261028-101010-100.dmp";
    write_dump(handler_dump, kStackOnlyFlags, 60min);

    // The full dumps Windows writes for the host process: a LocalDumps file
    // and the memory dump in a Windows Error Reporting report. Both are newer
    // and larger than the crash handler's dump, and neither may be offered.
    write_blob(local_dir / "luminalshine.exe.4321.dmp", 11 * kMiB, 10min);
    const auto report_dir = wer_dir / "AppCrash_luminalshine.exe_1";
    fs::create_directories(report_dir);
    write_wer_report(report_dir / "Report.wer", L"Version=1\r\nEventType=APPCRASH\r\nAppPath=C:\\Program Files\\LuminalShine\\luminalshine.exe\r\n");
    write_blob(report_dir / "memory.hdmp", 11 * kMiB, 5min);

    const auto dumps = find_dumps();
    ASSERT_EQ(dumps.size(), 1u);
    EXPECT_EQ(dumps[0].process, "luminalshine.exe");
    EXPECT_EQ(dumps[0].path, handler_dump);
  }

  TEST_F(CrashBundleDumpDiscovery, SkipsCrashHandlerDumpsThatAreNotStackOnly) {
    const auto stack_only = handler_dir / "luminalshine-20261028-101010-100.dmp";
    write_dump(stack_only, kStackOnlyFlags, 60min);

    // Newer, but written before the dump type changed: it also holds the
    // memory behind stack pointers.
    write_dump(handler_dir / "luminalshine-20261028-111111-100.dmp", kStackOnlyFlags | MiniDumpWithIndirectlyReferencedMemory, 10min);

    // Not one of the crash handler's files, whatever it holds.
    write_dump(handler_dir / "other.dmp", kStackOnlyFlags, 5min);

    const auto dumps = find_dumps();
    ASSERT_EQ(dumps.size(), 1u);
    EXPECT_EQ(dumps[0].path, stack_only);
  }

  TEST_F(CrashBundleDumpDiscovery, AcceptsFlagsDbghelpAddsOnItsOwn) {
    // dbghelp ORs 0x200000 (AVX state) into the header flags of dumps written
    // on CPUs that have it, so real stack-only dumps carry that bit.
    const auto dump = handler_dir / "luminalshine-20261028-101010-100.dmp";
    write_dump(dump, kStackOnlyFlags | MiniDumpWithAvxXStateContext, 60min);

    const auto dumps = find_dumps();
    ASSERT_EQ(dumps.size(), 1u);
    EXPECT_EQ(dumps[0].path, dump);
  }

  TEST_F(CrashBundleDumpDiscovery, SkipsCrashHandlerFilesWithoutDumpHeader) {
    // Right name, but not a minidump: zeros where the header should be, and a
    // file too short to hold one.
    write_blob(handler_dir / "luminalshine-20261028-101010-100.dmp", 4096, 60min);
    write_blob(handler_dir / "luminalshine-20261028-111111-100.dmp", 10, 10min);

    EXPECT_TRUE(find_dumps().empty());
  }

  TEST_F(CrashBundleDumpDiscovery, KeepsHelperProcessDumps) {
    write_blob(local_dir / "luminalshine_display_helper.exe.777.dmp", 4096, 5min);

    const auto dumps = find_dumps();
    ASSERT_EQ(dumps.size(), 1u);
    EXPECT_EQ(dumps[0].process, "luminalshine_display_helper.exe");
  }
}  // namespace
#endif  // _WIN32

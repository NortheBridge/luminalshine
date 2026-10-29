/**
 * @file tests/unit/test_crash_handler.cpp
 * @brief Test src/crash_handler.*.
 *
 * The dump-directory pruning helper and the minidump writer are exercised
 * here. The actual exception filter / terminate handler are deliberately NOT
 * tested in-process: triggering them would tear the test runner down.
 */
#include "../tests_common.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <src/crash_handler.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
// clang-format off
  #include <windows.h>
  #include <dbghelp.h>
// clang-format on
#endif

namespace {
  namespace fs = std::filesystem;

  class CrashHandlerPruneTest: public testing::Test {
  protected:
    void SetUp() override {
      test_dir = fs::temp_directory_path() / ("crash_handler_prune_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      fs::create_directories(test_dir);
    }

    void TearDown() override {
      std::error_code ec;
      fs::remove_all(test_dir, ec);
    }

    /// Create a file and stamp it with a deterministic mtime, `age_rank`
    /// seconds in the past — rank 0 is the newest. Explicit stamps avoid
    /// flakiness from filesystem timestamp resolution.
    fs::path make_file(const std::string &name, int age_rank) {
      const auto path = test_dir / name;
      std::ofstream {path} << "fake dump";
      fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::seconds {age_rank * 10});
      return path;
    }

    std::vector<std::string> remaining_files() const {
      std::vector<std::string> names;
      for (const auto &entry : fs::directory_iterator {test_dir}) {
        names.push_back(entry.path().filename().string());
      }
      std::sort(names.begin(), names.end());
      return names;
    }

    fs::path test_dir;
  };

  TEST_F(CrashHandlerPruneTest, KeepsNewestFive) {
    // dump-0 is the newest, dump-8 the oldest.
    for (int i = 0; i < 9; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 5), 4u);

    const std::vector<std::string> expected {"dump-0.dmp", "dump-1.dmp", "dump-2.dmp", "dump-3.dmp", "dump-4.dmp"};
    EXPECT_EQ(remaining_files(), expected);
  }

  TEST_F(CrashHandlerPruneTest, NoopAtOrUnderLimit) {
    for (int i = 0; i < 5; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 5), 0u);
    EXPECT_EQ(remaining_files().size(), 5u);
  }

  TEST_F(CrashHandlerPruneTest, IgnoresNonDumpFiles) {
    // Six dumps (one over the limit) plus two bystanders older than all
    // of them. Only the oldest .dmp may go; the bystanders must survive
    // and must not count toward the limit.
    for (int i = 0; i < 6; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }
    make_file("notes.txt", 20);
    make_file("dump-archive.dmp.bak", 21);

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 5), 1u);

    const auto names = remaining_files();
    EXPECT_EQ(names.size(), 7u);
    EXPECT_EQ(std::count(names.begin(), names.end(), "dump-5.dmp"), 0);
    EXPECT_EQ(std::count(names.begin(), names.end(), "notes.txt"), 1);
    EXPECT_EQ(std::count(names.begin(), names.end(), "dump-archive.dmp.bak"), 1);
  }

  TEST_F(CrashHandlerPruneTest, MissingDirectoryIsSafe) {
    EXPECT_EQ(crash_handler::prune_dumps(test_dir / "does_not_exist", 5), 0u);
  }

  TEST_F(CrashHandlerPruneTest, KeepZeroRemovesEverything) {
    for (int i = 0; i < 3; ++i) {
      make_file("dump-" + std::to_string(i) + ".dmp", i);
    }

    EXPECT_EQ(crash_handler::prune_dumps(test_dir, 0), 3u);
    EXPECT_TRUE(remaining_files().empty());
  }

#ifdef _WIN32

  // Same value as kTerminateExceptionCode in crash_handler.cpp.
  constexpr DWORD kTerminateCode = 0xE04C5354;
  constexpr std::size_t kTagLength = 8;
  constexpr std::size_t kRandomDigits = 16;
  constexpr std::size_t kTextLength = kTagLength + kRandomDigits;

  /// Unique ASCII text: @p tag (8 characters) followed by 16 random hex
  /// digits. It is written one character at a time straight into a heap
  /// buffer, so the complete text never sits in a stack buffer where a
  /// stack-only dump would capture it.
  std::unique_ptr<char[]> make_heap_text(std::string_view tag) {
    constexpr std::string_view hex_digits = "0123456789abcdef";
    std::random_device random;
    auto text = std::make_unique<char[]>(kTextLength + 1);
    std::size_t at = 0;
    for (const char ch : tag) {
      text[at++] = ch;
    }
    for (std::size_t i = 0; i < kRandomDigits; ++i) {
      text[at++] = hex_digits[random() & 0xF];
    }
    text[at] = '\0';
    return text;
  }

  /// Keep @p marker only behind a pointer in this frame, then write a dump
  /// from a context captured here. The synthetic exception carries @p code.
  [[gnu::noinline]] bool dump_with_heap_marker(HANDLE file, DWORD code, char *marker) {
    char *volatile slot = marker;
    CONTEXT context {};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record {};
    record.ExceptionCode = code;
    EXCEPTION_POINTERS pointers {&record, &context};
    const bool written = crash_handler::write_minidump(file, &pointers);
    return written && slot == marker;
  }

  bool contains(const std::string &haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
  }

  class CrashHandlerMinidump: public testing::Test {
  protected:
    void SetUp() override {
      if (test_env::scratch_dir().empty()) {
        GTEST_SKIP() << "test scratch directory is unavailable";
      }
      dump_path = test_env::scratch_dir() / "crash_handler_minidump.dmp";
      marker = make_heap_text("HEAPMARK");
      message = make_heap_text("TERMMSG:");
    }

    void TearDown() override {
      crash_handler::note_terminate_message(nullptr);
      std::error_code ec;
      fs::remove(dump_path, ec);
    }

    /// Write a dump for a synthetic exception with @p code, then read the file back into `bytes`.
    bool write_dump(DWORD code) {
      const HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        return false;
      }
      const bool written = dump_with_heap_marker(file, code, marker.get());
      CloseHandle(file);

      std::ifstream in {dump_path, std::ios::binary};
      bytes.assign(std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {});
      return written;
    }

    /// Checks shared by every dump: valid header, only the stack-only
    /// types, and the heap behind the stack pointer left out.
    void expect_stack_only_dump() const {
      MINIDUMP_HEADER header {};
      ASSERT_GE(bytes.size(), sizeof(header));
      std::memcpy(&header, bytes.data(), sizeof(header));
      EXPECT_EQ(header.Signature, static_cast<ULONG32>(MINIDUMP_SIGNATURE));

      // dbghelp ORs bits of its own above the requested type into Flags (for
      // example 0x200000 on CPUs with AVX state), so compare only the type bits.
      EXPECT_EQ(header.Flags & 0xFFFF, static_cast<ULONG64>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules));

      // Control: the frame holding the pointer was captured.
      const auto address = reinterpret_cast<std::uintptr_t>(marker.get());
      EXPECT_TRUE(contains(bytes, {reinterpret_cast<const char *>(&address), sizeof(address)}));

      // The heap behind that pointer was not.
      EXPECT_FALSE(contains(bytes, {marker.get(), kTextLength}));
    }

    fs::path dump_path;
    std::unique_ptr<char[]> marker;
    std::unique_ptr<char[]> message;
    std::string bytes;
  };

  TEST_F(CrashHandlerMinidump, WritesStackOnlyDumpWithTerminateMessage) {
    crash_handler::note_terminate_message(message.get());
    ASSERT_TRUE(write_dump(kTerminateCode));

    expect_stack_only_dump();
    EXPECT_TRUE(contains(bytes, {message.get(), kTextLength}));
  }

  TEST_F(CrashHandlerMinidump, OmitsTerminateMessageForOtherExceptions) {
    crash_handler::note_terminate_message(message.get());
    ASSERT_TRUE(write_dump(EXCEPTION_ACCESS_VIOLATION));

    expect_stack_only_dump();
    EXPECT_FALSE(contains(bytes, {message.get(), kTextLength}));
  }

#endif  // _WIN32
}  // namespace

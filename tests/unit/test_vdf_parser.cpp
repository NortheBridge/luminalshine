/**
 * @file tests/unit/test_vdf_parser.cpp
 * @brief Tests for the minimal text-VDF parser used by the Steam
 *        library auto-sync feature. Fixtures are abbreviated but
 *        structurally faithful samples of what Steam itself writes
 *        for libraryfolders.vdf and appmanifest_*.acf.
 */
#include "../tests_common.h"

#include <src/steam/vdf_parser.h>

namespace vdf = steam::vdf;

TEST(VdfParserTests, ParsesSimpleSingleEntry) {
  constexpr std::string_view input = R"(
    "AppState"
    {
      "appid"  "440"
      "name"   "Team Fortress 2"
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->key, "AppState");
  EXPECT_EQ(root->find_string("appid"), "440");
  EXPECT_EQ(root->find_string("name"), "Team Fortress 2");
}

TEST(VdfParserTests, ParsesNestedBlocks) {
  constexpr std::string_view input = R"(
    "libraryfolders"
    {
      "0"
      {
        "path"        "C:\\Program Files (x86)\\Steam"
        "label"       ""
        "apps"
        {
          "440"   "1234"
          "570"   "5678"
        }
      }
      "1"
      {
        "path"        "D:\\SteamLibrary"
      }
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->key, "libraryfolders");
  ASSERT_EQ(root->children().size(), 2u);

  const auto *folder0 = root->find("0");
  ASSERT_NE(folder0, nullptr);
  EXPECT_FALSE(folder0->is_string());
  EXPECT_EQ(folder0->find_string("path"), "C:\\Program Files (x86)\\Steam");

  const auto *apps = folder0->find("apps");
  ASSERT_NE(apps, nullptr);
  EXPECT_FALSE(apps->is_string());
  EXPECT_EQ(apps->children().size(), 2u);
  EXPECT_EQ(apps->find_string("440"), "1234");
  EXPECT_EQ(apps->find_string("570"), "5678");

  const auto *folder1 = root->find("1");
  ASSERT_NE(folder1, nullptr);
  EXPECT_EQ(folder1->find_string("path"), "D:\\SteamLibrary");
}

TEST(VdfParserTests, FindIsCaseInsensitive) {
  constexpr std::string_view input = R"(
    "AppState"
    {
      "AppId"  "440"
      "Name"   "TF2"
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->find_string("appid"), "440");
  EXPECT_EQ(root->find_string("NAME"), "TF2");
}

TEST(VdfParserTests, HandlesLineComments) {
  constexpr std::string_view input = R"(
    // top-level comment
    "AppState"
    {
      // before-key comment
      "appid"  "440"  // trailing comment
      "name"   "TF2"
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->find_string("appid"), "440");
}

TEST(VdfParserTests, HandlesEscapeSequences) {
  constexpr std::string_view input = R"(
    "AppState"
    {
      "installdir"  "Counter-Strike\\bin"
      "displayname" "\"Quoted\" Title"
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->find_string("installdir"), "Counter-Strike\\bin");
  EXPECT_EQ(root->find_string("displayname"), "\"Quoted\" Title");
}

TEST(VdfParserTests, RejectsUnclosedBlock) {
  constexpr std::string_view input = R"(
    "AppState"
    {
      "appid"  "440"
  )";
  auto root = vdf::parse(input);
  EXPECT_EQ(root, nullptr);
}

TEST(VdfParserTests, RejectsMissingOpeningBrace) {
  constexpr std::string_view input = R"(
    "AppState"
    "appid"  "440"
  )";
  auto root = vdf::parse(input);
  EXPECT_EQ(root, nullptr);
}

TEST(VdfParserTests, EmptyInputReturnsNull) {
  EXPECT_EQ(vdf::parse(""), nullptr);
  EXPECT_EQ(vdf::parse("   \n  // only a comment\n"), nullptr);
}

TEST(VdfParserTests, LegacyLibraryfoldersFormatIsParseable) {
  // Pre-2021 Steam wrote libraryfolders.vdf with index → path strings
  // at the top level, no per-folder block. parse() should still
  // produce a tree the caller can navigate (children are leaf nodes).
  constexpr std::string_view input = R"(
    "LibraryFolders"
    {
      "TimeNextStatsReport"   "0"
      "ContentStatsID"        "0"
      "1"  "D:\\SteamLibrary"
      "2"  "E:\\OtherDrive\\Steam"
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->find_string("1"), "D:\\SteamLibrary");
  EXPECT_EQ(root->find_string("2"), "E:\\OtherDrive\\Steam");
}

TEST(VdfParserTests, BinaryParsesShortcutsLikeBlob) {
  // Hand-built fixture mirroring the shape of Steam's
  // shortcuts.vdf. Top-level "shortcuts" block holding one indexed
  // "0" child shortcut. We cover all three value tags (0x00 nested
  // block, 0x01 string, 0x02 int32) plus the 0x08 end-of-block
  // sentinel so the parser exercises every branch.
  const std::string blob = std::string(
                             "\x00"
                             "shortcuts\x00"
                             "\x00"
                             "0\x00"
                             "\x02"
                             "appid\x00"
                             "\x40\x30\x20\x10"  // 0x10203040 little-endian
                             "\x01"
                             "AppName\x00"
                             "My Game\x00"
                             "\x01"
                             "Exe\x00"
                             "\"C:\\Games\\game.exe\"\x00"
                             "\x01"
                             "StartDir\x00"
                             "\"C:\\Games\\\"\x00"
                             "\x01"
                             "LaunchOptions\x00"
                             "-foo -bar\x00"
                             "\x00"
                             "tags\x00"
                             "\x08"
                             "\x08"
                             "\x08",
                             // sizeof above char array (including the
                             // intermediate NUL bytes that std::string
                             // would otherwise truncate at)
                             sizeof(
                               "\x00"
                               "shortcuts\x00"
                               "\x00"
                               "0\x00"
                               "\x02"
                               "appid\x00"
                               "\x40\x30\x20\x10"
                               "\x01"
                               "AppName\x00"
                               "My Game\x00"
                               "\x01"
                               "Exe\x00"
                               "\"C:\\Games\\game.exe\"\x00"
                               "\x01"
                               "StartDir\x00"
                               "\"C:\\Games\\\"\x00"
                               "\x01"
                               "LaunchOptions\x00"
                               "-foo -bar\x00"
                               "\x00"
                               "tags\x00"
                               "\x08"
                               "\x08"
                               "\x08"
                             ) -
                               1
  );
  auto root = vdf::parse_binary(blob);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->key, "shortcuts");
  const auto *shortcut = root->find("0");
  ASSERT_NE(shortcut, nullptr);
  EXPECT_FALSE(shortcut->is_string());
  EXPECT_EQ(shortcut->find_string("AppName"), "My Game");
  EXPECT_EQ(shortcut->find_string("Exe"), "\"C:\\Games\\game.exe\"");
  EXPECT_EQ(shortcut->find_string("StartDir"), "\"C:\\Games\\\"");
  EXPECT_EQ(shortcut->find_string("LaunchOptions"), "-foo -bar");
  EXPECT_EQ(shortcut->find_string("appid"), std::to_string(0x10203040));
  // Empty nested block "tags" parses successfully even with no
  // children — the 0x08 sentinel terminates it cleanly.
  const auto *tags = shortcut->find("tags");
  ASSERT_NE(tags, nullptr);
  EXPECT_FALSE(tags->is_string());
  EXPECT_EQ(tags->children().size(), 0u);
}

TEST(VdfParserTests, BinaryRejectsTruncatedInput) {
  // Truncated mid-value-string: the close-NUL is missing.
  const std::string blob(
    "\x00"
    "shortcuts\x00"
    "\x01"
    "key\x00"
    "unterminated value",  // no trailing NUL
    sizeof("\x00"
           "shortcuts\x00"
           "\x01"
           "key\x00"
           "unterminated value") -
      1
  );
  EXPECT_EQ(vdf::parse_binary(blob), nullptr);
}

TEST(VdfParserTests, BinaryRejectsUnknownTag) {
  // Unknown tag 0x05 must fail closed rather than silently swallow
  // the rest of the stream.
  const std::string blob(
    "\x00"
    "shortcuts\x00"
    "\x05"
    "weird\x00"
    "\x08",
    sizeof("\x00"
           "shortcuts\x00"
           "\x05"
           "weird\x00"
           "\x08") -
      1
  );
  EXPECT_EQ(vdf::parse_binary(blob), nullptr);
}

TEST(VdfParserTests, AppManifestRealisticShape) {
  // Trimmed but structurally accurate sample of what Steam writes to
  // appmanifest_<appid>.acf for an installed game.
  constexpr std::string_view input = R"(
    "AppState"
    {
      "appid"           "440"
      "Universe"        "1"
      "name"            "Team Fortress 2"
      "StateFlags"      "4"
      "installdir"      "Team Fortress 2"
      "LastUpdated"     "1700000000"
      "SizeOnDisk"      "23456789012"
      "buildid"         "9999999"
      "LastOwner"       "76561198000000000"
      "UserConfig"
      {
        "language"      "english"
      }
      "InstalledDepots"
      {
        "441"
        {
          "manifest"    "1111"
          "size"        "100000"
        }
      }
    }
  )";
  auto root = vdf::parse(input);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->key, "AppState");
  EXPECT_EQ(root->find_string("appid"), "440");
  EXPECT_EQ(root->find_string("name"), "Team Fortress 2");
  EXPECT_EQ(root->find_string("installdir"), "Team Fortress 2");
  EXPECT_EQ(root->find_string("StateFlags"), "4");

  const auto *user_config = root->find("UserConfig");
  ASSERT_NE(user_config, nullptr);
  EXPECT_EQ(user_config->find_string("language"), "english");
}

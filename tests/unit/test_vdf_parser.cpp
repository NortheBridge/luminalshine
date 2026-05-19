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

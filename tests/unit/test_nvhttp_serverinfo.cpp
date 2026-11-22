/**
 * @file tests/unit/test_nvhttp_serverinfo.cpp
 * @brief Test the LuminalShine release version in the nvhttp /serverinfo response.
 */

// standard includes
#include <regex>
#include <sstream>
#include <string>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

// local includes
#include "../tests_common.h"
#include "server_metadata.h"
#include "src/nvhttp.h"

namespace {
  namespace pt = boost::property_tree;

  /**
   * @brief Serialize the tree the way serverinfo does, then parse the XML back.
   */
  pt::ptree write_and_read_xml(const pt::ptree &tree) {
    std::ostringstream data;
    pt::write_xml(data, tree);

    std::istringstream input(data.str());
    pt::ptree parsed;
    pt::read_xml(input, parsed);
    return parsed;
  }

  TEST(ServerInfoVersion, ReportsTheProjectVersion) {
    pt::ptree tree;
    nvhttp::put_luminalshine_version(tree);

    const auto version = write_and_read_xml(tree).get<std::string>("root.LuminalShineVersion");
    EXPECT_FALSE(version.empty());
    EXPECT_EQ(version, PROJECT_VERSION);
  }

  TEST(ServerInfoVersion, AppendsAfterTheGameStreamEmulationFields) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.appversion", nvhttp::VERSION);
    tree.put("root.GfeVersion", nvhttp::GFE_VERSION);
    nvhttp::put_luminalshine_version(tree);

    const auto parsed = write_and_read_xml(tree);
    EXPECT_EQ(parsed.get<std::string>("root.appversion"), nvhttp::VERSION);
    EXPECT_EQ(parsed.get<std::string>("root.GfeVersion"), nvhttp::GFE_VERSION);

    const auto &root = parsed.get_child("root");
    ASSERT_EQ(root.size(), 7u);
    EXPECT_EQ(root.back().first, "LuminalShineVersion");
    EXPECT_EQ(root.back().second.data(), PROJECT_VERSION);
  }

  TEST(ServerInfoVersion, AppendsMetadataInDiscoveryOrder) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.PairStatus", 0);
    tree.put("root.currentgame", 42);
    tree.put("root.state", "SUNSHINE_SERVER_BUSY");
    tree.put("root.ServerCodecModeSupport", 127861505);
    tree.put("root.HttpsPort", 47984);
    tree.put("root.mac", "00:00:00:00:00:00");
    const auto original = tree.get_child("root");
    nvhttp::put_luminalshine_version(tree);
    const auto parsed = write_and_read_xml(tree);
    for (const auto &[key, value] : original) {
      EXPECT_EQ(parsed.get_child("root").get_child(key), value);
    }
    ASSERT_EQ(parsed.get_child("root").size(), original.size() + 4);
    auto field = parsed.get_child("root").begin();
    std::advance(field, original.size());
    EXPECT_EQ((field++)->first, "ServerIdentity");
    EXPECT_EQ((field++)->first, "ServerRel");
    EXPECT_EQ((field++)->first, "ServerBuild");
    EXPECT_EQ((field++)->first, "LuminalShineVersion");
    EXPECT_EQ(field, parsed.get_child("root").end());
    EXPECT_EQ(parsed.get<std::string>("root.ServerIdentity"), "LuminalShine");
    EXPECT_EQ(parsed.get<int>("root.ServerRel"), server_metadata::release);
    EXPECT_GE(server_metadata::release, 0);
    EXPECT_LE(server_metadata::release, 2);
    EXPECT_EQ(parsed.get<std::string>("root.ServerBuild"), server_metadata::build);
    EXPECT_TRUE(std::regex_match(server_metadata::build, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)")));
  }

  TEST(ServerInfoVersion, PreservesPairedHttpsFields) {
    pt::ptree tree;
    tree.put("root.PairStatus", 1);
    tree.put("root.mac", "01:23:45:67:89:ab");
    nvhttp::put_luminalshine_version(tree);
    const auto parsed = write_and_read_xml(tree);
    EXPECT_EQ(parsed.get<int>("root.PairStatus"), 1);
    EXPECT_EQ(parsed.get<std::string>("root.mac"), "01:23:45:67:89:ab");
    EXPECT_EQ(parsed.get<std::string>("root.LuminalShineVersion"), PROJECT_VERSION);
  }
}  // namespace

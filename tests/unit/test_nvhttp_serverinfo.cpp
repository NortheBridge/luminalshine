/**
 * @file tests/unit/test_nvhttp_serverinfo.cpp
 * @brief Test the LuminalShine fields in the nvhttp /serverinfo response.
 */

// standard includes
#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

// local includes
#include "../tests_common.h"
#include "src/host_capabilities.h"
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

  /**
   * @brief Split a `LuminalShineCapabilities` text the way a client would.
   */
  std::vector<std::string> split_capabilities(const std::string &wire) {
    std::vector<std::string> result;
    std::istringstream input(wire);
    for (std::string name; std::getline(input, name, ',');) {
      result.push_back(name);
    }
    return result;
  }

  bool has_capability(const std::string &wire, const std::string &name) {
    const auto names = split_capabilities(wire);
    return std::find(names.begin(), names.end(), name) != names.end();
  }

  TEST(ServerInfoLuminalShineFields, ReportsTheProjectVersion) {
    pt::ptree tree;
    nvhttp::put_luminalshine_fields(tree, {});

    const auto version = write_and_read_xml(tree).get<std::string>("root.LuminalShineVersion");
    EXPECT_FALSE(version.empty());
    EXPECT_EQ(version, PROJECT_VERSION);
  }

  TEST(ServerInfoLuminalShineFields, AppendsAfterTheGameStreamEmulationFields) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.appversion", nvhttp::VERSION);
    tree.put("root.GfeVersion", nvhttp::GFE_VERSION);
    nvhttp::put_luminalshine_fields(tree, {});

    const auto parsed = write_and_read_xml(tree);
    EXPECT_EQ(parsed.get<std::string>("root.appversion"), nvhttp::VERSION);
    EXPECT_EQ(parsed.get<std::string>("root.GfeVersion"), nvhttp::GFE_VERSION);

    const auto &root = parsed.get_child("root");
    ASSERT_EQ(root.size(), 6u);
    auto field = root.begin();
    std::advance(field, 3);
    EXPECT_EQ(field->first, "LuminalShineVersion");
    EXPECT_EQ(field->second.data(), PROJECT_VERSION);
    ++field;
    EXPECT_EQ(field->first, "LuminalShineApiLevel");
    ++field;
    EXPECT_EQ(field->first, "LuminalShineCapabilities");
  }

  TEST(ServerInfoLuminalShineFields, ReportsTheApiLevel) {
    pt::ptree tree;
    nvhttp::put_luminalshine_fields(tree, {});

    const auto api_level = write_and_read_xml(tree).get_optional<int>("root.LuminalShineApiLevel");
    ASSERT_TRUE(api_level.has_value());
    EXPECT_EQ(*api_level, host_capabilities::api_level);
  }

  TEST(ServerInfoLuminalShineFields, ReportsPyrowaveWhenItIsAvailable) {
    pt::ptree tree;
    nvhttp::put_luminalshine_fields(tree, {.pyrowave = true});

    const auto capabilities = write_and_read_xml(tree).get_optional<std::string>("root.LuminalShineCapabilities");
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_TRUE(has_capability(*capabilities, "pyrowave")) << *capabilities;
  }

  TEST(ServerInfoLuminalShineFields, OmitsPyrowaveWhenItIsUnavailable) {
    pt::ptree tree;
    nvhttp::put_luminalshine_fields(tree, {.pyrowave = false});

    // The element is written even when it lists nothing; clients detect support
    // by LuminalShineApiLevel, since some XML readers drop an empty element.
    const auto capabilities = write_and_read_xml(tree).get_optional<std::string>("root.LuminalShineCapabilities");
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_FALSE(has_capability(*capabilities, "pyrowave")) << *capabilities;
  }
}  // namespace

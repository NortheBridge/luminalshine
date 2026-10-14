/**
 * @file tests/unit/test_nvhttp_serverinfo.cpp
 * @brief Test the LuminalShine release version in the nvhttp /serverinfo response.
 */

// standard includes
#include <sstream>
#include <string>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

// local includes
#include "../tests_common.h"
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
    ASSERT_EQ(root.size(), 4u);
    EXPECT_EQ(root.back().first, "LuminalShineVersion");
    EXPECT_EQ(root.back().second.data(), PROJECT_VERSION);
  }
}  // namespace

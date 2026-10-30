/**
 * @file tests/unit/test_nvhttp_serverinfo.cpp
 * @brief Test the LuminalShine fields in the nvhttp /serverinfo response.
 */

// standard includes
#include <iterator>
#include <sstream>
#include <string>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

// local includes
#include "../tests_common.h"
#include "src/config.h"
#include "src/host_capabilities.h"
#include "src/nvhttp.h"
#include "src/pyrowave/backend.h"

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
   * @brief Restore `config::pyrowave.enabled` when a test that changes it ends.
   */
  struct pyrowave_enabled_guard_t {
    bool saved = config::pyrowave.enabled;

    ~pyrowave_enabled_guard_t() {
      config::pyrowave.enabled = saved;
    }
  };

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

  TEST(ServerInfoVersion, ReportsTheApiLevel) {
    pt::ptree tree;
    nvhttp::put_luminalshine_version(tree);

    const auto api_level = write_and_read_xml(tree).get_optional<int>("root.LuminalShineApiLevel");
    ASSERT_TRUE(api_level.has_value());
    EXPECT_EQ(*api_level, host_capabilities::api_level);
    EXPECT_GE(*api_level, 1);
  }

  TEST(ServerInfoVersion, ReportsTheCapabilitiesOfThisHost) {
    pt::ptree tree;
    nvhttp::put_luminalshine_version(tree);

    const host_capabilities::flags_t flags {
      .pyrowave = config::pyrowave.enabled && pyrowave::available(),
    };
    const auto capabilities = write_and_read_xml(tree).get_optional<std::string>("root.LuminalShineCapabilities");
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_EQ(*capabilities, host_capabilities::wire_list(flags));
  }

  TEST(ServerInfoVersion, OmitsPyrowaveWhenItIsDisabled) {
    pyrowave_enabled_guard_t guard;
    config::pyrowave.enabled = false;

    pt::ptree tree;
    nvhttp::put_luminalshine_version(tree);

    // The element stays, empty, so a client reads "no capabilities" rather than a missing field.
    const auto capabilities = write_and_read_xml(tree).get_optional<std::string>("root.LuminalShineCapabilities");
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_EQ(*capabilities, "");
  }
}  // namespace

/**
 * @file tests/integration/test_web_static_routes.cpp
 * @brief Every /images/<file> the Web UI references must be served by confighttp.
 *
 * getSpaEntry treats /images as a reserved prefix, so a file under it is
 * reachable only through an explicit `server.resource["^/images/...$"]`
 * route. The Mission Control brand mark shipped for months as a broken
 * image because the Web UI referenced /images/logo-luminalshine.png and
 * nothing registered it. This test keeps the three places in step: the
 * references in the Web UI sources, the routes in src/confighttp.cpp, and
 * the files under public/images/.
 */
#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

namespace {
  namespace fs = std::filesystem;

  const fs::path source_root = fs::path(SUNSHINE_SOURCE_DIR);
  const fs::path web_root = source_root / "src_assets" / "common" / "assets" / "web";
  const fs::path images_dir = web_root / "public" / "images";

  std::string slurp(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  bool is_web_source(const fs::path &path) {
    static const std::set<std::string> extensions = {".vue", ".ts", ".js", ".html", ".ejs"};
    return extensions.contains(path.extension().string());
  }

  /// Every `/images/<name>` literal in the Web UI sources (node_modules and build output excluded).
  std::set<std::string> referenced_images() {
    static const std::regex reference(R"(/images/([A-Za-z0-9_.\-]+))");
    std::set<std::string> names;
    for (auto it = fs::recursive_directory_iterator(web_root); it != fs::recursive_directory_iterator(); ++it) {
      const auto filename = it->path().filename().string();
      if (it->is_directory() && (filename == "node_modules" || filename == "dist")) {
        it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file() || !is_web_source(it->path())) {
        continue;
      }
      const std::string content = slurp(it->path());
      for (std::sregex_iterator match(content.begin(), content.end(), reference), end; match != end; ++match) {
        names.insert((*match)[1].str());
      }
    }
    return names;
  }

  /// Every `server.resource["^/images/<name>$"]` route registered in src/confighttp.cpp.
  std::set<std::string> registered_images() {
    static const std::regex route(R"DELIM(server\.resource\["\^/images/([^"$]+)\$"\])DELIM");
    const std::string content = slurp(source_root / "src" / "confighttp.cpp");
    std::set<std::string> names;
    for (std::sregex_iterator match(content.begin(), content.end(), route), end; match != end; ++match) {
      std::string name = (*match)[1].str();
      // The route is a regex: "logo\\.png" in the C++ source is the file "logo.png".
      std::erase(name, '\\');
      names.insert(name);
    }
    return names;
  }
}  // namespace

TEST(WebStaticRoutes, EveryReferencedImageHasARoute) {
  const auto referenced = referenced_images();
  ASSERT_FALSE(referenced.empty()) << "No /images/ references found under " << web_root << "; the scanner is broken";
  const auto registered = registered_images();
  ASSERT_FALSE(registered.empty()) << "No /images/ routes found in src/confighttp.cpp; the route regex is broken";

  for (const auto &name : referenced) {
    EXPECT_TRUE(registered.contains(name))
      << "/images/" << name << " is referenced by the Web UI but src/confighttp.cpp registers no route for it, "
      << "so the request 404s (getSpaEntry reserves /images). Add a server.resource[\"^/images/...$\"] route.";
  }
}

TEST(WebStaticRoutes, EveryRoutedImageShipsInPublicImages) {
  for (const auto &name : registered_images()) {
    EXPECT_TRUE(fs::is_regular_file(images_dir / name))
      << "src/confighttp.cpp routes /images/" << name << " but " << (images_dir / name) << " does not exist";
  }
  for (const auto &name : referenced_images()) {
    EXPECT_TRUE(fs::is_regular_file(images_dir / name))
      << "The Web UI references /images/" << name << " but " << (images_dir / name) << " does not exist";
  }
}

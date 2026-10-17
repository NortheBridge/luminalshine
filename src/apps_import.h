#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace confighttp {
  // Preserve extension fields while rejecting shapes that property_tree would
  // silently interpret as empty commands or environment values.
  inline void validate_apps_import(nlohmann::json &document) {
    auto require = [](bool valid) {
      if (!valid) {
        throw std::invalid_argument("Invalid apps.json structure.");
      }
    };
    require(document.is_object() && document.contains("apps") && document["apps"].is_array());
    if (!document.contains("env")) {
      document["env"] = nlohmann::json::object();
    }
    require(document["env"].is_object());
    for (const auto &value : document["env"]) {
      require(value.is_string());
    }
    for (const auto &app : document["apps"]) {
      require(app.is_object() && app.contains("name") && app["name"].is_string());
      require(!app["name"].get<std::string>().empty());
      for (const auto *key : {"cmd", "uuid", "working-dir", "image-path", "output", "playnite-id"}) {
        require(!app.contains(key) || app[key].is_string());
      }
      for (const auto *key : {"config-overrides", "lossless-scaling-custom", "lossless-scaling-recommended"}) {
        require(!app.contains(key) || app[key].is_object());
      }
      if (app.contains("detached")) {
        require(app["detached"].is_array());
        for (const auto &command : app["detached"]) {
          require(command.is_string());
        }
      }
      if (app.contains("prep-cmd")) {
        require(app["prep-cmd"].is_array());
        for (const auto &command : app["prep-cmd"]) {
          require(command.is_object());
          for (const auto *key : {"do", "undo"}) {
            require(!command.contains(key) || command[key].is_string());
          }
          require(!command.contains("elevated") || command["elevated"].is_boolean());
        }
      }
    }
  }
}  // namespace confighttp

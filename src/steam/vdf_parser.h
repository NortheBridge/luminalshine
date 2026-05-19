/**
 * @file src/steam/vdf_parser.h
 * @brief Minimal Valve KeyValues (VDF) text-format parser.
 *
 * Covers the two Steam metadata file shapes used by the library
 * auto-sync feature:
 *   - `<SteamRoot>/config/libraryfolders.vdf` (list of library roots)
 *   - `<LibraryRoot>/steamapps/appmanifest_<appid>.acf` (one per
 *     installed game)
 *
 * Both files are well-formed KeyValues v1 text: quoted-string keys,
 * either a quoted-string value or a `{`-delimited child block, with
 * optional `//` line comments and `\\` / `\"` / `\n` / `\t` escape
 * sequences in string literals.
 *
 * Binary KeyValues (used by `userdata/<steamid3>/config/shortcuts.vdf`)
 * is a different format and is deferred to PR 3.
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace steam::vdf {

  /**
   * @brief One node in a parsed VDF tree. Either a leaf (key → string
   *        value) or a block (key → list of child nodes). Children are
   *        held by value in a vector — keys can repeat within a block
   *        in well-formed VDF (rare but supported) so we don't collapse
   *        to a map.
   */
  struct Node {
    std::string key;
    std::variant<std::string, std::vector<Node>> value;

    bool is_string() const {
      return std::holds_alternative<std::string>(value);
    }

    const std::string &as_string() const {
      return std::get<std::string>(value);
    }

    const std::vector<Node> &children() const {
      return std::get<std::vector<Node>>(value);
    }

    std::vector<Node> &children() {
      return std::get<std::vector<Node>>(value);
    }

    /**
     * @brief Find the first child with key matching @p k
     *        case-insensitively. Returns nullptr if not found or if
     *        this node is a leaf. Steam writes most keys in
     *        consistent casing but third-party tools sometimes vary,
     *        so case-insensitive matches keep us robust.
     */
    const Node *find(std::string_view k) const;

    /**
     * @brief Convenience: return the string value of the first child
     *        with key @p k, or @p fallback if missing / non-leaf.
     */
    std::string find_string(std::string_view k, std::string fallback = {}) const;
  };

  /**
   * @brief Parse a UTF-8 VDF text document. Returns std::nullopt on
   *        unrecoverable syntax errors. Best-effort: trailing garbage
   *        after the top-level block is tolerated and ignored.
   *
   *        The returned node's `key` is the top-level identifier
   *        (e.g. "libraryfolders" or "AppState") and its children are
   *        the contents.
   */
  std::unique_ptr<Node> parse(std::string_view content);

}  // namespace steam::vdf

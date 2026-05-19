/**
 * @file src/steam/vdf_parser.cpp
 * @brief Implementation of the minimal text-VDF parser. Hand-rolled
 *        tokeniser + recursive-descent parser; no third-party
 *        dependencies. Designed to handle the well-formed VDF files
 *        Steam itself writes (libraryfolders.vdf, appmanifest_*.acf)
 *        and to reject garbage input cleanly rather than crash.
 */
#include "src/steam/vdf_parser.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace steam::vdf {

  const Node *Node::find(std::string_view k) const {
    if (!std::holds_alternative<std::vector<Node>>(value)) {
      return nullptr;
    }
    const auto &kids = std::get<std::vector<Node>>(value);
    auto it = std::find_if(kids.begin(), kids.end(), [&](const Node &n) {
      if (n.key.size() != k.size()) {
        return false;
      }
      for (size_t i = 0; i < n.key.size(); ++i) {
        const auto a = static_cast<unsigned char>(n.key[i]);
        const auto b = static_cast<unsigned char>(k[i]);
        if (std::tolower(a) != std::tolower(b)) {
          return false;
        }
      }
      return true;
    });
    return it == kids.end() ? nullptr : &*it;
  }

  std::string Node::find_string(std::string_view k, std::string fallback) const {
    const Node *n = find(k);
    if (!n || !n->is_string()) {
      return fallback;
    }
    return n->as_string();
  }

  namespace {

    /**
     * @brief Streaming cursor over the input text. Caller-driven; the
     *        tokeniser reads ahead via `peek` and consumes via `next`.
     */
    struct Cursor {
      std::string_view src;
      size_t pos {0};

      bool eof() const {
        return pos >= src.size();
      }

      char peek() const {
        return eof() ? '\0' : src[pos];
      }

      char peek2() const {
        return pos + 1 < src.size() ? src[pos + 1] : '\0';
      }

      char next() {
        return eof() ? '\0' : src[pos++];
      }
    };

    /// Skip whitespace, `//` line comments, and `/* ... */` block
    /// comments. Steam's writer only emits `//` but third-party tools
    /// (e.g. some Proton fork managers) emit block comments, so handle
    /// both.
    void skip_whitespace(Cursor &c) {
      for (;;) {
        while (!c.eof() && std::isspace(static_cast<unsigned char>(c.peek()))) {
          c.next();
        }
        if (c.peek() == '/' && c.peek2() == '/') {
          while (!c.eof() && c.peek() != '\n') {
            c.next();
          }
          continue;
        }
        if (c.peek() == '/' && c.peek2() == '*') {
          c.next();
          c.next();
          while (!c.eof() && !(c.peek() == '*' && c.peek2() == '/')) {
            c.next();
          }
          if (!c.eof()) {
            c.next();
            c.next();
          }
          continue;
        }
        return;
      }
    }

    /// Parse a quoted string with Steam's escape sequences. Returns
    /// nullopt if the input doesn't start with `"` or the string is
    /// unterminated.
    std::optional<std::string> parse_quoted(Cursor &c) {
      if (c.peek() != '"') {
        return std::nullopt;
      }
      c.next();
      std::string out;
      while (!c.eof()) {
        const char ch = c.next();
        if (ch == '"') {
          return out;
        }
        if (ch == '\\' && !c.eof()) {
          const char esc = c.next();
          switch (esc) {
            case 'n':
              out.push_back('\n');
              break;
            case 't':
              out.push_back('\t');
              break;
            case 'r':
              out.push_back('\r');
              break;
            case '\\':
              out.push_back('\\');
              break;
            case '"':
              out.push_back('"');
              break;
            default:
              // Steam writes literal `\\` for path separators on
              // Windows; unrecognised escapes pass through as the
              // character following the backslash, matching Valve's
              // own KeyValues reader behaviour.
              out.push_back(esc);
              break;
          }
          continue;
        }
        out.push_back(ch);
      }
      // Unterminated string — caller decides whether to fail.
      return std::nullopt;
    }

    /// Parse an unquoted identifier (whitespace- and brace-terminated).
    /// Steam emits quoted keys exclusively, but the format tolerates
    /// bareword identifiers so we accept them defensively.
    std::optional<std::string> parse_bareword(Cursor &c) {
      if (c.eof()) {
        return std::nullopt;
      }
      const char first = c.peek();
      if (first == '"' || first == '{' || first == '}' ||
          std::isspace(static_cast<unsigned char>(first))) {
        return std::nullopt;
      }
      std::string out;
      while (!c.eof()) {
        const char ch = c.peek();
        if (ch == '"' || ch == '{' || ch == '}' ||
            std::isspace(static_cast<unsigned char>(ch))) {
          break;
        }
        out.push_back(c.next());
      }
      return out.empty() ? std::nullopt : std::optional<std::string> {out};
    }

    std::optional<std::string> parse_token(Cursor &c) {
      skip_whitespace(c);
      if (c.peek() == '"') {
        return parse_quoted(c);
      }
      return parse_bareword(c);
    }

    bool parse_block(Cursor &c, std::vector<Node> &out);

    /// Parse one key + value pair. Value is either a quoted/bareword
    /// string or a brace-delimited child block. Caller has already
    /// consumed the key via parse_token.
    bool parse_value_for_key(Cursor &c, std::string key, std::vector<Node> &out) {
      skip_whitespace(c);
      if (c.peek() == '{') {
        c.next();
        Node node;
        node.key = std::move(key);
        node.value = std::vector<Node> {};
        auto &kids = std::get<std::vector<Node>>(node.value);
        if (!parse_block(c, kids)) {
          return false;
        }
        skip_whitespace(c);
        if (c.peek() != '}') {
          return false;
        }
        c.next();
        out.emplace_back(std::move(node));
        return true;
      }
      auto value = parse_token(c);
      if (!value) {
        return false;
      }
      Node node;
      node.key = std::move(key);
      node.value = std::move(*value);
      out.emplace_back(std::move(node));
      return true;
    }

    /// Parse the contents of a block (zero or more key/value pairs),
    /// stopping at `}` or EOF.
    bool parse_block(Cursor &c, std::vector<Node> &out) {
      for (;;) {
        skip_whitespace(c);
        if (c.eof() || c.peek() == '}') {
          return true;
        }
        auto key = parse_token(c);
        if (!key) {
          return false;
        }
        if (!parse_value_for_key(c, std::move(*key), out)) {
          return false;
        }
      }
    }

  }  // namespace

  std::unique_ptr<Node> parse(std::string_view content) {
    Cursor c {content, 0};
    skip_whitespace(c);
    auto key = parse_token(c);
    if (!key) {
      return nullptr;
    }
    skip_whitespace(c);
    if (c.peek() != '{') {
      return nullptr;
    }
    c.next();
    auto root = std::make_unique<Node>();
    root->key = std::move(*key);
    root->value = std::vector<Node> {};
    auto &kids = std::get<std::vector<Node>>(root->value);
    if (!parse_block(c, kids)) {
      return nullptr;
    }
    skip_whitespace(c);
    if (c.peek() != '}') {
      return nullptr;
    }
    c.next();
    return root;
  }

}  // namespace steam::vdf

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
#include <cstdint>
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

    // Maximum block-nesting depth. VDF files Steam writes are shallow; a malicious file with deeply
    // nested blocks would otherwise blow the stack via unbounded recursion (local DoS).
    constexpr int kMaxVdfDepth = 100;

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

    bool parse_block(Cursor &c, std::vector<Node> &out, int depth);

    /// Parse one key + value pair. Value is either a quoted/bareword
    /// string or a brace-delimited child block. Caller has already
    /// consumed the key via parse_token.
    bool parse_value_for_key(Cursor &c, std::string key, std::vector<Node> &out, int depth) {
      skip_whitespace(c);
      if (c.peek() == '{') {
        if (depth >= kMaxVdfDepth) {
          return false;
        }
        c.next();
        Node node;
        node.key = std::move(key);
        node.value = std::vector<Node> {};
        auto &kids = std::get<std::vector<Node>>(node.value);
        if (!parse_block(c, kids, depth + 1)) {
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
    bool parse_block(Cursor &c, std::vector<Node> &out, int depth) {
      if (depth >= kMaxVdfDepth) {
        return false;
      }
      for (;;) {
        skip_whitespace(c);
        if (c.eof() || c.peek() == '}') {
          return true;
        }
        auto key = parse_token(c);
        if (!key) {
          return false;
        }
        if (!parse_value_for_key(c, std::move(*key), out, depth)) {
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
    if (!parse_block(c, kids, 1)) {
      return nullptr;
    }
    skip_whitespace(c);
    if (c.peek() != '}') {
      return nullptr;
    }
    c.next();
    return root;
  }

  namespace {

    enum BinaryTag : std::uint8_t {
      kBinTagBlock = 0x00,
      kBinTagString = 0x01,
      kBinTagInt32 = 0x02,
      kBinTagEndBlock = 0x08,
    };

    struct BinCursor {
      const unsigned char *data;
      size_t size;
      size_t pos {0};

      bool has(size_t n) const {
        return pos + n <= size;
      }

      std::uint8_t read_u8() {
        return data[pos++];
      }

      std::int32_t read_i32_le() {
        const auto a = static_cast<std::uint32_t>(data[pos]);
        const auto b = static_cast<std::uint32_t>(data[pos + 1]);
        const auto c = static_cast<std::uint32_t>(data[pos + 2]);
        const auto d = static_cast<std::uint32_t>(data[pos + 3]);
        pos += 4;
        return static_cast<std::int32_t>(a | (b << 8) | (c << 16) | (d << 24));
      }

      /// Read a null-terminated UTF-8 string. Returns nullopt if no
      /// terminator is found before EOF (malformed input).
      std::optional<std::string> read_cstr() {
        size_t end = pos;
        while (end < size && data[end] != 0) {
          ++end;
        }
        if (end >= size) {
          return std::nullopt;
        }
        std::string out(reinterpret_cast<const char *>(data + pos), end - pos);
        pos = end + 1;  // skip the null
        return out;
      }
    };

    bool parse_bin_block(BinCursor &c, std::vector<Node> &out, int depth) {
      if (depth >= kMaxVdfDepth) {
        return false;
      }
      for (;;) {
        if (!c.has(1)) {
          return false;
        }
        const auto tag = c.read_u8();
        if (tag == kBinTagEndBlock) {
          return true;
        }
        auto key = c.read_cstr();
        if (!key) {
          return false;
        }
        Node node;
        node.key = std::move(*key);
        if (tag == kBinTagBlock) {
          node.value = std::vector<Node> {};
          if (!parse_bin_block(c, std::get<std::vector<Node>>(node.value), depth + 1)) {
            return false;
          }
        } else if (tag == kBinTagString) {
          auto val = c.read_cstr();
          if (!val) {
            return false;
          }
          node.value = std::move(*val);
        } else if (tag == kBinTagInt32) {
          if (!c.has(4)) {
            return false;
          }
          const auto v = c.read_i32_le();
          node.value = std::to_string(v);
        } else {
          // Unknown tag — fail closed rather than guess at the layout.
          return false;
        }
        out.emplace_back(std::move(node));
      }
    }

  }  // namespace

  std::unique_ptr<Node> parse_binary(std::string_view content) {
    if (content.empty()) {
      return nullptr;
    }
    BinCursor c {reinterpret_cast<const unsigned char *>(content.data()),
                 content.size(),
                 0};
    // Top-level: a single 0x00-tagged block named "shortcuts" (or
    // whatever the document's root key is). Mirror the text-parser
    // behaviour: synthesise a root Node carrying the top-level key
    // + its children.
    if (!c.has(1) || c.read_u8() != kBinTagBlock) {
      return nullptr;
    }
    auto root_key = c.read_cstr();
    if (!root_key) {
      return nullptr;
    }
    auto root = std::make_unique<Node>();
    root->key = std::move(*root_key);
    root->value = std::vector<Node> {};
    if (!parse_bin_block(c, std::get<std::vector<Node>>(root->value), 1)) {
      return nullptr;
    }
    return root;
  }

}  // namespace steam::vdf

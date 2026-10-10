#include "xml_text.h"

#include <cstdint>
#include <stdexcept>

#include "common/text.h"

namespace aa::tool {

namespace {

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  XmlNode document() {
    skip_misc();
    if (!peek('<')) fail("élément racine attendu");
    XmlNode root = element(0);
    skip_misc();
    if (pos_ < s_.size()) fail("contenu après l'élément racine");
    return root;
  }

 private:
  [[noreturn]] void fail(const std::string& what) const {
    throw std::runtime_error("ligne " + std::to_string(line_) + " : " + what);
  }

  bool peek(char c) const { return pos_ < s_.size() && s_[pos_] == c; }
  bool starts(std::string_view p) const { return s_.substr(pos_, p.size()) == p; }

  void advance(size_t n = 1) {
    for (size_t i = 0; i < n && pos_ < s_.size(); ++i) {
      if (s_[pos_] == '\n') ++line_;
      ++pos_;
    }
  }

  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) advance();
  }

  void skip_until(std::string_view end) {
    while (pos_ < s_.size() && !starts(end)) advance();
    if (pos_ >= s_.size()) fail("fin de fichier inattendue (attendu " + std::string(end) + ")");
    advance(end.size());
  }

  // Whitespace, comments, processing instructions and text.
  void skip_misc() {
    while (pos_ < s_.size()) {
      if (starts("<!--")) {
        skip_until("-->");
      } else if (starts("<?")) {
        skip_until("?>");
      } else if (starts("<!")) {
        skip_until(">");
      } else if (peek('<')) {
        return;
      } else {
        advance();
      }
    }
  }

  std::string name() {
    size_t start = pos_;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '_' || c == '-' || c == '.') {
        ++pos_;
      } else {
        break;
      }
    }
    if (start == pos_) fail("nom attendu");
    return std::string(s_.substr(start, pos_ - start));
  }

  std::string decode_entities(std::string_view raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != '&') {
        out.push_back(raw[i]);
        continue;
      }
      size_t end = raw.find(';', i);
      if (end == std::string_view::npos) fail("entité non terminée");
      std::string_view ent = raw.substr(i + 1, end - i - 1);
      if (ent == "amp") out += '&';
      else if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else if (!ent.empty() && ent[0] == '#') {
        uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                          ? uint32_t(std::stoul(std::string(ent.substr(2)), nullptr, 16))
                          : uint32_t(std::stoul(std::string(ent.substr(1))));
        std::u16string u;
        if (cp >= 0x10000) {
          cp -= 0x10000;
          u.push_back(char16_t(0xD800 + (cp >> 10)));
          u.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
          u.push_back(char16_t(cp));
        }
        out += text::utf16_to_utf8(u);
      } else {
        fail("entité inconnue &" + std::string(ent) + ";");
      }
      i = end;
    }
    return out;
  }

  XmlNode element(int depth) {
    if (depth > 64) fail("imbrication trop profonde");
    XmlNode node;
    node.line = line_;
    advance();  // '<'
    node.name = name();
    while (true) {
      skip_ws();
      if (starts("/>")) {
        advance(2);
        return node;
      }
      if (peek('>')) {
        advance();
        break;
      }
      std::string attr = name();
      skip_ws();
      if (!peek('=')) fail("'=' attendu après " + attr);
      advance();
      skip_ws();
      if (!peek('"') && !peek('\'')) fail("valeur d'attribut entre guillemets attendue");
      char q = s_[pos_];
      advance();
      size_t start = pos_;
      while (pos_ < s_.size() && s_[pos_] != q) advance();
      if (pos_ >= s_.size()) fail("valeur d'attribut non terminée");
      node.attributes.emplace_back(attr, decode_entities(s_.substr(start, pos_ - start)));
      advance();
    }
    while (true) {
      skip_misc();
      if (pos_ >= s_.size()) fail("élément <" + node.name + "> non fermé");
      if (starts("</")) {
        advance(2);
        std::string closing = name();
        if (closing != node.name) fail("</" + closing + "> ne ferme pas <" + node.name + ">");
        skip_ws();
        if (!peek('>')) fail("'>' attendu");
        advance();
        return node;
      }
      node.children.push_back(element(depth + 1));
    }
  }

  std::string_view s_;
  size_t pos_ = 0;
  int line_ = 1;
};

}  // namespace

XmlNode parse_xml_text(std::string_view text) { return Parser(text).document(); }

}  // namespace aa::tool

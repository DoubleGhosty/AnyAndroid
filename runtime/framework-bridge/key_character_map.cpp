#include "framework-bridge/key_character_map.h"

#include <cstdlib>
#include <cstring>

namespace aa::bridge {

namespace {

// AKEYCODE_* of the modifier keys (android/keycodes.h).
constexpr int32_t kKeyAltLeft = 57, kKeyAltRight = 58, kKeyShiftLeft = 59, kKeyShiftRight = 60, kKeySym = 63,
                  kKeyCtrlLeft = 113, kKeyCtrlRight = 114, kKeyCapsLock = 115, kKeyScrollLock = 116,
                  kKeyMetaLeft = 117, kKeyMetaRight = 118, kKeyFunction = 119, kKeyNumLock = 143;

constexpr int32_t kMaxKeys = 8192;  // MAX_KEYS

int32_t set_ephemeral_meta_state(int32_t mask, bool down, int32_t old_meta_state) {
  int32_t meta = down ? old_meta_state | mask
                      : old_meta_state & ~(mask | kMetaAltOn | kMetaShiftOn | kMetaCtrlOn | kMetaMetaOn);
  return normalize_meta_state(meta);
}

int32_t toggle_locked_meta_state(int32_t mask, bool down, int32_t old_meta_state) {
  return down ? old_meta_state : old_meta_state ^ mask;
}

constexpr const char* kWhitespace = " \t\r";
constexpr const char* kWhitespaceOrPropertyDelimiter = " \t\r,:";

struct Modifier {
  const char* label;
  int32_t meta_state;
};
constexpr Modifier kModifiers[] = {
    {"shift", kMetaShiftOn},   {"lshift", kMetaShiftLeftOn}, {"rshift", kMetaShiftRightOn},
    {"alt", kMetaAltOn},       {"lalt", kMetaAltLeftOn},     {"ralt", kMetaAltRightOn},
    {"ctrl", kMetaCtrlOn},     {"lctrl", kMetaCtrlLeftOn},   {"rctrl", kMetaCtrlRightOn},
    {"meta", kMetaMetaOn},     {"lmeta", kMetaMetaLeftOn},   {"rmeta", kMetaMetaRightOn},
    {"sym", kMetaSymOn},       {"fn", kMetaFunctionOn},      {"capslock", kMetaCapsLockOn},
    {"numlock", kMetaNumLockOn}, {"scrolllock", kMetaScrollLockOn},
};

// libutils' Tokenizer over a buffer.
class Tokenizer {
 public:
  Tokenizer(std::string name, std::string_view text) : name_(std::move(name)), text_(text) {}
  bool is_eof() const { return pos_ >= text_.size(); }
  bool is_eol() const { return is_eof() || text_[pos_] == '\n'; }
  char peek_char() const { return is_eof() ? '\0' : text_[pos_]; }
  char next_char() { return is_eof() ? '\0' : text_[pos_++]; }
  std::string next_token(const char* delimiters) {
    size_t start = pos_;
    while (!is_eof()) {
      char ch = text_[pos_];
      if (ch == '\n' || std::strchr(delimiters, ch)) break;
      pos_++;
    }
    return std::string(text_.substr(start, pos_ - start));
  }
  void skip_delimiters(const char* delimiters) {
    while (!is_eof()) {
      char ch = text_[pos_];
      if (ch == '\n' || !std::strchr(delimiters, ch)) break;
      pos_++;
    }
  }
  void next_line() {
    while (!is_eof()) {
      if (text_[pos_++] == '\n') {
        line_++;
        break;
      }
    }
  }
  std::string location() const { return name_ + ":" + std::to_string(line_); }

 private:
  std::string name_;
  std::string_view text_;
  size_t pos_ = 0;
  int line_ = 1;
};

}  // namespace

int32_t normalize_meta_state(int32_t meta) {
  if (meta & (kMetaAltLeftOn | kMetaAltRightOn)) meta |= kMetaAltOn;
  if (meta & (kMetaShiftLeftOn | kMetaShiftRightOn)) meta |= kMetaShiftOn;
  if (meta & (kMetaCtrlLeftOn | kMetaCtrlRightOn)) meta |= kMetaCtrlOn;
  if (meta & (kMetaMetaLeftOn | kMetaMetaRightOn)) meta |= kMetaMetaOn;
  return meta;
}

int32_t update_meta_state(int32_t key_code, bool down, int32_t meta) {
  switch (key_code) {
    case kKeyAltLeft: return set_ephemeral_meta_state(kMetaAltLeftOn, down, meta);
    case kKeyAltRight: return set_ephemeral_meta_state(kMetaAltRightOn, down, meta);
    case kKeyShiftLeft: return set_ephemeral_meta_state(kMetaShiftLeftOn, down, meta);
    case kKeyShiftRight: return set_ephemeral_meta_state(kMetaShiftRightOn, down, meta);
    case kKeySym: return set_ephemeral_meta_state(kMetaSymOn, down, meta);
    case kKeyFunction: return set_ephemeral_meta_state(kMetaFunctionOn, down, meta);
    case kKeyCtrlLeft: return set_ephemeral_meta_state(kMetaCtrlLeftOn, down, meta);
    case kKeyCtrlRight: return set_ephemeral_meta_state(kMetaCtrlRightOn, down, meta);
    case kKeyMetaLeft: return set_ephemeral_meta_state(kMetaMetaLeftOn, down, meta);
    case kKeyMetaRight: return set_ephemeral_meta_state(kMetaMetaRightOn, down, meta);
    case kKeyCapsLock: return toggle_locked_meta_state(kMetaCapsLockOn, down, meta);
    case kKeyNumLock: return toggle_locked_meta_state(kMetaNumLockOn, down, meta);
    case kKeyScrollLock: return toggle_locked_meta_state(kMetaScrollLockOn, down, meta);
    default: return meta;
  }
}

// --- Parser (KeyCharacterMap::Parser) ----------------------------------------

class KeyCharacterMap::Parser {
 public:
  Parser(KeyCharacterMap* map, Tokenizer* tokenizer, Format format, const KeyCodeLookup& lookup)
      : map_(map), t_(tokenizer), format_(format), lookup_(lookup) {}

  bool parse() {
    while (!t_->is_eof()) {
      t_->skip_delimiters(kWhitespace);
      if (!t_->is_eol() && t_->peek_char() != '#') {
        if (state_ == kTop) {
          std::string keyword = t_->next_token(kWhitespace);
          bool ok;
          if (keyword == "type") {
            t_->skip_delimiters(kWhitespace);
            ok = parse_type();
          } else if (keyword == "map") {
            t_->skip_delimiters(kWhitespace);
            ok = parse_map();
          } else if (keyword == "key") {
            t_->skip_delimiters(kWhitespace);
            ok = parse_key();
          } else {
            ok = fail("Expected keyword, got '" + keyword + "'.");
          }
          if (!ok) return false;
        } else if (!parse_key_property()) {
          return false;
        }
        t_->skip_delimiters(kWhitespace);
        if (!t_->is_eol() && t_->peek_char() != '#') return fail("Expected end of line or trailing comment.");
      }
      t_->next_line();
    }
    if (state_ != kTop) return fail("Unterminated key description at end of file.");
    if (map_->type_ == KeyboardType::kUnknown) return fail("Keyboard layout missing required keyboard 'type' declaration.");
    if (format_ == Format::kBase && map_->type_ == KeyboardType::kOverlay) {
      return fail("Base keyboard layout must specify a keyboard 'type' other than 'OVERLAY'.");
    }
    if (format_ == Format::kOverlay && map_->type_ != KeyboardType::kOverlay) {
      return fail("Overlay keyboard layout missing required keyboard 'type OVERLAY' declaration.");
    }
    return true;
  }

  std::string error;

 private:
  enum State { kTop, kKey };
  enum class PropertyKind { kLabel, kNumber, kMeta };
  struct Property {
    PropertyKind kind;
    int32_t meta_state = 0;
  };

  bool fail(const std::string& message) {
    error = t_->location() + ": " + message;
    return false;
  }

  std::optional<int32_t> key_code(const std::string& label) { return lookup_ ? lookup_(label) : std::nullopt; }

  bool parse_type() {
    if (map_->type_ != KeyboardType::kUnknown) return fail("Duplicate keyboard 'type' declaration.");
    std::string token = t_->next_token(kWhitespace);
    KeyboardType type;
    if (token == "NUMERIC") type = KeyboardType::kNumeric;
    else if (token == "PREDICTIVE") type = KeyboardType::kPredictive;
    else if (token == "ALPHA") type = KeyboardType::kAlpha;
    else if (token == "FULL") type = KeyboardType::kFull;
    else if (token == "SPECIAL_FUNCTION") type = KeyboardType::kSpecialFunction;
    else if (token == "OVERLAY") type = KeyboardType::kOverlay;
    else return fail("Expected keyboard type label, got '" + token + "'.");
    map_->type_ = type;
    return true;
  }

  bool parse_map() {
    std::string keyword = t_->next_token(kWhitespace);
    if (keyword != "key") return fail("Expected keyword after 'map', got '" + keyword + "'.");
    t_->skip_delimiters(kWhitespace);
    std::string code_token = t_->next_token(kWhitespace);
    bool usage = false;
    if (code_token == "usage") {
      usage = true;
      t_->skip_delimiters(kWhitespace);
      code_token = t_->next_token(kWhitespace);
    }
    char* end = nullptr;
    int32_t code = int32_t(std::strtol(code_token.c_str(), &end, 0));
    if (code_token.empty() || *end) return fail("Expected key number, got '" + code_token + "'.");
    std::map<int32_t, int32_t>& m = usage ? map_->keys_by_usage_code_ : map_->keys_by_scan_code_;
    if (m.count(code)) return fail("Duplicate entry for key '" + code_token + "'.");
    t_->skip_delimiters(kWhitespace);
    std::string label = t_->next_token(kWhitespace);
    std::optional<int32_t> kc = key_code(label);
    if (!kc) return fail("Expected key code label, got '" + label + "'.");
    m.insert_or_assign(code, *kc);
    return true;
  }

  bool parse_key() {
    std::string label = t_->next_token(kWhitespace);
    std::optional<int32_t> kc = key_code(label);
    if (!kc) return fail("Expected key code label, got '" + label + "'.");
    if (map_->keys_.count(*kc)) return fail("Duplicate entry for key code '" + label + "'.");
    t_->skip_delimiters(kWhitespace);
    std::string brace = t_->next_token(kWhitespace);
    if (brace != "{") return fail("Expected '{' after key code label, got '" + brace + "'.");
    key_code_ = *kc;
    map_->keys_.emplace(*kc, Key{});
    state_ = kKey;
    return true;
  }

  bool parse_key_property() {
    Key& key = map_->keys_[key_code_];
    std::string token = t_->next_token(kWhitespaceOrPropertyDelimiter);
    if (token == "}") {
      state_ = kTop;
      finish_key(key);
      return true;
    }
    std::vector<Property> properties;
    for (;;) {
      if (token == "label") {
        properties.push_back({PropertyKind::kLabel});
      } else if (token == "number") {
        properties.push_back({PropertyKind::kNumber});
      } else {
        int32_t meta;
        if (!parse_modifier(token, &meta)) return fail("Expected a property name or modifier, got '" + token + "'.");
        properties.push_back({PropertyKind::kMeta, meta});
      }
      t_->skip_delimiters(kWhitespace);
      if (!t_->is_eol()) {
        char ch = t_->next_char();
        if (ch == ':') break;
        if (ch == ',') {
          t_->skip_delimiters(kWhitespace);
          token = t_->next_token(kWhitespaceOrPropertyDelimiter);
          continue;
        }
      }
      return fail("Expected ',' or ':' after property name.");
    }

    t_->skip_delimiters(kWhitespace);
    Behavior behavior;
    bool have_character = false, have_fallback = false, have_replacement = false;
    do {
      if (t_->peek_char() == '\'') {
        char16_t character;
        if (!parse_character_literal(&character) || !character) return fail("Invalid character literal for key.");
        if (have_character) return fail("Cannot combine multiple character literals or 'none'.");
        if (have_replacement) return fail("Cannot combine character literal with replace action.");
        behavior.character = character;
        have_character = true;
      } else {
        token = t_->next_token(kWhitespace);
        if (token == "none") {
          if (have_character) return fail("Cannot combine multiple character literals or 'none'.");
          if (have_replacement) return fail("Cannot combine 'none' with replace action.");
          have_character = true;
        } else if (token == "fallback" || token == "replace") {
          bool fallback = token == "fallback";
          t_->skip_delimiters(kWhitespace);
          token = t_->next_token(kWhitespace);
          std::optional<int32_t> kc = key_code(token);
          if (!kc) return fail("Invalid key code label for " + std::string(fallback ? "fallback behavior" : "replace") + ", got '" + token + "'.");
          if (!fallback && have_character) return fail("Cannot combine character literal with replace action.");
          if (have_fallback || have_replacement) return fail("Cannot combine multiple fallback/replacement key codes.");
          if (fallback) {
            behavior.fallback_key_code = *kc;
            have_fallback = true;
          } else {
            behavior.replacement_key_code = *kc;
            have_replacement = true;
          }
        } else {
          return fail("Expected a key behavior after ':'.");
        }
      }
      t_->skip_delimiters(kWhitespace);
    } while (!t_->is_eol() && t_->peek_char() != '#');

    for (const Property& p : properties) {
      switch (p.kind) {
        case PropertyKind::kLabel:
          if (key.label) return fail("Duplicate label for key.");
          key.label = behavior.character;
          break;
        case PropertyKind::kNumber:
          if (key.number) return fail("Duplicate number for key.");
          key.number = behavior.character;
          break;
        case PropertyKind::kMeta: {
          for (const Behavior& b : key.behaviors) {
            if (b.meta_state == p.meta_state) return fail("Duplicate key behavior for modifier.");
          }
          Behavior b = behavior;
          b.meta_state = p.meta_state;
          key.behaviors.push_front(b);
          break;
        }
      }
    }
    return true;
  }

  // The default number: the key's digit, else its symbol.
  static void finish_key(Key& key) {
    if (key.number) return;
    char16_t digit = 0, symbol = 0;
    for (const Behavior& b : key.behaviors) {
      char16_t ch = b.character;
      if (!ch) continue;
      if (ch >= '0' && ch <= '9') {
        digit = ch;
      } else if (ch == '(' || ch == ')' || ch == '#' || ch == '*' || ch == '-' || ch == '+' || ch == ',' ||
                 ch == '.' || ch == '\'' || ch == ':' || ch == ';' || ch == '/') {
        symbol = ch;
      }
    }
    key.number = digit ? digit : symbol;
  }

  bool parse_modifier(const std::string& token, int32_t* out) {
    if (token == "base") {
      *out = 0;
      return true;
    }
    int32_t combined = 0;
    size_t start = 0;
    for (size_t cur = 0;; cur++) {
      if (cur == token.size() || token[cur] == '+') {
        std::string_view part(token.data() + start, cur - start);
        int32_t meta = 0;
        for (const Modifier& m : kModifiers) {
          if (part == m.label) {
            meta = m.meta_state;
            break;
          }
        }
        if (!meta || (combined & meta)) return false;
        combined |= meta;
        start = cur + 1;
        if (cur == token.size()) break;
      }
    }
    *out = combined;
    return true;
  }

  bool parse_character_literal(char16_t* out) {
    if (t_->next_char() != '\'') return false;
    char ch = t_->next_char();
    if (ch == '\\') {
      ch = t_->next_char();
      if (ch == 'n') *out = '\n';
      else if (ch == 't') *out = '\t';
      else if (ch == '\\') *out = '\\';
      else if (ch == '\'') *out = '\'';
      else if (ch == '"') *out = '"';
      else if (ch == 'u') {
        *out = 0;
        for (int i = 0; i < 4; i++) {
          ch = t_->next_char();
          int digit;
          if (ch >= '0' && ch <= '9') digit = ch - '0';
          else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
          else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
          else return false;
          *out = char16_t((*out << 4) | digit);
        }
      } else {
        return false;
      }
    } else if (ch >= 32 && ch <= 126 && ch != '\'') {
      *out = char16_t(ch);
    } else {
      return false;
    }
    if (t_->next_char() != '\'') return false;
    return t_->next_token(kWhitespace).empty();  // the whole token consumed
  }

  KeyCharacterMap* map_;
  Tokenizer* t_;
  Format format_;
  const KeyCodeLookup& lookup_;
  State state_ = kTop;
  int32_t key_code_ = 0;
};

// --- KeyCharacterMap -------------------------------------------------------

std::unique_ptr<KeyCharacterMap> KeyCharacterMap::load_contents(const std::string& name, std::string_view contents,
                                                                Format format, const KeyCodeLookup& lookup,
                                                                std::string* error) {
  auto map = std::make_unique<KeyCharacterMap>(name);
  map->lookup_ = lookup;
  if (format != Format::kOverlay) map->base_contents_ = std::string(contents);
  Tokenizer tokenizer(name, contents);
  Parser parser(map.get(), &tokenizer, format, lookup);
  if (!parser.parse()) {
    if (error) *error = parser.error;
    return nullptr;
  }
  return map;
}

const KeyCharacterMap::Key* KeyCharacterMap::key(int32_t key_code) const {
  auto it = keys_.find(key_code);
  return it == keys_.end() ? nullptr : &it->second;
}

const KeyCharacterMap::Behavior* KeyCharacterMap::key_behavior(int32_t key_code, int32_t meta_state) const {
  if (const Key* k = key(key_code)) {
    for (const Behavior& b : k->behaviors) {
      if (matches_meta_state(meta_state, b.meta_state)) return &b;
    }
  }
  return nullptr;
}

// A behavior needs its modifiers; CTRL, ALT and META of the event must match
// exactly, a behavior handling one, both or either of a left/right pair.
bool KeyCharacterMap::matches_meta_state(int32_t event_meta, int32_t behavior_meta) {
  if ((event_meta & behavior_meta) != behavior_meta) return false;
  constexpr int32_t kExact = kMetaCtrlOn | kMetaCtrlLeftOn | kMetaCtrlRightOn | kMetaAltOn | kMetaAltLeftOn |
                             kMetaAltRightOn | kMetaMetaOn | kMetaMetaLeftOn | kMetaMetaRightOn;
  int32_t unmatched = event_meta & ~behavior_meta & kExact;
  if (behavior_meta & kMetaCtrlOn) unmatched &= ~(kMetaCtrlLeftOn | kMetaCtrlRightOn);
  else if (behavior_meta & (kMetaCtrlLeftOn | kMetaCtrlRightOn)) unmatched &= ~kMetaCtrlOn;
  if (behavior_meta & kMetaAltOn) unmatched &= ~(kMetaAltLeftOn | kMetaAltRightOn);
  else if (behavior_meta & (kMetaAltLeftOn | kMetaAltRightOn)) unmatched &= ~kMetaAltOn;
  if (behavior_meta & kMetaMetaOn) unmatched &= ~(kMetaMetaLeftOn | kMetaMetaRightOn);
  else if (behavior_meta & (kMetaMetaLeftOn | kMetaMetaRightOn)) unmatched &= ~kMetaMetaOn;
  return !unmatched;
}

char16_t KeyCharacterMap::display_label(int32_t key_code) const {
  const Key* k = key(key_code);
  return k ? k->label : 0;
}

char16_t KeyCharacterMap::number(int32_t key_code) const {
  const Key* k = key(key_code);
  return k ? k->number : 0;
}

char16_t KeyCharacterMap::character(int32_t key_code, int32_t meta_state) const {
  const Behavior* b = key_behavior(key_code, meta_state);
  return b ? b->character : 0;
}

bool KeyCharacterMap::fallback_action(int32_t key_code, int32_t meta_state, FallbackAction* out) const {
  *out = FallbackAction{};
  const Behavior* b = key_behavior(key_code, meta_state);
  if (!b || !b->fallback_key_code) return false;
  out->key_code = b->fallback_key_code;
  out->meta_state = meta_state & ~b->meta_state;
  return true;
}

// The most general behavior giving one of `chars`, unless one matches the
// meta state exactly.
char16_t KeyCharacterMap::match(int32_t key_code, const char16_t* chars, size_t count, int32_t meta_state) const {
  char16_t result = 0;
  if (const Key* k = key(key_code)) {
    for (const Behavior& b : k->behaviors) {
      if (!b.character) continue;
      for (size_t i = 0; i < count; i++) {
        if (b.character == chars[i]) {
          result = b.character;
          if ((b.meta_state & meta_state) == b.meta_state) return result;
          break;
        }
      }
    }
  }
  return result;
}

bool KeyCharacterMap::find_key(char16_t ch, int32_t* out_key_code, int32_t* out_meta_state) const {
  if (!ch) return false;
  for (const auto& [code, k] : keys_) {
    const Behavior* found = nullptr;
    for (const Behavior& b : k.behaviors) {
      if (b.character == ch) found = &b;  // the most general: the last
    }
    if (found) {
      *out_key_code = code;
      *out_meta_state = found->meta_state;
      return true;
    }
  }
  return false;
}

bool KeyCharacterMap::events(const char16_t* chars, size_t count, std::vector<KeyStroke>& out) const {
  auto add = [&out](int32_t code, int32_t meta, bool down) { out.push_back({code, meta, down}); };
  auto single = [&](int32_t meta_state, bool down, int32_t code, int32_t key_meta, int32_t* current) {
    if ((meta_state & key_meta) != key_meta) return false;
    *current = update_meta_state(code, down, *current);
    add(code, *current, down);
    return true;
  };
  auto pair = [&](int32_t meta_state, bool down, int32_t left, int32_t left_meta, int32_t right, int32_t right_meta,
                  int32_t either_meta, int32_t* current) {
    bool specific = single(meta_state, down, left, left_meta, current);
    specific |= single(meta_state, down, right, right_meta, current);
    if (!specific) single(meta_state, down, left, either_meta, current);
  };
  auto locked = [&](int32_t meta_state, int32_t code, int32_t key_meta, int32_t* current) {
    if ((meta_state & key_meta) != key_meta) return;
    *current = update_meta_state(code, true, *current);
    add(code, *current, true);
    *current = update_meta_state(code, false, *current);
    add(code, *current, false);
  };
  for (size_t i = 0; i < count; i++) {
    int32_t code, meta;
    if (!find_key(chars[i], &code, &meta)) return false;
    int32_t current = 0;
    // Meta keys pressed and released symmetrically (addMetaKeys).
    locked(meta, kKeyCapsLock, kMetaCapsLockOn, &current);
    locked(meta, kKeyNumLock, kMetaNumLockOn, &current);
    locked(meta, kKeyScrollLock, kMetaScrollLockOn, &current);
    pair(meta, true, kKeyShiftLeft, kMetaShiftLeftOn, kKeyShiftRight, kMetaShiftRightOn, kMetaShiftOn, &current);
    pair(meta, true, kKeyAltLeft, kMetaAltLeftOn, kKeyAltRight, kMetaAltRightOn, kMetaAltOn, &current);
    pair(meta, true, kKeyCtrlLeft, kMetaCtrlLeftOn, kKeyCtrlRight, kMetaCtrlRightOn, kMetaCtrlOn, &current);
    pair(meta, true, kKeyMetaLeft, kMetaMetaLeftOn, kKeyMetaRight, kMetaMetaRightOn, kMetaMetaOn, &current);
    single(meta, true, kKeySym, kMetaSymOn, &current);
    single(meta, true, kKeyFunction, kMetaFunctionOn, &current);
    add(code, current, true);
    add(code, current, false);
    single(meta, false, kKeyFunction, kMetaFunctionOn, &current);
    single(meta, false, kKeySym, kMetaSymOn, &current);
    pair(meta, false, kKeyMetaLeft, kMetaMetaLeftOn, kKeyMetaRight, kMetaMetaRightOn, kMetaMetaOn, &current);
    pair(meta, false, kKeyCtrlLeft, kMetaCtrlLeftOn, kKeyCtrlRight, kMetaCtrlRightOn, kMetaCtrlOn, &current);
    pair(meta, false, kKeyAltLeft, kMetaAltLeftOn, kKeyAltRight, kMetaAltRightOn, kMetaAltOn, &current);
    pair(meta, false, kKeyShiftLeft, kMetaShiftLeftOn, kKeyShiftRight, kMetaShiftRightOn, kMetaShiftOn, &current);
    locked(meta, kKeyScrollLock, kMetaScrollLockOn, &current);
    locked(meta, kKeyNumLock, kMetaNumLockOn, &current);
    locked(meta, kKeyCapsLock, kMetaCapsLockOn, &current);
  }
  return true;
}

bool KeyCharacterMap::map_key(int32_t scan_code, int32_t usage_code, int32_t* out) const {
  if (usage_code) {
    if (auto it = keys_by_usage_code_.find(usage_code); it != keys_by_usage_code_.end()) {
      *out = it->second;
      return true;
    }
  }
  if (scan_code) {
    if (auto it = keys_by_scan_code_.find(scan_code); it != keys_by_scan_code_.end()) {
      *out = it->second;
      return true;
    }
  }
  *out = 0;  // AKEYCODE_UNKNOWN
  return false;
}

void KeyCharacterMap::combine(const KeyCharacterMap& overlay) {
  if (overlay_applied_) {
    // reloadBaseFromFile.
    keys_.clear();
    keys_by_scan_code_.clear();
    keys_by_usage_code_.clear();
    overlay_applied_ = false;
    type_ = KeyboardType::kUnknown;
    if (!base_contents_.empty()) {
      Tokenizer tokenizer(load_file_name_, base_contents_);
      Parser parser(this, &tokenizer, Format::kBase, lookup_);
      parser.parse();
    }
  }
  for (const auto& [code, k] : overlay.keys_) keys_.insert_or_assign(code, k);
  for (const auto& [from, to] : overlay.keys_by_scan_code_) keys_by_scan_code_.insert_or_assign(from, to);
  for (const auto& [from, to] : overlay.keys_by_usage_code_) keys_by_usage_code_.insert_or_assign(from, to);
  overlay_applied_ = true;
}

void KeyCharacterMap::write_to_parcel(const ParcelWriter& w) const {
  w.write_string8(load_file_name_);
  w.write_i32(int32_t(type_));
  w.write_i32(overlay_applied_ ? 1 : 0);
  w.write_i32(int32_t(keys_.size()));
  for (const auto& [code, k] : keys_) {
    w.write_i32(code);
    w.write_i32(k.label);
    w.write_i32(k.number);
    for (const Behavior& b : k.behaviors) {
      w.write_i32(1);
      w.write_i32(b.meta_state);
      w.write_i32(b.character);
      w.write_i32(b.fallback_key_code);
      w.write_i32(b.replacement_key_code);
    }
    w.write_i32(0);
  }
  w.write_i32(int32_t(key_remapping_.size()));
  for (const auto& [from, to] : key_remapping_) {
    w.write_i32(from);
    w.write_i32(to);
  }
  w.write_i32(int32_t(keys_by_scan_code_.size()));
  for (const auto& [from, to] : keys_by_scan_code_) {
    w.write_i32(from);
    w.write_i32(to);
  }
  w.write_i32(int32_t(keys_by_usage_code_.size()));
  for (const auto& [from, to] : keys_by_usage_code_) {
    w.write_i32(from);
    w.write_i32(to);
  }
}

std::unique_ptr<KeyCharacterMap> KeyCharacterMap::read_from_parcel(const ParcelReader& r) {
  auto map = std::make_unique<KeyCharacterMap>(r.read_string8());
  map->type_ = KeyboardType(r.read_i32());
  map->overlay_applied_ = r.read_i32() != 0;
  int32_t num_keys = r.read_i32();
  if (r.failed() || num_keys < 0 || num_keys > kMaxKeys) return nullptr;
  for (int32_t i = 0; i < num_keys; i++) {
    int32_t code = r.read_i32();
    Key k;
    k.label = char16_t(r.read_i32());
    k.number = char16_t(r.read_i32());
    if (r.failed()) return nullptr;
    while (r.read_i32()) {
      Behavior b;
      b.meta_state = r.read_i32();
      b.character = char16_t(r.read_i32());
      b.fallback_key_code = r.read_i32();
      b.replacement_key_code = r.read_i32();
      if (r.failed()) return nullptr;
      k.behaviors.push_back(b);
    }
    map->keys_.emplace(code, std::move(k));
    if (r.failed()) return nullptr;
  }
  auto read_pairs = [&r](auto& m) {
    int32_t n = r.read_i32();
    if (r.failed() || n < 0) return false;
    for (int32_t i = 0; i < n; i++) {
      int32_t from = r.read_i32(), to = r.read_i32();
      if (r.failed()) return false;
      m.insert_or_assign(from, to);
    }
    return true;
  };
  if (!read_pairs(map->key_remapping_) || !read_pairs(map->keys_by_scan_code_) ||
      !read_pairs(map->keys_by_usage_code_)) {
    return nullptr;
  }
  return map;
}

}  // namespace aa::bridge

#include "headless/headless_backend.h"

#include <cstdio>
#include <functional>

#include "common/text.h"

namespace aa::backend {

namespace {

std::string u8(const std::u16string& s) { return text::utf16_to_utf8(s); }

std::string quoted(const std::u16string& s) { return "\"" + text::printable(u8(s), 200) + "\""; }

// Splits "click \"Mon bouton\"" into the command word and the rest.
std::pair<std::string, std::string> split_command(const std::string& line) {
  size_t sp = line.find(' ');
  if (sp == std::string::npos) return {line, ""};
  return {line.substr(0, sp), text::trim(line.substr(sp + 1))};
}

// Reads a selector (quoted text or a single word) and returns the rest.
std::pair<std::string, std::string> take_selector(const std::string& args) {
  if (!args.empty() && args[0] == '"') {
    size_t end = args.find('"', 1);
    if (end == std::string::npos) return {args, ""};
    return {args.substr(0, end + 1), text::trim(args.substr(end + 1))};
  }
  size_t sp = args.find(' ');
  if (sp == std::string::npos) return {args, ""};
  return {args.substr(0, sp), text::trim(args.substr(sp + 1))};
}

void visit(ui::Node* n, const std::function<void(ui::Node*)>& fn) {
  if (!n) return;
  fn(n);
  for (ui::Node* c : n->children) visit(c, fn);
}

bool shown(const ui::Node* n) {
  for (; n; n = n->parent) {
    if (!n->visible || n->gone) return false;
  }
  return true;
}

}  // namespace

HeadlessBackend::HeadlessBackend(const std::string& script, std::FILE* out) : commands_(parse_script(script)), out_(out) {}

std::vector<std::string> HeadlessBackend::parse_script(const std::string& script) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quotes = false;
  auto flush = [&] {
    std::string t = text::trim(cur);
    if (!t.empty() && t[0] != '#') out.push_back(t);
    cur.clear();
  };
  for (char c : script) {
    if (c == '"') in_quotes = !in_quotes;
    if (!in_quotes && (c == ';' || c == '\n' || c == '\r')) {
      flush();
    } else {
      cur.push_back(c);
    }
  }
  flush();
  return out;
}

void HeadlessBackend::print(const std::string& line) {
  std::string l = line + "\n";
  std::fwrite(l.data(), 1, l.size(), out_);
  std::fflush(out_);
}

void HeadlessBackend::show_toast(const std::u16string& text, bool long_duration) {
  print("[toast] " + quoted(text) + (long_duration ? " (long)" : ""));
}

void HeadlessBackend::show_error(const std::u16string& title, const std::u16string& details) {
  print("[erreur] " + u8(title));
  for (const auto& line : text::split(u8(details), '\n')) {
    if (!line.empty()) print("  " + line);
  }
}

void HeadlessBackend::lifecycle_event(const std::string& activity, const std::string& event) {
  print("[cycle] " + activity + "." + event);
}

void HeadlessBackend::app_output(const std::string& line) { print(line); }

void HeadlessBackend::dump_node(const ui::Node& n, int depth) {
  std::string line(size_t(depth) * 2, ' ');
  line += n.type_name.empty() ? "View" : n.type_name;
  if (n.kind == ui::NodeKind::kGroup) line += n.orientation == ui::Orientation::kVertical ? " vertical" : " horizontal";
  if (n.kind != ui::NodeKind::kGroup && n.kind != ui::NodeKind::kView) line += " " + quoted(n.text);
  if (!n.hint.empty()) line += " indice=" + quoted(n.hint);
  if (n.id != -1) line += " #" + std::to_string(n.id);
  if (n.gone) line += " [gone]";
  else if (!n.visible) line += " [invisible]";
  if (!n.enabled) line += " [désactivé]";
  if (n.clickable && n.kind != ui::NodeKind::kButton) line += " [cliquable]";
  print(line);
  for (const ui::Node* c : n.children) dump_node(*c, depth + 1);
}

void HeadlessBackend::dump() {
  print("=== Écran « " + u8(title_) + " » ===");
  if (root_) {
    dump_node(*root_, 0);
  } else {
    print("(aucun contenu)");
  }
}

ui::Node* HeadlessBackend::select(const std::string& selector) {
  ui::Node* found = nullptr;
  if (!selector.empty() && selector[0] == '"') {
    std::u16string want = text::utf8_to_utf16(selector.substr(1, selector.size() >= 2 ? selector.size() - 2 : 0));
    visit(root_, [&](ui::Node* n) {
      if (!found && shown(n) && (n->text == want || n->hint == want)) found = n;
    });
  } else if (!selector.empty() && selector[0] == '#') {
    int32_t id = int32_t(std::stol(selector.substr(1), nullptr, 0));
    visit(root_, [&](ui::Node* n) {
      if (!found && n->id == id) found = n;
    });
  } else {
    size_t colon = selector.find(':');
    std::string type = selector.substr(0, colon);
    int index = colon == std::string::npos ? 0 : std::stoi(selector.substr(colon + 1));
    visit(root_, [&](ui::Node* n) {
      if (!found && n->type_name == type && index-- == 0) found = n;
    });
  }
  return found;
}

void HeadlessBackend::fail(const std::string& message) {
  print("[script] ÉCHEC : " + message);
  if (!quit_) {
    quit_ = true;
    code_ = 3;
  }
}

int HeadlessBackend::run(ui::EventSink& sink) {
  sink.on_tasks();
  for (const std::string& line : commands_) {
    if (quit_) break;
    auto [cmd, args] = split_command(line);
    try {
      if (cmd == "dump") {
        dump();
      } else if (cmd == "click") {
        ui::Node* n = select(args);
        if (!n) {
          fail("élément introuvable : " + args);
          break;
        }
        print("> click " + args);
        sink.on_click(n);
      } else if (cmd == "type") {
        auto [sel, value] = take_selector(args);
        ui::Node* n = select(sel);
        if (!n || n->kind != ui::NodeKind::kEdit) {
          fail("champ de saisie introuvable : " + sel);
          break;
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        print("> type " + sel + " \"" + value + "\"");
        sink.on_text_changed(n, text::utf8_to_utf16(value));
      } else if (cmd == "expect") {
        ui::Node* n = select(args);
        if (!n) {
          fail("texte attendu absent : " + args);
          break;
        }
        print("> expect " + args + " : ok");
      } else if (cmd == "back") {
        print("> back");
        sink.on_back();
      } else if (cmd == "close") {
        print("> close");
        sink.on_close();
      } else {
        fail("commande inconnue : " + cmd);
        break;
      }
    } catch (const std::exception& e) {
      fail(std::string("commande invalide : ") + line + " (" + e.what() + ")");
      break;
    }
    sink.on_tasks();
  }
  if (!quit_) {
    print("[script] fin du script : fermeture de la fenêtre");
    sink.on_close();
  }
  return code_;
}

void HeadlessBackend::quit(int code) {
  if (quit_) return;
  quit_ = true;
  code_ = code;
  print("[fin] code " + std::to_string(code));
}

}  // namespace aa::backend

// android.view.KeyCharacterMap and KeyEvent natives (J8 of ADR-006), each
// signature checked against the framework.jar of Android 17, as
// android_view_KeyCharacterMap.cpp and android_view_KeyEvent.cpp do it over
// libinput (key_character_map.h). Key code labels are the KEYCODE_ constants
// of the framework's own KeyEvent class. The host's input service loads the
// key character maps of /system/usr/keychars as system_server's InputReader
// does (aa.host.services.InputService.nativeLoadKeyCharacterMap); the virtual
// keyboard's map is kept for the input dispatcher, which types text with it.
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/clock.h"
#include "common/log.h"
#include "common/text.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/input_dispatcher.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/key_character_map.h"
#include "framework-bridge/motion_event.h"
#include "vfs/vfs.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

// NativeKeyCharacterMap: a device's map, null for an empty one.
struct NativeKeyCharacterMap {
  int32_t device_id = 0;
  std::shared_ptr<const KeyCharacterMap> map;
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<NativeKeyCharacterMap>& maps() {
  static auto* t = new HandleTable<NativeKeyCharacterMap>(0x1126);  // never destroyed
  return *t;
}

std::shared_ptr<NativeKeyCharacterMap> native_map(Runtime& rt, int64_t h) {
  auto m = maps().get(h);
  if (!m) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] KeyCharacterMap invalide");
  return m;
}

// The KEYCODE_ constants of android.view.KeyEvent, both ways.
struct KeyCodeLabels {
  std::map<std::string, int32_t, std::less<>> by_label;
  std::map<int32_t, std::string> by_code;
};

const KeyCodeLabels& key_code_labels(Runtime& rt) {
  static std::mutex lock;
  static KeyCodeLabels* labels = nullptr;  // never destroyed
  std::lock_guard<std::mutex> guard(lock);
  if (!labels) {
    auto* l = new KeyCodeLabels;
    rt::Class* c = rt.find_class("Landroid/view/KeyEvent;");
    rt.ensure_initialized(c);
    for (const auto& f : c->fields) {
      if (!f->is_static() || f->type != "I" || f->name.rfind("KEYCODE_", 0) != 0) continue;
      int32_t code = int32_t(c->statics.at(f->slot));
      std::string label = f->name.substr(8);
      l->by_label.emplace(label, code);
      l->by_code.emplace(code, label);
    }
    labels = l;
  }
  return *labels;
}

KeyCharacterMap::KeyCodeLookup lookup_of(Runtime& rt) {
  const KeyCodeLabels* labels = &key_code_labels(rt);
  return [labels](std::string_view label) -> std::optional<int32_t> {
    auto it = labels->by_label.find(label);
    if (it == labels->by_label.end()) return std::nullopt;
    return it->second;
  };
}

// android_view_KeyCharacterMap_create: a Java KeyCharacterMap over a native one.
Handle java_map(Runtime& rt, int32_t device_id, std::shared_ptr<const KeyCharacterMap> map) {
  auto native = std::make_shared<NativeKeyCharacterMap>(NativeKeyCharacterMap{device_id, std::move(map)});
  int64_t h = maps().add(std::move(native));
  return construct(rt, "Landroid/view/KeyCharacterMap;", "(J)V", {uint32_t(uint64_t(h)), uint32_t(uint64_t(h) >> 32)});
}

std::u16string chars_of(Runtime& rt, Handle array) {
  if (array == kNull) rt.throw_npe("chars");
  rt::ArrayObject& a = rt.array(array, "char[]");
  std::u16string out(a.length, u'\0');
  std::memcpy(out.data(), a.data.data(), size_t(a.length) * 2);
  return out;
}

}  // namespace

// android_view_KeyEvent_obtainAsCopy: KeyEvent.obtain(id, downTime, eventTime,
// action, code, repeat, metaState, deviceId, scanCode, flags, source,
// displayId, hmac, characters), times in nanoseconds.
rt::Handle obtain_key_event(rt::Runtime& rt, int32_t device_id, int32_t key_code, int32_t meta_state, bool down,
                            int64_t time) {
  constexpr int32_t kSourceKeyboard = 0x101, kInvalidDisplay = -1;
  rt::Handle hmac = rt.new_array("[B", 32);  // INVALID_HMAC
  rt::GlobalRef keep(rt, hmac);
  uint32_t t_lo = uint32_t(uint64_t(time)), t_hi = uint32_t(uint64_t(time) >> 32);
  return rt::Handle(rt.call_static("Landroid/view/KeyEvent;", "obtain",
                                   "(IJJIIIIIIIII[BLjava/lang/String;)Landroid/view/KeyEvent;",
                                   {uint32_t(next_input_event_id()), t_lo, t_hi, t_lo, t_hi, uint32_t(down ? 0 : 1),
                                    uint32_t(key_code), 0, uint32_t(meta_state), uint32_t(device_id), 0, 0,
                                    uint32_t(kSourceKeyboard), uint32_t(kInvalidDisplay), hmac, rt::kNull}));
}

namespace {
constexpr char kVirtualKeyboardMap[] = "/system/usr/keychars/Virtual.kcm";
constexpr int32_t kVirtualKeyboard = -1;  // KeyCharacterMap.VIRTUAL_KEYBOARD

// The virtual keyboard's map once loaded, by the input service or by
// virtual_key_character_map: one map for both.
std::mutex virtual_map_lock;
std::shared_ptr<const KeyCharacterMap>& virtual_map_slot() {
  static auto* slot = new std::shared_ptr<const KeyCharacterMap>;  // never destroyed
  return *slot;
}

// KeyCharacterMap::load of a base map of the system image: *readable is
// false when the file cannot be read; null with the parser's message in
// *error when it is malformed.
std::shared_ptr<const KeyCharacterMap> load_base_map(Runtime& rt, const std::string& path, bool* readable,
                                                     std::string* error) {
  std::vector<uint8_t> bytes;
  try {
    bytes = libcore_vfs().read_file(path, 1u << 20);
  } catch (const vfs::VfsError&) {
    *readable = false;
    return nullptr;
  }
  *readable = true;
  std::string_view contents(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return KeyCharacterMap::load_contents(path, contents, KeyCharacterMap::Format::kBase, lookup_of(rt), error);
}

void set_int_field(Runtime& rt, Handle obj, const char* name, int32_t v) {
  rt::Object& o = rt.nonnull(obj, name);
  rt::Field* f = find_field(o.klass, name, "I");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
  o.fields.at(f->slot) = uint32_t(v);
}

void register_key_character_map(rt::NativeRegistry& r) {
  const char* kcm = "Landroid/view/KeyCharacterMap;";
  r.bind(kcm, "nativeObtainEmptyKeyCharacterMap", "(I)Landroid/view/KeyCharacterMap;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_obj(java_map(rt, a.i32(0), nullptr)); });
  r.bind(kcm, "nativeDispose", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    maps().remove(a.i64(0));
    return 0;
  });
  r.bind(kcm, "nativeReadFromParcel", "(Landroid/os/Parcel;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    bool ok = true;
    int32_t device_id = parcel_read_i32(rt, a.h(0), &ok);
    if (!ok) return 0;
    std::shared_ptr<const KeyCharacterMap> map;
    if (parcel_read_i32(rt, a.h(0), &ok) && ok) {  // not a null map
      KeyCharacterMap::ParcelReader reader{
          [&] { return parcel_read_i32(rt, a.h(0), &ok); },
          [&] { return parcel_read_string8(rt, a.h(0), &ok); },
          [&] { return !ok; }};
      map = KeyCharacterMap::read_from_parcel(reader);
      if (!map) return 0;
    }
    if (!ok) return 0;
    return rt::ret_long(maps().add(std::make_shared<NativeKeyCharacterMap>(NativeKeyCharacterMap{device_id, map})));
  });
  r.bind(kcm, "nativeWriteToParcel", "(JLandroid/os/Parcel;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = maps().get(a.i64(0));
    if (!m || a.h(2) == kNull) return 0;
    parcel_write_i32(rt, a.h(2), m->device_id);
    if (!m->map) {
      parcel_write_i32(rt, a.h(2), 0);
      return 0;
    }
    parcel_write_i32(rt, a.h(2), 1);
    m->map->write_to_parcel({[&](int32_t v) { parcel_write_i32(rt, a.h(2), v); },
                             [&](const std::string& s) { parcel_write_string8(rt, a.h(2), s); }});
    return 0;
  });
  r.bind(kcm, "nativeGetCharacter", "(JII)C", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    return m->map ? uint64_t(m->map->character(a.i32(2), a.i32(3))) : 0;
  });
  r.bind(kcm, "nativeGetFallbackAction", "(JIILandroid/view/KeyCharacterMap$FallbackAction;)Z",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto m = native_map(rt, a.i64(0));
           if (!m->map) return rt::ret_bool(false);
           KeyCharacterMap::FallbackAction action;
           bool result = m->map->fallback_action(a.i32(2), a.i32(3), &action);
           if (result) {
             set_int_field(rt, a.h(4), "keyCode", action.key_code);
             set_int_field(rt, a.h(4), "metaState", action.meta_state);
           }
           return rt::ret_bool(result);
         });
  r.bind(kcm, "nativeGetNumber", "(JI)C", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    return m->map ? uint64_t(m->map->number(a.i32(2))) : 0;
  });
  r.bind(kcm, "nativeGetMatch", "(JI[CI)C", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    if (!m->map) return 0;
    std::u16string chars = chars_of(rt, a.h(3));
    return uint64_t(m->map->match(a.i32(2), chars.data(), chars.size(), a.i32(4)));
  });
  r.bind(kcm, "nativeGetDisplayLabel", "(JI)C", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    return m->map ? uint64_t(m->map->display_label(a.i32(2))) : 0;
  });
  r.bind(kcm, "nativeGetKeyboardType", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    return rt::ret_int(m->map ? int32_t(m->map->keyboard_type()) : 0);
  });
  r.bind(kcm, "nativeGetEvents", "(J[C)[Landroid/view/KeyEvent;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    std::u16string chars = chars_of(rt, a.h(2));
    std::vector<KeyCharacterMap::KeyStroke> strokes;
    if (m->map && !m->map->events(chars.data(), chars.size(), strokes)) return rt::ret_obj(kNull);
    Handle arr = rt.new_array("[Landroid/view/KeyEvent;", int32_t(strokes.size()));
    rt::GlobalRef keep(rt, arr);
    int64_t now = clock::monotonic_nanos();
    for (size_t i = 0; i < strokes.size(); i++) {
      const auto& s = strokes[i];
      Handle e = obtain_key_event(rt, m->device_id, s.key_code, s.meta_state, s.down, now);
      std::memcpy(rt.array(arr, "KeyEvent[]").data.data() + i * 4, &e, 4);
    }
    return rt::ret_obj(arr);
  });
  r.bind(kcm, "nativeEquals", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto x = native_map(rt, a.i64(0));
    auto y = native_map(rt, a.i64(2));
    if (!x->map || !y->map) return rt::ret_bool(x->map == y->map);
    return rt::ret_bool(*x->map == *y->map);
  });
  r.bind(kcm, "nativeObtainMapWithOverlay", "(JLjava/lang/String;Ljava/lang/String;)Landroid/view/KeyCharacterMap;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto m = native_map(rt, a.i64(0));
           if (!m->map || a.h(2) == kNull || a.h(3) == kNull) return rt::ret_obj(kNull);
           std::string error;
           auto overlay = KeyCharacterMap::load_contents(rt.string_utf8(a.h(2)), rt.string_utf8(a.h(3)),
                                                         KeyCharacterMap::Format::kOverlay, lookup_of(rt), &error);
           if (!overlay) return rt::ret_obj(kNull);
           auto combined = std::make_shared<KeyCharacterMap>(*m->map);
           combined->combine(*overlay);
           return rt::ret_obj(java_map(rt, m->device_id, std::move(combined)));
         });
  r.bind(kcm, "nativeGetMappedKey", "(JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = native_map(rt, a.i64(0));
    int32_t key_code = 0;
    if (!m->map || !m->map->map_key(a.i32(2), 0, &key_code)) return 0;
    return rt::ret_int(key_code);
  });

  // The host's InputReader: a device's base map read from the system image.
  r.bind("Laa/host/services/InputService;", "nativeLoadKeyCharacterMap",
         "(ILjava/lang/String;)Landroid/view/KeyCharacterMap;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(1) == kNull) rt.throw_npe("path");
           std::string path = rt.string_utf8(a.h(1));
           const bool virtual_keyboard = a.i32(0) == kVirtualKeyboard && path == kVirtualKeyboardMap;
           std::shared_ptr<const KeyCharacterMap> map;
           if (virtual_keyboard) {
             std::lock_guard<std::mutex> guard(virtual_map_lock);
             map = virtual_map_slot();
           }
           if (!map) {
             bool readable = false;
             std::string error;
             map = load_base_map(rt, path, &readable, &error);
             if (!readable) return rt::ret_obj(kNull);
             if (!map) rt.throw_new("Ljava/lang/IllegalArgumentException;", "[AnyAndroid] " + error);
             if (virtual_keyboard) {
               std::lock_guard<std::mutex> guard(virtual_map_lock);
               if (!virtual_map_slot()) virtual_map_slot() = map;
             }
           }
           return rt::ret_obj(java_map(rt, a.i32(0), std::move(map)));
         });
}

void register_key_event(rt::NativeRegistry& r) {
  const char* ke = "Landroid/view/KeyEvent;";
  r.bind(ke, "nativeKeyCodeToString", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const KeyCodeLabels& labels = key_code_labels(rt);
    auto it = labels.by_code.find(a.i32(0));
    return rt::ret_obj(it == labels.by_code.end() ? kNull : rt.new_string_utf8(it->second));
  });
  r.bind(ke, "nativeKeyCodeFromString", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("label");
    const KeyCodeLabels& labels = key_code_labels(rt);
    auto it = labels.by_label.find(rt.string_utf8(a.h(0)));
    return rt::ret_int(it == labels.by_label.end() ? 0 : it->second);  // AKEYCODE_UNKNOWN
  });
  // InputEvent::nextId, shared with MotionEvent and the input dispatcher.
  r.bind(ke, "nativeNextId", "()I", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_int(next_input_event_id());
  });
}

}  // namespace

std::shared_ptr<const KeyCharacterMap> virtual_key_character_map(Runtime& rt) {
  {
    std::lock_guard<std::mutex> guard(virtual_map_lock);
    if (virtual_map_slot()) return virtual_map_slot();
  }
  bool readable = false;
  std::string error;
  auto map = load_base_map(rt, kVirtualKeyboardMap, &readable, &error);
  if (!map) {
    log::warn("input", readable ? "carte de touches du clavier virtuel invalide : " + error
                                : "carte de touches du clavier virtuel illisible : " + std::string(kVirtualKeyboardMap));
    return nullptr;
  }
  std::lock_guard<std::mutex> guard(virtual_map_lock);
  if (!virtual_map_slot()) virtual_map_slot() = map;
  return virtual_map_slot();
}

void register_framework_input(rt::NativeRegistry& r) {
  register_key_character_map(r);
  register_key_event(r);
}

}  // namespace aa::bridge

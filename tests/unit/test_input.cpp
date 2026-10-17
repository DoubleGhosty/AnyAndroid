// libinput's KeyCharacterMap (framework-bridge/key_character_map.h): the .kcm
// syntax of Android's keyboards, modifier matching, typed characters as key
// strokes, overlays and the parcel layout.
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/key_character_map.h"
#include "framework-bridge/motion_event.h"
#include "smali.h"
#include "test.h"

using namespace aa;
using bridge::KeyCharacterMap;

namespace {

// KeyEvent's codes of the keys used below.
constexpr int32_t kA = 29, kB = 30, k1 = 8, kSpace = 62, kShiftLeft = 59, kCapsLock = 115, kDpadUp = 19;

KeyCharacterMap::KeyCodeLookup lookup() {
  static const std::map<std::string, int32_t, std::less<>> codes = {
      {"A", kA}, {"B", kB}, {"1", k1}, {"SPACE", kSpace}, {"SHIFT_LEFT", kShiftLeft},
      {"CAPS_LOCK", kCapsLock}, {"DPAD_UP", kDpadUp}};
  return [](std::string_view label) -> std::optional<int32_t> {
    auto it = codes.find(label);
    if (it == codes.end()) return std::nullopt;
    return it->second;
  };
}

// As Virtual.kcm writes its keys.
constexpr const char* kVirtual = R"(# Comment
type FULL

key A {
    label:                              'A'
    base:                               'a'
    shift, capslock:                    'A'
    shift+capslock:                     'a'
}

key 1 {
    label:                              '1'
    base:                               '1'
    shift:                              '!'
}

key SPACE {
    label:                              ' '
    base:                               ' '
    alt, meta:                          fallback SEARCH
    ctrl:                               none
}

map key 30 B
)";

std::unique_ptr<KeyCharacterMap> load(const std::string& text, KeyCharacterMap::Format format, std::string* error) {
  auto lookup_with_search = [base = lookup()](std::string_view label) -> std::optional<int32_t> {
    if (label == "SEARCH") return 84;
    return base(label);
  };
  return KeyCharacterMap::load_contents("test.kcm", text, format, lookup_with_search, error);
}

}  // namespace

TEST(input_key_character_map_parses_kcm_files) {
  std::string error;
  auto map = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  CHECK(map != nullptr);
  CHECK_EQ(error, std::string());
  CHECK(map->keyboard_type() == KeyCharacterMap::KeyboardType::kFull);
  CHECK_EQ(map->display_label(kA), char16_t('A'));
  CHECK_EQ(map->character(kA, 0), char16_t('a'));
  CHECK_EQ(map->character(kA, bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn), char16_t('A'));
  CHECK_EQ(map->character(kA, bridge::kMetaCapsLockOn), char16_t('A'));
  CHECK_EQ(map->character(kA, bridge::kMetaShiftOn | bridge::kMetaCapsLockOn), char16_t('a'));
  // CTRL, ALT and META must match exactly: no character for Ctrl+A.
  CHECK_EQ(map->character(kA, bridge::kMetaCtrlOn), char16_t(0));
  CHECK_EQ(map->number(k1), char16_t('1'));      // a digit
  CHECK_EQ(map->number(kSpace), char16_t(0));    // neither a digit nor a symbol
  CHECK_EQ(map->character(kSpace, bridge::kMetaCtrlOn), char16_t(0));  // ctrl: none
  KeyCharacterMap::FallbackAction fallback;
  CHECK(map->fallback_action(kSpace, bridge::kMetaAltOn | bridge::kMetaAltLeftOn, &fallback));
  CHECK_EQ(fallback.key_code, 84);
  CHECK_EQ(fallback.meta_state, bridge::kMetaAltLeftOn);  // the behavior's modifier removed
  int32_t mapped = 0;
  CHECK(map->map_key(30, 0, &mapped));
  CHECK_EQ(mapped, kB);
  const char16_t candidates[] = {u'!', u'x'};
  CHECK_EQ(map->match(k1, candidates, 2, 0), char16_t('!'));
}

TEST(input_key_character_map_types_characters_as_key_strokes) {
  std::string error;
  auto map = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  std::vector<KeyCharacterMap::KeyStroke> strokes;
  CHECK(map->events(u"a!", 2, strokes));
  // 'a': A down, up; '!': SHIFT_LEFT down (shift on), 1 down, 1 up, SHIFT_LEFT up.
  CHECK_EQ(strokes.size(), size_t(6));
  CHECK_EQ(strokes[0].key_code, kA);
  CHECK(strokes[0].down && !strokes[1].down);
  CHECK_EQ(strokes[2].key_code, kShiftLeft);
  CHECK_EQ(strokes[2].meta_state, bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn);
  CHECK_EQ(strokes[3].key_code, k1);
  CHECK_EQ(strokes[3].meta_state, bridge::kMetaShiftOn | bridge::kMetaShiftLeftOn);
  CHECK_EQ(strokes[5].key_code, kShiftLeft);
  CHECK(!strokes[5].down);
  CHECK_EQ(strokes[5].meta_state, 0);
  strokes.clear();
  CHECK(!map->events(u"z", 1, strokes));  // no key types 'z'
}

TEST(input_key_character_map_overlays_and_parcels) {
  std::string error;
  auto base = load(kVirtual, KeyCharacterMap::Format::kBase, &error);
  auto overlay = load("type OVERLAY\nkey A {\n  base: 'q'\n}\n", KeyCharacterMap::Format::kOverlay, &error);
  CHECK(overlay != nullptr);
  KeyCharacterMap combined = *base;
  combined.combine(*overlay);
  CHECK_EQ(combined.character(kA, 0), char16_t('q'));
  CHECK_EQ(combined.character(k1, 0), char16_t('1'));
  CHECK_EQ(base->character(kA, 0), char16_t('a'));

  std::vector<int32_t> ints;
  std::vector<std::string> strings;
  base->write_to_parcel({[&](int32_t v) { ints.push_back(v); }, [&](const std::string& s) { strings.push_back(s); }});
  size_t i = 0, j = 0;
  bool failed = false;
  auto copy = KeyCharacterMap::read_from_parcel(
      {[&] {
         if (i < ints.size()) return ints[i++];
         failed = true;
         return 0;
       },
       [&] { return j < strings.size() ? strings[j++] : std::string(); }, [&] { return failed; }});
  CHECK(copy != nullptr);
  CHECK(*copy == *base);
  CHECK_EQ(i, ints.size());
}

TEST(input_key_character_map_rejects_malformed_files) {
  std::string error;
  CHECK(load("key A {\n base: 'a'\n}\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(error.find("missing required keyboard 'type'") != std::string::npos);
  CHECK(load("type FULL\nkey NOPE {\n}\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(error.find("test.kcm:2") != std::string::npos);
  CHECK(load("type FULL\nkey A {\n base: 'a' 'b'\n}\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(load("type FULL\nkey A {\n base: 'a'\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(error.find("Unterminated") != std::string::npos);
  CHECK(load("type OVERLAY\n", KeyCharacterMap::Format::kBase, &error) == nullptr);
  CHECK(load("type FULL\nkey A {\n base: '\\u0041'\n}\n", KeyCharacterMap::Format::kBase, &error) != nullptr);
}

namespace {

void set_long_field(rt::Runtime& rt, rt::Handle obj, const char* name, int64_t v) {
  rt::Object& o = rt.nonnull(obj, name);
  rt::Field* f = o.klass->declared_field(name, "J");
  if (!f) return;
  o.fields.at(f->slot) = uint32_t(uint64_t(v));
  o.fields.at(f->slot + 1) = uint32_t(uint64_t(v) >> 32);
}

int64_t get_long_field(rt::Runtime& rt, rt::Handle obj, const char* name) {
  rt::Object& o = rt.nonnull(obj, name);
  rt::Field* f = o.klass->declared_field(name, "J");
  if (!f) return 0;
  return int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
}

struct InputVm {
  std::unique_ptr<dex::DexFile> boot;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;

  InputVm() {
    std::vector<tool::ClassDef> b;
    b.push_back(tool::assemble_smali(R"(
.class public final Landroid/view/InputChannel;
.super Ljava/lang/Object;
.field public mPtr:J
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static native nativeOpenInputChannelPair(Ljava/lang/String;)[J
.end method
.method public native nativeDispose(J)V
.end method
.method public native nativeGetToken(J)Landroid/os/IBinder;
.end method
.method public native nativeWriteToParcel(Landroid/os/Parcel;)V
.end method
.method public native nativeReadFromParcel(Landroid/os/Parcel;)V
.end method
)", "InputChannel.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public abstract Landroid/view/InputEventReceiver;
.super Ljava/lang/Object;
.field public mReceiverPtr:J
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static native nativeInit(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J
.end method
.method public static native nativeDispose(J)V
.end method
.method public static native nativeFinishInputEvent(JIZ)V
.end method
.method public static native nativeProbablyHasInput(J)Z
.end method
.method public dispatchInputEvent(ILandroid/view/InputEvent;)V
    .registers 3
    return-void
.end method
)", "InputEventReceiver.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public LTestReceiver;
.super Landroid/view/InputEventReceiver;
.field public static lastSeq:I
.field public static lastEvent:Landroid/view/InputEvent;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/view/InputEventReceiver;-><init>()V
    return-void
.end method
.method public dispatchInputEvent(ILandroid/view/InputEvent;)V
    .registers 3
    sput p1, LTestReceiver;->lastSeq:I
    sput-object p2, LTestReceiver;->lastEvent:Landroid/view/InputEvent;
    return-void
.end method
)", "TestReceiver.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public Landroid/os/Binder;
.super Ljava/lang/Object;
.implements Landroid/os/IBinder;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
)", "Binder.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public interface abstract Landroid/os/IBinder;
.super Ljava/lang/Object;
)", "IBinder.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public final Landroid/os/Parcel;
.super Ljava/lang/Object;
.field public mNativePtr:J
.method public constructor <init>()V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    invoke-static {}, Landroid/os/Parcel;->nativeCreate()J
    move-result-wide v0
    iput-wide v0, p0, Landroid/os/Parcel;->mNativePtr:J
    return-void
.end method
.method public static native nativeCreate()J
.end method
.method public static native nativeDestroy(J)V
.end method
.method public static native nativeSetDataPosition(JI)V
.end method
)", "Parcel.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public final Landroid/os/MessageQueue;
.super Ljava/lang/Object;
.field public mPtr:J
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static native nativeInit()J
.end method
.method public static native nativeDestroy(J)V
.end method
.method public native nativePollOnce(JI)V
.end method
)", "MessageQueue.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public abstract Landroid/view/InputEvent;
.super Ljava/lang/Object;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
)", "InputEvent.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public final Landroid/view/MotionEvent;
.super Landroid/view/InputEvent;
.field public mNativePtr:J
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/view/InputEvent;-><init>()V
    return-void
.end method
.method public static obtain()Landroid/view/MotionEvent;
    .registers 1
    new-instance v0, Landroid/view/MotionEvent;
    invoke-direct {v0}, Landroid/view/MotionEvent;-><init>()V
    return-object v0
.end method
)", "MotionEvent.smali"));

    b.push_back(tool::assemble_smali(R"(
.class public final Landroid/view/KeyEvent;
.super Landroid/view/InputEvent;
.field public mNativePtr:J
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Landroid/view/InputEvent;-><init>()V
    return-void
.end method
.method public static obtain(IJJIIIIIIIII[BLjava/lang/String;)Landroid/view/KeyEvent;
    .registers 17
    new-instance v0, Landroid/view/KeyEvent;
    invoke-direct {v0}, Landroid/view/KeyEvent;-><init>()V
    return-object v0
.end method
)", "KeyEvent.smali"));

    boot = dex::DexFile::open(tool::write_dex(b), "test-input.dex");
    rt = std::make_unique<rt::Runtime>();
    rt->add_boot_dex(boot.get());
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
};

}  // namespace

TEST(input_channel_and_event_receiver_pipeline) {
  InputVm vm;
  auto& rt = *vm.rt;

  // 1. Open channel pair
  rt::Handle name = rt.new_string_utf8("test-channel");
  rt::Handle pair_arr = rt::Handle(rt.call_static("Landroid/view/InputChannel;", "nativeOpenInputChannelPair",
                                                  "(Ljava/lang/String;)[J", {name}));
  CHECK(pair_arr != rt::kNull);
  auto& arr = rt.array(pair_arr, "long[]");
  CHECK_EQ(arr.length, 2u);
  int64_t server_h = 0, client_h = 0;
  std::memcpy(&server_h, arr.data.data(), sizeof(int64_t));
  std::memcpy(&client_h, arr.data.data() + sizeof(int64_t), sizeof(int64_t));
  CHECK(server_h != 0);
  CHECK(client_h != 0);
  CHECK(server_h != client_h);

  rt::Handle server_chan = bridge::construct(rt, "Landroid/view/InputChannel;", "()V", {});
  set_long_field(rt, server_chan, "mPtr", server_h);

  rt::Handle client_chan = bridge::construct(rt, "Landroid/view/InputChannel;", "()V", {});
  set_long_field(rt, client_chan, "mPtr", client_h);

  uint32_t s_lo = uint32_t(uint64_t(server_h)), s_hi = uint32_t(uint64_t(server_h) >> 32);
  uint32_t c_lo = uint32_t(uint64_t(client_h)), c_hi = uint32_t(uint64_t(client_h) >> 32);

  rt::Handle server_token = rt::Handle(rt.call_virtual(server_chan, "nativeGetToken",
                                                       "(J)Landroid/os/IBinder;", {s_lo, s_hi}));
  rt::Handle client_token = rt::Handle(rt.call_virtual(client_chan, "nativeGetToken",
                                                       "(J)Landroid/os/IBinder;", {c_lo, c_hi}));
  CHECK(server_token != rt::kNull);
  CHECK(client_token != rt::kNull);
  CHECK_EQ(server_token, client_token);

  // 2. Parcel serialization and deserialization
  rt::Handle parcel = bridge::construct(rt, "Landroid/os/Parcel;", "()V", {});
  rt.call_virtual(client_chan, "nativeWriteToParcel", "(Landroid/os/Parcel;)V", {parcel});

  int64_t parcel_native_ptr = get_long_field(rt, parcel, "mNativePtr");
  uint32_t p_lo = uint32_t(uint64_t(parcel_native_ptr)), p_hi = uint32_t(uint64_t(parcel_native_ptr) >> 32);
  rt.call_static("Landroid/os/Parcel;", "nativeSetDataPosition", "(JI)V", {p_lo, p_hi, 0});

  rt::Handle restored_chan = bridge::construct(rt, "Landroid/view/InputChannel;", "()V", {});
  rt.call_virtual(restored_chan, "nativeReadFromParcel", "(Landroid/os/Parcel;)V", {parcel});
  int64_t restored_h = get_long_field(rt, restored_chan, "mPtr");
  CHECK(restored_h != 0);

  uint32_t r_lo = uint32_t(uint64_t(restored_h)), r_hi = uint32_t(uint64_t(restored_h) >> 32);
  rt::Handle restored_token = rt::Handle(rt.call_virtual(restored_chan, "nativeGetToken",
                                                         "(J)Landroid/os/IBinder;", {r_lo, r_hi}));
  CHECK_EQ(restored_token, client_token);

  // 3. MessageQueue and InputEventReceiver
  rt::Handle mq = bridge::construct(rt, "Landroid/os/MessageQueue;", "()V", {});
  int64_t mq_ptr = int64_t(rt.call_static("Landroid/os/MessageQueue;", "nativeInit", "()J", {}));
  set_long_field(rt, mq, "mPtr", mq_ptr);
  uint32_t mq_lo = uint32_t(uint64_t(mq_ptr)), mq_hi = uint32_t(uint64_t(mq_ptr) >> 32);

  rt::Handle recv = bridge::construct(rt, "LTestReceiver;", "()V", {});
  rt::GlobalRef keep_recv(rt, recv);
  rt::Handle weak_ref = bridge::construct(rt, "Ljava/lang/ref/WeakReference;", "(Ljava/lang/Object;)V", {recv});

  int64_t recv_ptr = int64_t(rt.call_static("Landroid/view/InputEventReceiver;", "nativeInit",
                                            "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
                                            {weak_ref, client_chan, mq}));
  CHECK(recv_ptr != 0);
  set_long_field(rt, recv, "mReceiverPtr", recv_ptr);
  uint32_t recv_lo = uint32_t(uint64_t(recv_ptr)), recv_hi = uint32_t(uint64_t(recv_ptr) >> 32);

  CHECK(!rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {recv_lo, recv_hi}));

  rt::Class* tr_cls = rt.find_class("LTestReceiver;");
  CHECK(tr_cls != nullptr);
  rt::Field* seq_f = tr_cls->declared_field("lastSeq", "I");
  rt::Field* ev_f = tr_cls->declared_field("lastEvent", "Landroid/view/InputEvent;");
  CHECK(seq_f != nullptr && ev_f != nullptr);

  // 4. Post MotionEvent to server channel
  bridge::MotionEvent motion = bridge::make_touch_event(0 /* DOWN */, 120.0f, 240.0f, 1000, 1000);
  CHECK(bridge::post_channel_motion_event(rt, server_h, motion));
  CHECK(rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {recv_lo, recv_hi}));

  // Poll MessageQueue to dispatch
  rt.call_virtual(mq, "nativePollOnce", "(JI)V", {mq_lo, mq_hi, 0});
  CHECK_EQ(int32_t(tr_cls->statics.at(seq_f->slot)), 1);
  CHECK(tr_cls->statics.at(ev_f->slot) != rt::kNull);
  CHECK(!rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {recv_lo, recv_hi}));
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeFinishInputEvent", "(JIZ)V", {recv_lo, recv_hi, 1, 1});

  // 5. Post KeyEvent to server channel
  CHECK(bridge::post_channel_key_event(rt, server_h, 0, 66 /* ENTER */, 0, true, 2000));
  rt.call_virtual(mq, "nativePollOnce", "(JI)V", {mq_lo, mq_hi, 0});
  CHECK_EQ(int32_t(tr_cls->statics.at(seq_f->slot)), 2);
  CHECK(tr_cls->statics.at(ev_f->slot) != rt::kNull);
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeFinishInputEvent", "(JIZ)V", {recv_lo, recv_hi, 2, 1});

  // 6. Post window touch and key through active runtime
  bridge::set_active_runtime(&rt);
  CHECK(bridge::post_window_touch(2 /* MOVE */, 130.0f, 250.0f, 1000, 3000));
  rt.call_virtual(mq, "nativePollOnce", "(JI)V", {mq_lo, mq_hi, 0});
  CHECK_EQ(int32_t(tr_cls->statics.at(seq_f->slot)), 3);
  CHECK(tr_cls->statics.at(ev_f->slot) != rt::kNull);
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeFinishInputEvent", "(JIZ)V", {recv_lo, recv_hi, 3, 1});

  CHECK(bridge::post_window_key(0, 4 /* BACK */, 0, false, 4000));
  rt.call_virtual(mq, "nativePollOnce", "(JI)V", {mq_lo, mq_hi, 0});
  CHECK_EQ(int32_t(tr_cls->statics.at(seq_f->slot)), 4);
  CHECK(tr_cls->statics.at(ev_f->slot) != rt::kNull);
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeFinishInputEvent", "(JIZ)V", {recv_lo, recv_hi, 4, 1});

  bridge::set_active_runtime(nullptr);

  // 7. Cleanup
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeDispose", "(J)V", {recv_lo, recv_hi});
  rt.call_virtual(server_chan, "nativeDispose", "(J)V", {s_lo, s_hi});
  rt.call_virtual(client_chan, "nativeDispose", "(J)V", {c_lo, c_hi});
  rt.call_virtual(restored_chan, "nativeDispose", "(J)V", {r_lo, r_hi});
  rt.call_static("Landroid/os/MessageQueue;", "nativeDestroy", "(J)V", {mq_lo, mq_hi});
  rt.call_static("Landroid/os/Parcel;", "nativeDestroy", "(J)V", {p_lo, p_hi});
}

TEST(input_event_receiver_lifo_stack_routes_to_top_and_restores_on_dispose) {
  InputVm vm;
  auto& rt = *vm.rt;

  // Channel 1 & Receiver 1 (Main Window)
  rt::Handle name1 = rt.new_string_utf8("chan1");
  rt::Handle pair1 = rt::Handle(rt.call_static("Landroid/view/InputChannel;", "nativeOpenInputChannelPair",
                                              "(Ljava/lang/String;)[J", {name1}));
  auto& arr1 = rt.array(pair1, "long[]");
  int64_t server_h1 = 0, client_h1 = 0;
  std::memcpy(&server_h1, arr1.data.data(), sizeof(int64_t));
  std::memcpy(&client_h1, arr1.data.data() + sizeof(int64_t), sizeof(int64_t));

  rt::Handle client_chan1 = bridge::construct(rt, "Landroid/view/InputChannel;", "()V", {});
  set_long_field(rt, client_chan1, "mPtr", client_h1);

  rt::Handle mq1 = bridge::construct(rt, "Landroid/os/MessageQueue;", "()V", {});
  int64_t mq_ptr1 = int64_t(rt.call_static("Landroid/os/MessageQueue;", "nativeInit", "()J", {}));
  set_long_field(rt, mq1, "mPtr", mq_ptr1);

  rt::Handle recv1 = bridge::construct(rt, "LTestReceiver;", "()V", {});
  rt::GlobalRef keep_recv1(rt, recv1);
  rt::Handle weak_ref1 = bridge::construct(rt, "Ljava/lang/ref/WeakReference;", "(Ljava/lang/Object;)V", {recv1});

  int64_t recv_ptr1 = int64_t(rt.call_static("Landroid/view/InputEventReceiver;", "nativeInit",
                                             "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
                                             {weak_ref1, client_chan1, mq1}));
  CHECK(recv_ptr1 != 0);
  set_long_field(rt, recv1, "mReceiverPtr", recv_ptr1);
  CHECK_EQ(bridge::active_input_receivers_count(), 1u);

  // Channel 2 & Receiver 2 (Dialog / Popup Window)
  rt::Handle name2 = rt.new_string_utf8("chan2");
  rt::Handle pair2 = rt::Handle(rt.call_static("Landroid/view/InputChannel;", "nativeOpenInputChannelPair",
                                              "(Ljava/lang/String;)[J", {name2}));
  auto& arr2 = rt.array(pair2, "long[]");
  int64_t server_h2 = 0, client_h2 = 0;
  std::memcpy(&server_h2, arr2.data.data(), sizeof(int64_t));
  std::memcpy(&client_h2, arr2.data.data() + sizeof(int64_t), sizeof(int64_t));

  rt::Handle client_chan2 = bridge::construct(rt, "Landroid/view/InputChannel;", "()V", {});
  set_long_field(rt, client_chan2, "mPtr", client_h2);

  rt::Handle recv2 = bridge::construct(rt, "LTestReceiver;", "()V", {});
  rt::GlobalRef keep_recv2(rt, recv2);
  rt::Handle weak_ref2 = bridge::construct(rt, "Ljava/lang/ref/WeakReference;", "(Ljava/lang/Object;)V", {recv2});

  int64_t recv_ptr2 = int64_t(rt.call_static("Landroid/view/InputEventReceiver;", "nativeInit",
                                             "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
                                             {weak_ref2, client_chan2, mq1}));
  CHECK(recv_ptr2 != 0);
  set_long_field(rt, recv2, "mReceiverPtr", recv_ptr2);
  CHECK_EQ(bridge::active_input_receivers_count(), 2u);

  // Event routing: when dialog is open, post_window_touch routes to recv2
  bridge::set_active_runtime(&rt);
  uint32_t r2_lo = uint32_t(uint64_t(recv_ptr2)), r2_hi = uint32_t(uint64_t(recv_ptr2) >> 32);
  uint32_t r1_lo = uint32_t(uint64_t(recv_ptr1)), r1_hi = uint32_t(uint64_t(recv_ptr1) >> 32);
  CHECK(!rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {r2_lo, r2_hi}));
  CHECK(!rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {r1_lo, r1_hi}));

  CHECK(bridge::post_window_touch(0 /* DOWN */, 50.0f, 50.0f, 100, 100));
  CHECK(rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {r2_lo, r2_hi}));
  CHECK(!rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {r1_lo, r1_hi}));

  // Dismiss dialog (dispose receiver 2) -> receiver 1 automatically becomes top
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeDispose", "(J)V", {r2_lo, r2_hi});
  CHECK_EQ(bridge::active_input_receivers_count(), 1u);

  CHECK(bridge::post_window_touch(1 /* UP */, 60.0f, 60.0f, 200, 200));
  CHECK(rt.call_static("Landroid/view/InputEventReceiver;", "nativeProbablyHasInput", "(J)Z", {r1_lo, r1_hi}));

  // Cleanup
  bridge::set_active_runtime(nullptr);
  rt.call_static("Landroid/view/InputEventReceiver;", "nativeDispose", "(J)V", {r1_lo, r1_hi});
  CHECK_EQ(bridge::active_input_receivers_count(), 0u);
  rt.call_static("Landroid/os/MessageQueue;", "nativeDestroy", "(J)V",
                 {uint32_t(uint64_t(mq_ptr1)), uint32_t(uint64_t(mq_ptr1) >> 32)});
}


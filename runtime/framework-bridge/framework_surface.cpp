// android.view.SurfaceControl, SurfaceControl.Transaction and Surface natives
// (G3, ADR-006 J5), each signature checked against the framework.jar of
// Android 17. The window's surfaces are logical until the compositor (J7-J8):
// a SurfaceControl holds what SurfaceFlinger would know of a layer, and a
// Transaction collects changes that apply() makes current. Natives are bound
// as the real startup path reaches them.
#include "framework-bridge/internal.h"

#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/clock.h"
#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// What SurfaceFlinger knows of a layer.
struct Layer {
  std::mutex lock;
  std::string name;
  int32_t width = 0, height = 0, format = 0, flags = 0;
  int64_t parent = 0;
  int32_t z = 0;
  float x = 0, y = 0, alpha = 1;
  bool hidden = false;
};

// android.view.DisplayEventReceiver: the vsync of the display (60 Hz, the
// refresh rate DisplayService reports), delivered as SurfaceFlinger's event
// thread does through the receiver's Looper: scheduleVsync asks for the next
// one, which pollOnce of that Looper dispatches at its time.
constexpr int64_t kVsyncPeriod = 16666667;  // ns

struct Receiver {
  std::mutex lock;
  std::shared_ptr<rt::GlobalRef> receiver;  // WeakReference<DisplayEventReceiver>
  std::shared_ptr<rt::GlobalRef> data;      // WeakReference<VsyncEventData>
  int64_t queue = 0;                        // MessageQueue.mPtr
  bool scheduled = false;
  int32_t frame = 0;
};

// android.view.InputChannel: one end of a window's input connection. Both ends
// share the connection token (an android.os.Binder). No event flows before the
// input of J8.
struct Channel {
  std::mutex lock;
  std::string name;
  std::shared_ptr<rt::GlobalRef> token;  // created on first request, shared by dups and the peer
};

// android.view.InputEventReceiver: receives a channel's events on a Looper.
struct InputReceiver {
  std::shared_ptr<Channel> channel;
};

// Changes collected by a SurfaceControl.Transaction, applied in order.
struct Transaction {
  std::mutex lock;
  std::vector<std::function<void()>> changes;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Layer>& layers() {
  static auto* t = new HandleTable<Layer>(0x1114);  // never destroyed
  return *t;
}

HandleTable<Transaction>& transactions() {
  static auto* t = new HandleTable<Transaction>(0x1115);  // never destroyed
  return *t;
}

HandleTable<Channel>& channels() {
  static auto* t = new HandleTable<Channel>(0x1117);  // never destroyed
  return *t;
}

HandleTable<InputReceiver>& input_receivers() {
  static auto* t = new HandleTable<InputReceiver>(0x1118);  // never destroyed
  return *t;
}

std::shared_ptr<Channel> channel(Runtime& rt, int64_t h) {
  auto c = channels().get(h);
  if (!c) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] InputChannel invalide");
  return c;
}

Handle channel_token(Runtime& rt, Channel& c) {
  std::lock_guard<std::mutex> guard(c.lock);
  if (!c.token) c.token = std::make_shared<rt::GlobalRef>(rt, construct(rt, "Landroid/os/Binder;", "()V", {}));
  return c.token->get();
}

int64_t long_field(Runtime& rt, Handle obj, const char* name) {
  rt::Object& o = rt.nonnull(obj, name);
  rt::Field* f = find_field(o.klass, name, "J");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/IllegalStateException;", std::string("[AnyAndroid] champ absent : ") + name);
  return int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
}

HandleTable<Receiver>& receivers() {
  static auto* t = new HandleTable<Receiver>(0x1116);  // never destroyed
  return *t;
}

void set_long(rt::Object& o, const char* name, int64_t v) {
  rt::Field* f = find_field(o.klass, name, "J");
  if (!f || f->is_static()) return;
  o.fields.at(f->slot) = uint32_t(uint64_t(v));
  o.fields.at(f->slot + 1) = uint32_t(uint64_t(v) >> 32);
}

void set_int(rt::Object& o, const char* name, int32_t v) {
  rt::Field* f = find_field(o.klass, name, "I");
  if (f && !f->is_static()) o.fields.at(f->slot) = uint32_t(v);
}

// What SurfaceFlinger reports with a vsync: one frame timeline, presented one
// period later, the frame interval.
void fill_vsync_data(Runtime& rt, Handle data, int64_t timestamp, int32_t frame) {
  rt::Object& o = rt.nonnull(data, "VsyncEventData");
  rt::Field* timelines = find_field(o.klass, "frameTimelines", "[Landroid/view/DisplayEventReceiver$VsyncEventData$FrameTimeline;");
  if (timelines && o.fields.at(timelines->slot) != kNull) {
    rt::ArrayObject& arr = rt.array(Handle(o.fields.at(timelines->slot)), "FrameTimeline[]");
    if (arr.length > 0) {
      Handle first;
      std::memcpy(&first, arr.data.data(), 4);
      if (first != kNull) {
        rt::Object& t = rt.nonnull(first, "FrameTimeline");
        set_long(t, "vsyncId", frame);
        set_long(t, "expectedPresentationTime", timestamp + kVsyncPeriod);
        set_long(t, "deadline", timestamp + kVsyncPeriod);
      }
    }
  }
  set_int(o, "frameTimelinesLength", 1);
  set_int(o, "preferredFrameTimelineIndex", 0);
  set_long(o, "frameInterval", kVsyncPeriod);
  set_int(o, "numberQueuedBuffers", 0);
}

void deliver_vsync(Runtime& rt, int64_t handle, int64_t timestamp) {
  auto r = receivers().get(handle);
  if (!r) return;  // disposed meanwhile
  std::shared_ptr<rt::GlobalRef> receiver_ref, data_ref;
  int32_t frame;
  {
    std::lock_guard<std::mutex> guard(r->lock);
    r->scheduled = false;
    frame = ++r->frame;
    receiver_ref = r->receiver;
    data_ref = r->data;
  }
  rt::GlobalRef receiver(rt, Handle(rt.call_virtual(receiver_ref->get(), "get", "()Ljava/lang/Object;")));
  if (receiver.get() == kNull) return;  // collected
  rt::GlobalRef data(rt, Handle(rt.call_virtual(data_ref->get(), "get", "()Ljava/lang/Object;")));
  if (data.get() != kNull) fill_vsync_data(rt, data.get(), timestamp, frame);
  rt.call_virtual(receiver.get(), "dispatchVsync", "(JJI)V",
                  {uint32_t(uint64_t(timestamp)), uint32_t(uint64_t(timestamp) >> 32), 0, 0, uint32_t(frame)});
}

std::shared_ptr<Transaction> transaction(Runtime& rt, int64_t h) {
  auto t = transactions().get(h);
  if (!t) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] SurfaceControl.Transaction invalide");
  return t;
}

}  // namespace

void register_framework_surface(rt::NativeRegistry& r) {
  const char* der = "Landroid/view/DisplayEventReceiver;";
  static const int64_t receiver_free = register_free_function([](int64_t h) { receivers().remove(h); });
  r.bind(der, "nativeGetDisplayEventReceiverFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(receiver_free); });
  r.bind(der, "nativeInit", "(Ljava/lang/ref/WeakReference;Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;IJ)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull || a.h(2) == kNull) rt.throw_npe("DisplayEventReceiver.nativeInit");
           rt::Object& mq = rt.nonnull(a.h(2), "MessageQueue");
           rt::Field* ptr = find_field(mq.klass, "mPtr", "J");
           if (!ptr) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] MessageQueue sans mPtr");
           auto recv = std::make_shared<Receiver>();
           recv->receiver = std::make_shared<rt::GlobalRef>(rt, a.h(0));
           recv->data = std::make_shared<rt::GlobalRef>(rt, a.h(1));
           recv->queue = int64_t(uint64_t(mq.fields.at(ptr->slot)) | (uint64_t(mq.fields.at(ptr->slot + 1)) << 32));
           return rt::ret_long(receivers().add(std::move(recv)));
         }, S::kPartial, "vsync à 60 Hz sur l'horloge monotone, sans écran réel");
  r.bind(der, "nativeScheduleVsync", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t handle = a.i64(0);
    auto recv = receivers().get(handle);
    if (!recv) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] DisplayEventReceiver invalide");
    int64_t queue_ptr;
    {
      std::lock_guard<std::mutex> guard(recv->lock);
      if (recv->scheduled) return 0;
      recv->scheduled = true;
      queue_ptr = recv->queue;
    }
    int64_t next = (aa::clock::monotonic_nanos() / kVsyncPeriod + 1) * kVsyncPeriod;
    post_queue_callback(rt, queue_ptr, next, [handle, next](Runtime& rt2) { deliver_vsync(rt2, handle, next); });
    return 0;
  });
  r.bind(der, "nativeGetLatestVsyncEventData", "(J)Landroid/view/DisplayEventReceiver$VsyncEventData;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto recv = receivers().get(a.i64(0));
           if (!recv) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] DisplayEventReceiver invalide");
           int32_t frame;
           {
             std::lock_guard<std::mutex> guard(recv->lock);
             frame = recv->frame;
           }
           rt::GlobalRef data(rt, construct(rt, "Landroid/view/DisplayEventReceiver$VsyncEventData;", "()V", {}));
           fill_vsync_data(rt, data.get(), aa::clock::monotonic_nanos() / kVsyncPeriod * kVsyncPeriod, frame);
           return data.get();
         });

  // InputChannel: a pair whose ends share one token; a dup is the same channel.
  const char* ic = "Landroid/view/InputChannel;";
  static const int64_t channel_free = register_free_function([](int64_t h) { channels().remove(h); });
  r.bind(ic, "nativeGetFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(channel_free); });
  r.bind(ic, "nativeOpenInputChannelPair", "(Ljava/lang/String;)[J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = a.h(0) ? rt.string_utf8(a.h(0)) : "";
    auto server = std::make_shared<Channel>();
    auto client = std::make_shared<Channel>();
    server->name = name + " (server)";
    client->name = name + " (client)";
    client->token = server->token = std::make_shared<rt::GlobalRef>(rt, construct(rt, "Landroid/os/Binder;", "()V", {}));
    int64_t handles[2] = {channels().add(server), channels().add(client)};
    Handle arr = rt.new_array("[J", 2);
    std::memcpy(rt.array(arr, "long[]").data.data(), handles, sizeof handles);
    return arr;
  }, S::kPartial, "canal logique : pas d'événements d'entrée avant J8");
  // Instance natives from here: argument 0 is the InputChannel.
  r.bind(ic, "nativeDup", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(channels().add(channel(rt, a.i64(1))));
  });
  r.bind(ic, "nativeDispose", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    channels().remove(a.i64(1));
    return 0;
  });
  r.bind(ic, "nativeIsValid", "(J)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(channels().get(a.i64(1)) != nullptr);
  });
  r.bind(ic, "nativeGetName", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = channel(rt, a.i64(1));
    std::lock_guard<std::mutex> guard(c->lock);
    return rt.new_string_utf8(c->name);
  });
  r.bind(ic, "nativeGetToken", "(J)Landroid/os/IBinder;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return channel_token(rt, *channel(rt, a.i64(1)));
  });

  // InputEventReceiver: bound to a channel and a Looper; nothing arrives before J8.
  const char* ier = "Landroid/view/InputEventReceiver;";
  r.bind(ier, "nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(1) == kNull) rt.throw_npe("inputChannel");
           auto recv = std::make_shared<InputReceiver>();
           recv->channel = channel(rt, long_field(rt, a.h(1), "mPtr"));
           return rt::ret_long(input_receivers().add(std::move(recv)));
         }, S::kPartial, "pas d'événements d'entrée avant J8");
  r.bind(ier, "nativeDispose", "(JZ)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    input_receivers().remove(a.i64(0));
    return 0;
  });
  r.bind(ier, "nativeFinishInputEvent", "(JIZ)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ier, "nativeConsumeBatchedInputEvents", "(JJ)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ier, "nativeProbablyHasInput", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ier, "nativeGetToken", "(J)Landroid/os/IBinder;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto recv = input_receivers().get(a.i64(0));
    if (!recv) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] InputEventReceiver invalide");
    return channel_token(rt, *recv->channel);
  });

  const char* sc = "Landroid/view/SurfaceControl;";
  static const int64_t layer_free = register_free_function([](int64_t h) { layers().remove(h); });
  static const int64_t transaction_free = register_free_function([](int64_t h) { transactions().remove(h); });
  r.bind(sc, "nativeGetNativeSurfaceControlFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(layer_free); });
  r.bind(sc, "nativeGetNativeTransactionFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(transaction_free); });
  r.bind(sc, "nativeCreateTransaction", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(transactions().add(std::make_shared<Transaction>()));
  });
  r.bind(sc, "nativeApplyTransaction", "(JZZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = transaction(rt, a.i64(0));
    std::vector<std::function<void()>> changes;
    {
      std::lock_guard<std::mutex> guard(t->lock);
      changes.swap(t->changes);
    }
    for (auto& change : changes) change();
    return 0;
  }, S::kPartial, "couches logiques : pas de compositeur avant J7-J8");
  r.bind(sc, "nativeClearTransaction", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = transaction(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(t->lock);
    t->changes.clear();
    return 0;
  });
  r.bind(sc, "nativeMergeTransaction", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto dst = transaction(rt, a.i64(0));
    auto src = transaction(rt, a.i64(2));
    if (dst == src) return 0;
    std::scoped_lock guard(dst->lock, src->lock);
    for (auto& change : src->changes) dst->changes.push_back(std::move(change));
    src->changes.clear();
    return 0;
  });
}

}  // namespace aa::bridge

// android.view.SurfaceControl, SurfaceControl.Transaction and Surface natives
// (G3, ADR-006 J5), each signature checked against the framework.jar of
// Android 17. The window's surfaces are logical until the compositor (J7-J8):
// a SurfaceControl holds what SurfaceFlinger would know of a layer, and a
// Transaction collects changes that apply() makes current. Natives are bound
// as the real startup path reaches them.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/clock.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/motion_event.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// What SurfaceFlinger knows of a layer. Copies of a SurfaceControl share it.
struct Layer {
  std::mutex lock;
  int32_t id = 0;  // the layer id, also its handle (the binder token SurfaceFlinger gives)
  std::string name;
  int32_t width = 0, height = 0, format = 0, flags = 0;
  int32_t parent = 0;           // id of the parent layer, 0 for none
  int32_t relative_to = 0;      // id of the layer z is relative to, 0 for none
  int32_t z = 0;
  float x = 0, y = 0, alpha = 1;
  bool hidden = false;
  int32_t transform_hint = 0;
  int32_t buffer_width = 0, buffer_height = 0;  // default buffer size (BLASTBufferQueue)
  // The other properties a transaction set, as the raw values of their
  // setter (by native name), for the compositor of J7-J8.
  std::map<std::string, std::vector<uint32_t>> properties;
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
// share the connection token (an android.os.Binder).
struct InputReceiver;

struct Channel : std::enable_shared_from_this<Channel> {
  std::mutex lock;
  std::string name;
  std::shared_ptr<rt::GlobalRef> token;  // created on first request, shared by dups and the peer
  std::weak_ptr<Channel> peer;
  std::weak_ptr<InputReceiver> receiver;
};

// android.view.InputEventReceiver: receives a channel's events on a Looper.
struct QueuedInputEvent {
  int32_t seq = 0;
  std::shared_ptr<MotionEvent> motion;
  struct Key {
    int32_t device_id = 0;
    int32_t key_code = 0;
    int32_t meta_state = 0;
    bool down = false;
    int64_t time = 0;
  };
  std::optional<Key> key;
};

struct InputReceiver : std::enable_shared_from_this<InputReceiver> {
  std::mutex lock;
  std::shared_ptr<Channel> channel;
  int64_t queue = 0;                        // MessageQueue.mPtr
  std::shared_ptr<rt::GlobalRef> receiver;  // WeakReference<InputEventReceiver>
  std::deque<QueuedInputEvent> pending_events;
  std::map<int32_t, bool> in_flight;        // seq -> handled
  bool dispatch_scheduled = false;
};

// Listeners of a transaction: TransactionCommittedListener and the
// Consumer<TransactionStats> of addTransactionCompletedListener.
struct Callbacks {
  std::vector<std::shared_ptr<rt::GlobalRef>> committed, completed;
  void take(Callbacks& o) {
    for (auto& c : o.committed) committed.push_back(std::move(c));
    for (auto& c : o.completed) completed.push_back(std::move(c));
    o.committed.clear();
    o.completed.clear();
  }
};

// A window's buffer queue (BLASTBufferQueue): the Surfaces it gives draw
// into one persistent buffer (what Surface::lock's copy-back of the previous
// frame amounts to); each post is a frame presented on the layer, with the
// transactions merged into it.
struct BufferQueue {
  std::mutex lock;
  std::string name;
  std::shared_ptr<Layer> layer;
  int32_t width = 0, height = 0, format = 0;
  int64_t frame_number = 0;          // of the last frame posted (acquired at once)
  std::shared_ptr<PixelBuffer> pixels = std::make_shared<PixelBuffer>();  // width x height
  bool has_frame = false;            // a frame of the current size was posted
  std::vector<std::function<void()>> next_frame_changes;  // mergeWithNextTransaction
  Callbacks next_frame_callbacks;
};

// A Surface (ANativeWindow) of a buffer queue. Locking gives another
// reference to the same surface, released after the post.
struct NativeSurface {
  std::shared_ptr<BufferQueue> queue;
  std::mutex lock;
  bool locked = false;
};

// Changes collected by a SurfaceControl.Transaction, applied in order.
struct Transaction {
  std::mutex lock;
  int64_t id = 0;  // getId(): unique in the process, as SurfaceComposerClient's
  std::vector<std::function<void()>> changes;
  Callbacks callbacks;
};

int64_t next_transaction_id() {
  static std::atomic<int64_t> next{1};
  return next.fetch_add(1);
}

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Layer>& layers() {
  static auto* t = new HandleTable<Layer>(0x1114);  // never destroyed
  return *t;
}

HandleTable<Transaction>& transactions() {
  static auto* t = new HandleTable<Transaction>(0x1115);  // never destroyed
  return *t;
}

HandleTable<BufferQueue>& queues() {
  static auto* t = new HandleTable<BufferQueue>(0x111E);  // never destroyed
  return *t;
}

HandleTable<NativeSurface>& surfaces() {
  static auto* t = new HandleTable<NativeSurface>(0x111F);  // never destroyed
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

static std::mutex active_receiver_lock;
static std::vector<std::weak_ptr<InputReceiver>> s_active_receivers;
static rt::Runtime* s_active_runtime = nullptr;

void register_active_input_receiver(const std::shared_ptr<InputReceiver>& recv) {
  std::lock_guard<std::mutex> guard(active_receiver_lock);
  s_active_receivers.erase(
      std::remove_if(s_active_receivers.begin(), s_active_receivers.end(),
                     [&](const std::weak_ptr<InputReceiver>& wp) {
                       auto sp = wp.lock();
                       return !sp || sp == recv;
                     }),
      s_active_receivers.end());
  s_active_receivers.push_back(recv);
}

void unregister_active_input_receiver(const std::shared_ptr<InputReceiver>& recv) {
  std::lock_guard<std::mutex> guard(active_receiver_lock);
  s_active_receivers.erase(
      std::remove_if(s_active_receivers.begin(), s_active_receivers.end(),
                     [&](const std::weak_ptr<InputReceiver>& wp) {
                       auto sp = wp.lock();
                       return !sp || sp == recv;
                     }),
      s_active_receivers.end());
}

std::shared_ptr<InputReceiver> get_active_input_receiver() {
  std::lock_guard<std::mutex> guard(active_receiver_lock);
  while (!s_active_receivers.empty()) {
    if (auto sp = s_active_receivers.back().lock()) {
      return sp;
    }
    s_active_receivers.pop_back();
  }
  return nullptr;
}

void deliver_input_events(Runtime& rt, std::shared_ptr<InputReceiver> recv) {
  std::vector<QueuedInputEvent> to_dispatch;
  std::shared_ptr<rt::GlobalRef> weak_ref;
  {
    std::lock_guard<std::mutex> guard(recv->lock);
    recv->dispatch_scheduled = false;
    while (!recv->pending_events.empty()) {
      to_dispatch.push_back(std::move(recv->pending_events.front()));
      recv->pending_events.pop_front();
    }
    weak_ref = recv->receiver;
  }
  if (!weak_ref || weak_ref->get() == kNull) return;
  Handle java_recv = Handle(rt.call_virtual(weak_ref->get(), "get", "()Ljava/lang/Object;"));
  if (java_recv == kNull) return;

  for (const auto& ev : to_dispatch) {
    Handle java_event = kNull;
    if (ev.motion) {
      java_event = obtain_motion_event(rt, *ev.motion);
    } else if (ev.key) {
      java_event = obtain_key_event(rt, ev.key->device_id, ev.key->key_code, ev.key->meta_state,
                                    ev.key->down, ev.key->time);
    }
    if (java_event == kNull) continue;
    rt.call_virtual(java_recv, "dispatchInputEvent", "(ILandroid/view/InputEvent;)V",
                    {uint32_t(ev.seq), java_event});
  }
}

void schedule_dispatch(Runtime& rt, std::shared_ptr<InputReceiver> recv) {
  int64_t queue_ptr = 0;
  {
    std::lock_guard<std::mutex> guard(recv->lock);
    if (recv->dispatch_scheduled || recv->pending_events.empty()) return;
    recv->dispatch_scheduled = true;
    queue_ptr = recv->queue;
  }
  post_queue_callback(rt, queue_ptr, 0, [recv](Runtime& rt2) {
    deliver_input_events(rt2, recv);
  });
}

std::shared_ptr<Transaction> transaction(Runtime& rt, int64_t h) {
  auto t = transactions().get(h);
  if (!t) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] SurfaceControl.Transaction invalide");
  return t;
}

std::shared_ptr<Layer> layer(Runtime& rt, int64_t h) {
  auto l = layers().get(h);
  if (!l) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] SurfaceControl invalide");
  return l;
}

std::shared_ptr<BufferQueue> queue(Runtime& rt, int64_t h) {
  auto q = queues().get(h);
  if (!q) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] BLASTBufferQueue invalide");
  return q;
}

// A signaled fence (SyncFence's native): its signal time, references held
// by the Java objects that share it (nIncRef).
struct Fence {
  int64_t signal_time = 0;
  std::atomic<int32_t> refs{1};
};

HandleTable<Fence>& fences() {
  static auto* t = new HandleTable<Fence>(0x1120);  // never destroyed
  return *t;
}

void release_fence(int64_t h) {
  auto f = fences().get(h);
  if (f && f->refs.fetch_sub(1) == 1) fences().remove(h);
}

// TransactionCompletedListener: the transaction is committed, then
// presented at once (latch and present at the same time).
void run_callbacks(Runtime& rt, const Callbacks& callbacks) {
  for (const auto& listener : callbacks.committed) {
    rt.call_virtual(listener->get(), "onTransactionCommitted", "()V");
  }
  if (callbacks.completed.empty()) return;
  int64_t now = clock::monotonic_nanos();
  for (const auto& listener : callbacks.completed) {
    auto f = std::make_shared<Fence>();
    f->signal_time = now;
    int64_t fh = fences().add(std::move(f));
    Handle stats = construct(rt, "Landroid/view/SurfaceControl$TransactionStats;", "(JJ)V",
                             {uint32_t(uint64_t(now)), uint32_t(uint64_t(now) >> 32), uint32_t(uint64_t(fh)),
                              uint32_t(uint64_t(fh) >> 32)});
    rt.call_virtual(listener->get(), "accept", "(Ljava/lang/Object;)V", {stats});
  }
}

// PixelFormat values whose buffers are opaque (OPAQUE, RGBX_8888, RGB_888,
// RGB_565): BLASTBufferQueue gives them an RGBX buffer.
bool opaque_format(int32_t format) { return format == -1 || format == 2 || format == 3 || format == 4; }

int32_t rect_field(Runtime& rt, Handle rect, const char* name) {
  rt::Object& o = rt.nonnull(rect, "Rect");
  rt::Field* f = find_field(o.klass, name, "I");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
  return int32_t(o.fields.at(f->slot));
}

// The host's frame listener (set_frame_listener).
std::mutex frame_listener_lock;
std::shared_ptr<std::function<void(const PostedFrame&)>>& frame_listener_slot() {
  static auto* slot = new std::shared_ptr<std::function<void(const PostedFrame&)>>;  // never destroyed
  return *slot;
}
std::shared_ptr<std::function<void(const PostedFrame&)>> frame_listener() {
  std::lock_guard<std::mutex> guard(frame_listener_lock);
  return frame_listener_slot();
}

static std::mutex active_buffer_queues_lock;
static std::vector<std::weak_ptr<BufferQueue>> active_buffer_queues;

void register_buffer_queue(rt::NativeRegistry& r) {
  const char* bq = "Landroid/graphics/BLASTBufferQueue;";
  r.bind(bq, "nativeCreate", "(Ljava/lang/String;Z)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto q = std::make_shared<BufferQueue>();
    if (a.h(0) != kNull) q->name = rt.string_utf8(a.h(0));
    {
      std::lock_guard<std::mutex> guard(active_buffer_queues_lock);
      active_buffer_queues.push_back(q);
    }
    return rt::ret_long(queues().add(std::move(q)));
  }, S::kPartial, "tampon logiciel unique ; présentation à l'écran en J8");
  r.bind(bq, "nativeDestroy", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto q = queues().get(a.i64(0));
    if (q) {
      std::lock_guard<std::mutex> guard(active_buffer_queues_lock);
      active_buffer_queues.erase(
          std::remove_if(active_buffer_queues.begin(), active_buffer_queues.end(),
                         [&](const std::weak_ptr<BufferQueue>& wp) {
                           auto sp = wp.lock();
                           return !sp || sp == q;
                         }),
          active_buffer_queues.end());
    }
    queues().remove(a.i64(0));
    return 0;
  });
  // update(SurfaceControl, width, height, format): a new size drops the
  // frame kept for copy-back.
  r.bind(bq, "nativeUpdate", "(JJJJI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto q = queue(rt, a.i64(0));
    auto l = layer(rt, a.i64(2));
    int64_t w = a.i64(4), h = a.i64(6);
    if (w < 0 || h < 0 || w > 16384 || h > 16384) rt.throw_new("Ljava/lang/IllegalArgumentException;", "taille de tampon");
    {
      std::lock_guard<std::mutex> guard(q->lock);
      q->layer = l;
      q->format = a.i32(8);
      if (q->width != int32_t(w) || q->height != int32_t(h)) {
        q->width = int32_t(w);
        q->height = int32_t(h);
        // A new buffer: a canvas still drawing keeps the old one.
        q->pixels = std::make_shared<PixelBuffer>(PixelBuffer{std::vector<uint32_t>(size_t(w) * size_t(h), 0)});
        q->has_frame = false;
      }
    }
    std::lock_guard<std::mutex> guard(l->lock);
    l->buffer_width = int32_t(w);
    l->buffer_height = int32_t(h);
    return 0;
  });
  r.bind(bq, "nativeGetSurface", "(JZ)Landroid/view/Surface;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = std::make_shared<NativeSurface>();
    s->queue = queue(rt, a.i64(0));
    int64_t h = surfaces().add(std::move(s));
    return rt::ret_obj(construct(rt, "Landroid/view/Surface;", "(J)V", {uint32_t(uint64_t(h)), uint32_t(uint64_t(h) >> 32)}));
  });
  r.bind(bq, "nativeIsSameSurfaceControl", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto q = queue(rt, a.i64(0));
    auto l = layer(rt, a.i64(2));
    std::lock_guard<std::mutex> guard(q->lock);
    return rt::ret_bool(q->layer == l);
  });
  r.bind(bq, "nativeGetLastAcquiredFrameNum", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto q = queue(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(q->lock);
    return rt::ret_long(q->frame_number);
  });
  // The transaction joins the next frame's.
  r.bind(bq, "nativeMergeWithNextTransaction", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto q = queue(rt, a.i64(0));
    auto t = transaction(rt, a.i64(2));
    std::scoped_lock guard(q->lock, t->lock);
    for (auto& change : t->changes) q->next_frame_changes.push_back(std::move(change));
    t->changes.clear();
    q->next_frame_callbacks.take(t->callbacks);
    return 0;
  });
  // Frames are acquired as they are posted: nothing waits for a frame number.
  r.bind(bq, "nativeApplyPendingTransactions", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    queue(rt, a.i64(0));
    return 0;
  });
  for (const char* name : {"nativeStopContinuousSyncTransaction", "nativeClearSyncTransaction"}) {
    r.bind(bq, name, "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      queue(rt, a.i64(0));
      return 0;
    });
  }
  // Callbacks of a compositor that is not there: never called.
  r.bind(bq, "nativeSetTransactionHangCallback", "(JLandroid/gui/ITransactionHangCallback;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           queue(rt, a.i64(0));
           return 0;
         }, S::kPartial, "jamais rappelé : pas de compositeur");
  r.bind(bq, "nativeSetWaitForBufferReleaseCallback",
         "(JLandroid/graphics/BLASTBufferQueue$WaitForBufferReleaseCallback;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           queue(rt, a.i64(0));
           return 0;
         }, S::kPartial, "jamais rappelé : un tampon toujours libre");
  r.bind(bq, "nativeSetApplyToken", "(JLandroid/os/IBinder;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    queue(rt, a.i64(0));
    return 0;
  });

  const char* sf = "Landroid/view/Surface;";
  auto with_surface = [](auto fn) {
    return [fn](Runtime& rt, NativeArgs& a) -> uint64_t { return fn(rt, surfaces().get(a.i64(0)), a); };
  };
  r.bind(sf, "nativeRelease", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    surfaces().remove(a.i64(0));
    return 0;
  });
  r.bind(sf, "nativeIsValid", "(J)Z", with_surface([](Runtime&, std::shared_ptr<NativeSurface> s, NativeArgs&) {
    return rt::ret_bool(s && s->queue);
  }));
  r.bind(sf, "nativeGetWidth", "(J)I", with_surface([](Runtime&, std::shared_ptr<NativeSurface> s, NativeArgs&) {
    if (!s || !s->queue) return rt::ret_int(0);
    std::lock_guard<std::mutex> guard(s->queue->lock);
    return rt::ret_int(s->queue->width);
  }));
  r.bind(sf, "nativeGetHeight", "(J)I", with_surface([](Runtime&, std::shared_ptr<NativeSurface> s, NativeArgs&) {
    if (!s || !s->queue) return rt::ret_int(0);
    std::lock_guard<std::mutex> guard(s->queue->lock);
    return rt::ret_int(s->queue->height);
  }));
  r.bind(sf, "nativeGetNextFrameNumber", "(J)J", with_surface([](Runtime&, std::shared_ptr<NativeSurface> s, NativeArgs&) {
    if (!s || !s->queue) return rt::ret_long(0);
    std::lock_guard<std::mutex> guard(s->queue->lock);
    return rt::ret_long(s->queue->frame_number + 1);
  }));
  // Surface::lock: the dirty rectangle within the buffer, the whole buffer
  // when no frame of this size can be copied back; the canvas draws into
  // the buffer, clipped to it.
  r.bind(sf, "nativeLockCanvas", "(JLandroid/graphics/Canvas;Landroid/graphics/Rect;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = surfaces().get(a.i64(0));
           if (!s || !s->queue) rt.throw_new("Ljava/lang/IllegalArgumentException;", "");
           Handle dirty_rect = a.h(3);
           int32_t width, height, format;
           bool has_frame;
           PixelTarget target;
           {
             std::lock_guard<std::mutex> guard(s->queue->lock);
             width = s->queue->width, height = s->queue->height, format = s->queue->format;
             has_frame = s->queue->has_frame;
             target.pixels = s->queue->pixels;
           }
           target.width = width;
           target.height = height;
           target.opaque = opaque_format(format);
           {
             std::lock_guard<std::mutex> guard(s->lock);
             if (s->locked) rt.throw_new("Ljava/lang/IllegalArgumentException;", "surface déjà verrouillée");
             s->locked = true;
           }
           int32_t dirty[4] = {0, 0, width, height};
           if (dirty_rect != kNull && has_frame) {
             dirty[0] = std::max(0, rect_field(rt, dirty_rect, "left"));
             dirty[1] = std::max(0, rect_field(rt, dirty_rect, "top"));
             dirty[2] = std::min(width, rect_field(rt, dirty_rect, "right"));
             dirty[3] = std::min(height, rect_field(rt, dirty_rect, "bottom"));
             if (dirty[0] >= dirty[2] || dirty[1] >= dirty[3]) dirty[0] = dirty[1] = dirty[2] = dirty[3] = 0;
           }
           canvas_target_surface(rt, a.h(2), target, dirty_rect != kNull ? dirty : nullptr);
           if (dirty_rect != kNull) {
             rt::Object& o = rt.nonnull(dirty_rect, "Rect");
             set_int(o, "left", dirty[0]);
             set_int(o, "top", dirty[1]);
             set_int(o, "right", dirty[2]);
             set_int(o, "bottom", dirty[3]);
           }
           // The same surface with one more reference, released after the post.
           surfaces().retain(a.i64(0));
           return rt::ret_long(a.i64(0));
         });
  r.bind(sf, "nativeUnlockCanvasAndPost", "(JLandroid/graphics/Canvas;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = surfaces().get(a.i64(0));
    if (!s || !s->queue) rt.throw_new("Ljava/lang/IllegalArgumentException;", "");
    canvas_target_surface(rt, a.h(2), PixelTarget{}, nullptr);
    {
      std::lock_guard<std::mutex> guard(s->lock);
      if (!s->locked) rt.throw_new("Ljava/lang/IllegalStateException;", "surface non verrouillée");
      s->locked = false;
    }
    std::vector<std::function<void()>> changes;
    Callbacks callbacks;
    std::shared_ptr<std::function<void(const PostedFrame&)>> listener = frame_listener();
    PostedFrame frame;
    std::shared_ptr<Layer> layer;
    {
      std::lock_guard<std::mutex> guard(s->queue->lock);
      s->queue->frame_number++;
      s->queue->has_frame = true;
      changes.swap(s->queue->next_frame_changes);
      callbacks.take(s->queue->next_frame_callbacks);
      if (listener) {
        frame.frame_number = s->queue->frame_number;
        frame.width = s->queue->width;
        frame.height = s->queue->height;
        frame.opaque = opaque_format(s->queue->format);
        frame.pixels = std::make_shared<const std::vector<uint32_t>>(s->queue->pixels->data);
        layer = s->queue->layer;
      }
    }
    for (auto& change : changes) change();
    run_callbacks(rt, callbacks);
    if (listener) {
      struct LayerItem {
        std::shared_ptr<const std::vector<uint32_t>> pixels;
        int32_t width = 0, height = 0, format = 0;
        int32_t id = 0, z = 0, flags = 0;
        float x = 0.0f, y = 0.0f, alpha = 1.0f;
        std::string name;
      };
      std::vector<LayerItem> visible_layers;
      {
        std::lock_guard<std::mutex> guard(active_buffer_queues_lock);
        active_buffer_queues.erase(
            std::remove_if(active_buffer_queues.begin(), active_buffer_queues.end(),
                           [](const std::weak_ptr<BufferQueue>& wp) { return wp.expired(); }),
            active_buffer_queues.end());

        for (auto& wp : active_buffer_queues) {
          if (auto cur_q = wp.lock()) {
            std::lock_guard<std::mutex> qguard(cur_q->lock);
            if (!cur_q->has_frame || cur_q->width <= 0 || cur_q->height <= 0 || !cur_q->pixels ||
                !cur_q->layer) {
              continue;
            }
            std::lock_guard<std::mutex> lguard(cur_q->layer->lock);
            if (cur_q->layer->hidden || cur_q->layer->alpha <= 0.0f) {
              continue;
            }
            LayerItem item;
            item.pixels = std::make_shared<const std::vector<uint32_t>>(cur_q->pixels->data);
            item.width = cur_q->width;
            item.height = cur_q->height;
            item.format = cur_q->format;
            item.id = cur_q->layer->id;
            item.z = cur_q->layer->z;
            item.flags = cur_q->layer->flags;
            item.x = cur_q->layer->x;
            item.y = cur_q->layer->y;
            item.alpha = cur_q->layer->alpha;
            item.name = cur_q->layer->name;
            visible_layers.push_back(std::move(item));
          }
        }
      }

      if (visible_layers.size() <= 1) {
        if (!visible_layers.empty()) {
          const auto& top = visible_layers.front();
          frame.frame_number = s->queue->frame_number;
          frame.width = top.width;
          frame.height = top.height;
          frame.opaque = opaque_format(top.format);
          frame.pixels = top.pixels;
          frame.layer_id = top.id;
          frame.layer_name = top.name;
        } else if (layer) {
          std::lock_guard<std::mutex> guard(layer->lock);
          frame.layer_id = layer->id;
          frame.layer_name = layer->name;
        }
        (*listener)(frame);
      } else {
        std::stable_sort(visible_layers.begin(), visible_layers.end(),
                         [](const LayerItem& item_a, const LayerItem& item_b) {
                           if (item_a.z != item_b.z) return item_a.z < item_b.z;
                           return item_a.id < item_b.id;
                         });

        int32_t canvas_w = 1080;
        int32_t canvas_h = 1920;
        for (const auto& item : visible_layers) {
          canvas_w = std::max(canvas_w, item.width);
          canvas_h = std::max(canvas_h, item.height);
        }

        std::vector<uint32_t> composite_pixels(size_t(canvas_w) * size_t(canvas_h), 0);
        SkBitmap comp_bitmap;
        comp_bitmap.installPixels(
            SkImageInfo::Make(canvas_w, canvas_h, kN32_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()),
            composite_pixels.data(), size_t(canvas_w) * 4);
        SkCanvas canvas(comp_bitmap);
        canvas.clear(SK_ColorBLACK);

        for (size_t i = 0; i < visible_layers.size(); ++i) {
          const auto& item = visible_layers[i];
          if (i > 0 && ((item.flags & 0x02) != 0 || item.z > 0)) {
            SkPaint dim_paint;
            dim_paint.setColor(SkColorSetARGB(153, 0, 0, 0));
            dim_paint.setBlendMode(SkBlendMode::kSrcOver);
            canvas.drawRect(SkRect::MakeWH(SkScalar(canvas_w), SkScalar(canvas_h)), dim_paint);
          }

          if (item.pixels && item.width > 0 && item.height > 0 &&
              item.pixels->size() >= size_t(item.width) * size_t(item.height)) {
            SkAlphaType at = opaque_format(item.format) ? kOpaque_SkAlphaType : kPremul_SkAlphaType;
            SkPixmap pixmap(SkImageInfo::Make(item.width, item.height, kN32_SkColorType, at, SkColorSpace::MakeSRGB()),
                            item.pixels->data(), size_t(item.width) * 4);
            auto* keep = new std::shared_ptr<const std::vector<uint32_t>>(item.pixels);
            sk_sp<SkImage> img = SkImages::RasterFromPixmap(
                pixmap, [](const void*, void* ctx) { delete static_cast<std::shared_ptr<const std::vector<uint32_t>>*>(ctx); },
                keep);
            if (img) {
              SkPaint layer_paint;
              if (item.alpha < 1.0f) {
                layer_paint.setAlphaf(std::clamp(item.alpha, 0.0f, 1.0f));
              }
              canvas.drawImage(img, item.x, item.y, SkSamplingOptions(), &layer_paint);
            }
          }
        }

        frame.frame_number = s->queue->frame_number;
        frame.width = canvas_w;
        frame.height = canvas_h;
        frame.opaque = true;
        frame.pixels = std::make_shared<const std::vector<uint32_t>>(std::move(composite_pixels));
        frame.layer_id = s->queue->layer ? s->queue->layer->id : 0;
        frame.layer_name = "composited";
        (*listener)(frame);
      }
    }
    return 0;
  }, S::kPartial, "compositeur multi-couches logiciel (J9)");
}

int32_t next_layer_id() {
  static std::atomic<int32_t> next{1};
  return next.fetch_add(1);
}

// Queues a change of a layer in a transaction (argument 0), the layer
// being argument 2.
void queue_change(Runtime& rt, NativeArgs& a, std::function<void(Layer&)> change) {
  auto t = transaction(rt, a.i64(0));
  auto l = layer(rt, a.i64(2));
  std::lock_guard<std::mutex> guard(t->lock);
  t->changes.push_back([l, change = std::move(change)] {
    std::lock_guard<std::mutex> layer_guard(l->lock);
    change(*l);
  });
}

// A Transaction setter (J transaction, J layer, then numbers) whose values
// are kept as they are for the compositor.
void bind_layer_property(rt::NativeRegistry& r, const char* cls, const char* name, const char* sig) {
  std::string key = name;
  r.bind(cls, name, sig, [key](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<uint32_t> values;
    for (uint32_t i = 4; i < a.count(); ++i) values.push_back(a.raw(i));
    queue_change(rt, a, [key, values](Layer& l) { l.properties[key] = values; });
    return 0;
  }, S::kPartial, "état de couche logique, appliqué sans compositeur (J7-J8)");
}

}  // namespace

void set_frame_listener(std::function<void(const PostedFrame&)> listener) {
  std::lock_guard<std::mutex> guard(frame_listener_lock);
  frame_listener_slot() =
      listener ? std::make_shared<std::function<void(const PostedFrame&)>>(std::move(listener)) : nullptr;
}

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
    server->peer = client;
    client->peer = server;
    int64_t handles[2] = {channels().add(server), channels().add(client)};
    Handle arr = rt.new_array("[J", 2);
    std::memcpy(rt.array(arr, "long[]").data.data(), handles, sizeof handles);
    return arr;
  });
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
  r.bind(ic, "nativeWriteToParcel", "(Landroid/os/Parcel;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle channel_obj = a.h(0);
    Handle parcel_obj = a.h(1);
    int64_t channel_h = long_field(rt, channel_obj, "mPtr");
    if (channel_h == 0 || !channels().get(channel_h)) {
      parcel_write_i32(rt, parcel_obj, 0);
      return 0;
    }
    parcel_write_i32(rt, parcel_obj, 1);
    parcel_write_i64(rt, parcel_obj, channel_h);
    return 0;
  });
  r.bind(ic, "nativeReadFromParcel", "(Landroid/os/Parcel;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle channel_obj = a.h(0);
    Handle parcel_obj = a.h(1);
    bool ok = true;
    int32_t init = parcel_read_i32(rt, parcel_obj, &ok);
    if (!ok || !init) return 0;
    int64_t h = parcel_read_i64(rt, parcel_obj, &ok);
    if (!ok) return 0;
    auto c = channel(rt, h);
    set_long(rt.nonnull(channel_obj, "InputChannel"), "mPtr", channels().add(c));
    return 0;
  });

  // InputEventReceiver: bound to a channel and a Looper (J8).
  const char* ier = "Landroid/view/InputEventReceiver;";
  r.bind(ier, "nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull || a.h(1) == kNull || a.h(2) == kNull) rt.throw_npe("InputEventReceiver.nativeInit");
           rt::Object& mq = rt.nonnull(a.h(2), "MessageQueue");
           rt::Field* ptr = find_field(mq.klass, "mPtr", "J");
           if (!ptr) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] MessageQueue sans mPtr");
           auto chan = channel(rt, long_field(rt, a.h(1), "mPtr"));
           auto recv = std::make_shared<InputReceiver>();
           recv->channel = chan;
           recv->receiver = std::make_shared<rt::GlobalRef>(rt, a.h(0));
           recv->queue = int64_t(uint64_t(mq.fields.at(ptr->slot)) | (uint64_t(mq.fields.at(ptr->slot + 1)) << 32));
           int64_t h = input_receivers().add(recv);
           {
             std::lock_guard<std::mutex> guard(chan->lock);
             chan->receiver = recv;
           }
           register_active_input_receiver(recv);
           return rt::ret_long(h);
         });
  r.bind(ier, "nativeDispose", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto recv = input_receivers().get(a.i64(0));
    if (recv) {
      unregister_active_input_receiver(recv);
      if (recv->channel) {
        std::lock_guard<std::mutex> guard(recv->channel->lock);
        recv->channel->receiver.reset();
      }
    }
    input_receivers().remove(a.i64(0));
    return 0;
  });
  r.bind(ier, "nativeFinishInputEvent", "(JIZ)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto recv = input_receivers().get(a.i64(0));
    if (recv) {
      std::lock_guard<std::mutex> guard(recv->lock);
      recv->in_flight[a.i32(2)] = a.z(3);
    }
    return 0;
  });
  r.bind(ier, "nativeConsumeBatchedInputEvents", "(JJ)Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ier, "nativeProbablyHasInput", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto recv = input_receivers().get(a.i64(0));
    if (!recv) return 0;
    std::lock_guard<std::mutex> guard(recv->lock);
    return rt::ret_bool(!recv->pending_events.empty());
  });
  r.bind(ier, "nativeReportTimeline", "(JIJJ)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(ier, "nativeDump", "(JLjava/lang/String;)Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8("");
  });
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
  // SurfaceComposerClient::createSurface: a layer of the application's own
  // window (Android 17 creates it client side, then hands it to relayout).
  r.bind(sc, "nativeCreate", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIIIJLandroid/os/Parcel;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto l = std::make_shared<Layer>();
           l->id = next_layer_id();
           l->name = rt.string_utf8(Handle(a.h(1)));
           l->width = a.i32(2);
           l->height = a.i32(3);
           l->format = a.i32(4);
           l->flags = a.i32(5);
           if (int64_t parent = a.i64(6)) l->parent = layer(rt, parent)->id;
           return rt::ret_long(layers().add(std::move(l)));
         }, S::kPartial, "couche logique : pas de compositeur avant J7-J8");
  r.bind(sc, "nativeCopyFromSurfaceControl", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(layers().add(layer(rt, a.i64(0))));
  });
  r.bind(sc, "nativeGetHandle", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(layer(rt, a.i64(0))->id);
  });
  r.bind(sc, "nativeGetLayerId", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(layer(rt, a.i64(0))->id);
  });
  r.bind(sc, "nativeGetName", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto l = layer(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(l->lock);
    return rt::ret_obj(rt.new_string_utf8(l->name));
  });
  r.bind(sc, "nativeSetTransformHint", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto l = layer(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(l->lock);
    l->transform_hint = a.i32(2);
    return 0;
  });
  r.bind(sc, "nativeGetTransformHint", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto l = layer(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(l->lock);
    return rt::ret_int(l->transform_hint);
  });
  r.bind(sc, "nativeUpdateDefaultBufferSize", "(JII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto l = layer(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(l->lock);
    l->buffer_width = a.i32(2);
    l->buffer_height = a.i32(3);
    return 0;
  });
  r.bind(sc, "nativeCreateTransaction", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    auto t = std::make_shared<Transaction>();
    t->id = next_transaction_id();
    return rt::ret_long(transactions().add(std::move(t)));
  });
  r.bind(sc, "nativeGetTransactionId", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(transaction(rt, a.i64(0))->id);
  });
  // Transaction-wide hints that only steer SurfaceFlinger's scheduling.
  for (const char* name : {"nativeSetFrameTimelineVsync", "nativeSetDesiredPresentTimeNanos"}) {
    r.bind(sc, name, "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      transaction(rt, a.i64(0));
      return 0;
    }, S::kPartial, "indication d'ordonnancement du compositeur, sans effet ici");
  }
  r.bind(sc, "nativeSetAnimationTransaction", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    transaction(rt, a.i64(0));
    return 0;
  }, S::kPartial, "indication d'ordonnancement du compositeur, sans effet ici");
  r.bind(sc, "nativeSetPosition", "(JJFF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    float x = a.f32(4), y = a.f32(5);
    queue_change(rt, a, [x, y](Layer& l) { l.x = x, l.y = y; });
    return 0;
  });
  r.bind(sc, "nativeSetAlpha", "(JJF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    float alpha = a.f32(4);
    queue_change(rt, a, [alpha](Layer& l) { l.alpha = alpha; });
    return 0;
  });
  r.bind(sc, "nativeSetLayer", "(JJI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t z = a.i32(4);
    queue_change(rt, a, [z](Layer& l) { l.z = z, l.relative_to = 0; });
    return 0;
  });
  r.bind(sc, "nativeSetRelativeLayer", "(JJJI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t relative = layer(rt, a.i64(4))->id, z = a.i32(6);
    queue_change(rt, a, [relative, z](Layer& l) { l.z = z, l.relative_to = relative; });
    return 0;
  });
  r.bind(sc, "nativeReparent", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t parent = a.i64(4) ? layer(rt, a.i64(4))->id : 0;
    queue_change(rt, a, [parent](Layer& l) { l.parent = parent; });
    return 0;
  });
  // layer_state_t::eLayerHidden is the bit 0x01 of the flags.
  r.bind(sc, "nativeSetFlags", "(JJII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t flags = a.i32(4), mask = a.i32(5);
    queue_change(rt, a, [flags, mask](Layer& l) {
      l.flags = (l.flags & ~mask) | (flags & mask);
      l.hidden = (l.flags & 0x01) != 0;
    });
    return 0;
  });
  static const struct {
    const char* name;
    const char* sig;
  } kProperties[] = {
      {"nativeSetBackgroundBlurRadius", "(JJI)V"}, {"nativeSetBackgroundBlurScale", "(JJF)V"},
      {"nativeSetBlurThrottlingTargetFps", "(JJI)V"}, {"nativeSetBufferTransform", "(JJI)V"},
      {"nativeSetCachingHint", "(JJI)V"}, {"nativeSetCanOccludePresentation", "(JJZ)V"},
      {"nativeSetClientDrawnCornerRadius", "(JJFFFFFFFF)V"}, {"nativeSetColorSpaceAgnostic", "(JJZ)V"},
      {"nativeSetCompositionFilterFlag", "(JJI)V"}, {"nativeSetContentPriority", "(JJI)V"},
      {"nativeSetCornerRadius", "(JJF)V"}, {"nativeSetCornerRadius", "(JJFFFF)V"}, {"nativeSetCrop", "(JJFFFF)V"},
      {"nativeSetDataSpace", "(JJI)V"}, {"nativeSetDefaultFrameRateCompatibility", "(JJI)V"},
      {"nativeSetDesiredHdrHeadroom", "(JJF)V"}, {"nativeSetDesiredMaxHdrHeadroom", "(JJF)V"},
      {"nativeSetDestinationFrame", "(JJIIII)V"}, {"nativeSetDimmingEnabled", "(JJZ)V"},
      {"nativeSetDropInputMode", "(JJI)V"}, {"nativeSetEdgeExtensionEffect", "(JJZZZZ)V"},
      {"nativeSetExtendedRangeBrightness", "(JJFF)V"}, {"nativeSetFixedTransformHint", "(JJI)V"},
      {"nativeSetFrameRate", "(JJFII)V"}, {"nativeSetFrameRateCategory", "(JJIZ)V"},
      {"nativeSetFrameRateSelectionPriority", "(JJI)V"}, {"nativeSetFrameRateSelectionStrategy", "(JJI)V"},
      {"nativeSetLayerStack", "(JJI)V"}, {"nativeSetMatrix", "(JJFFFF)V"}, {"nativeSetPictureProfileId", "(JJJ)V"},
      {"nativeSetScale", "(JJFF)V"}, {"nativeSetShadowRadius", "(JJF)V"},
      {"nativeSetStretchEffect", "(JJFFFFFFFFFF)V"}, {"nativeSetSystemContentPriority", "(JJI)V"},
      {"nativeSetTrustedOverlay", "(JJI)V"}, {"nativeSetWindowCrop", "(JJIIII)V"},
      {"nativeToggleRoundedCornerOpt", "(JJZ)V"}, {"nativeUnsetBuffer", "(JJ)V"},
  };
  for (const auto& p : kProperties) bind_layer_property(r, sc, p.name, p.sig);
  register_buffer_queue(r);
  r.bind(sc, "nativeApplyTransaction", "(JZZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto t = transaction(rt, a.i64(0));
    std::vector<std::function<void()>> changes;
    Callbacks callbacks;
    {
      std::lock_guard<std::mutex> guard(t->lock);
      changes.swap(t->changes);
      callbacks.take(t->callbacks);
    }
    for (auto& change : changes) change();
    run_callbacks(rt, callbacks);
    return 0;
  }, S::kPartial, "couches logiques : pas de compositeur avant J7-J8");
  r.bind(sc, "nativeAddTransactionCommittedListener", "(JLandroid/view/SurfaceControl$TransactionCommittedListener;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto t = transaction(rt, a.i64(0));
           if (a.h(2) == kNull) rt.throw_npe("listener");
           auto listener = std::make_shared<rt::GlobalRef>(rt, a.h(2));
           std::lock_guard<std::mutex> guard(t->lock);
           t->callbacks.committed.push_back(std::move(listener));
           return 0;
         }, S::kPartial, "rappelé à l'application de la transaction");
  r.bind(sc, "nativeAddTransactionCompletedListener", "(JLjava/util/function/Consumer;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto t = transaction(rt, a.i64(0));
           if (a.h(2) == kNull) rt.throw_npe("listener");
           auto listener = std::make_shared<rt::GlobalRef>(rt, a.h(2));
           std::lock_guard<std::mutex> guard(t->lock);
           t->callbacks.completed.push_back(std::move(listener));
           return 0;
         }, S::kPartial, "rappelé à l'application, avec une barrière déjà signalée");
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
    dst->callbacks.take(src->callbacks);
    return 0;
  });
  // Transaction::sanitize drops what the sending process may not change
  // (privileged layer flags...): the changes of an application are kept.
  r.bind(sc, "nativeSanitize", "(JII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    transaction(rt, a.i64(0));
    return 0;
  });

  // android.hardware.SyncFence: fences of a compositor that presents at once,
  // signaled when created.
  const char* fence = "Landroid/hardware/SyncFence;";
  static const int64_t fence_free = register_free_function([](int64_t h) { release_fence(h); });
  r.bind(fence, "nGetDestructor", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(fence_free); });
  r.bind(fence, "nIncRef", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = fences().get(a.i64(0));
    if (!f) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] SyncFence invalide");
    f->refs.fetch_add(1);
    return 0;
  });
  r.bind(fence, "nIsValid", "(J)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(fences().get(a.i64(0)) != nullptr);
  });
  r.bind(fence, "nWait", "(JJ)Z", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(fences().get(a.i64(0)) != nullptr);
  });
  r.bind(fence, "nGetSignalTime", "(J)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto f = fences().get(a.i64(0));
    return rt::ret_long(f ? f->signal_time : -1);
  });
  r.bind(fence, "nGetFd", "(J)I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); },
         S::kPartial, "barrière sans descripteur");
}

void set_active_runtime(rt::Runtime* rt) {
  std::lock_guard<std::mutex> guard(active_receiver_lock);
  s_active_runtime = rt;
}

size_t active_input_receivers_count() {
  std::lock_guard<std::mutex> guard(active_receiver_lock);
  size_t count = 0;
  for (const auto& wp : s_active_receivers) {
    if (!wp.expired()) ++count;
  }
  return count;
}

int32_t next_input_seq() {
  static std::atomic<int32_t> counter{1};
  int32_t seq = counter.fetch_add(1);
  return seq <= 0 ? 1 : seq;
}

bool post_channel_motion_event(Runtime& rt, int64_t channel_handle, const MotionEvent& event) {
  auto ch = channels().get(channel_handle);
  if (!ch) return false;
  std::shared_ptr<InputReceiver> target_receiver;
  {
    std::lock_guard<std::mutex> guard(ch->lock);
    target_receiver = ch->receiver.lock();
    if (!target_receiver) {
      if (auto p = ch->peer.lock()) {
        std::lock_guard<std::mutex> pguard(p->lock);
        target_receiver = p->receiver.lock();
      }
    }
  }
  if (!target_receiver) return false;
  int32_t seq = next_input_seq();
  {
    std::lock_guard<std::mutex> guard(target_receiver->lock);
    QueuedInputEvent qe;
    qe.seq = seq;
    qe.motion = std::make_shared<MotionEvent>(event);
    target_receiver->pending_events.push_back(std::move(qe));
  }
  schedule_dispatch(rt, target_receiver);
  return true;
}

bool post_channel_key_event(Runtime& rt, int64_t channel_handle, int32_t device_id, int32_t key_code,
                            int32_t meta_state, bool down, int64_t time) {
  auto ch = channels().get(channel_handle);
  if (!ch) return false;
  std::shared_ptr<InputReceiver> target_receiver;
  {
    std::lock_guard<std::mutex> guard(ch->lock);
    target_receiver = ch->receiver.lock();
    if (!target_receiver) {
      if (auto p = ch->peer.lock()) {
        std::lock_guard<std::mutex> pguard(p->lock);
        target_receiver = p->receiver.lock();
      }
    }
  }
  if (!target_receiver) return false;
  int32_t seq = next_input_seq();
  {
    std::lock_guard<std::mutex> guard(target_receiver->lock);
    QueuedInputEvent qe;
    qe.seq = seq;
    QueuedInputEvent::Key k;
    k.device_id = device_id;
    k.key_code = key_code;
    k.meta_state = meta_state;
    k.down = down;
    k.time = time;
    qe.key = k;
    target_receiver->pending_events.push_back(std::move(qe));
  }
  schedule_dispatch(rt, target_receiver);
  return true;
}

bool post_window_motion_event(const MotionEvent& event) {
  std::shared_ptr<InputReceiver> recv = get_active_input_receiver();
  rt::Runtime* rt = nullptr;
  {
    std::lock_guard<std::mutex> guard(active_receiver_lock);
    rt = s_active_runtime;
  }
  if (!recv || !rt) return false;
  int32_t seq = next_input_seq();
  {
    std::lock_guard<std::mutex> guard(recv->lock);
    QueuedInputEvent qe;
    qe.seq = seq;
    qe.motion = std::make_shared<MotionEvent>(event);
    recv->pending_events.push_back(std::move(qe));
  }
  schedule_dispatch(*rt, recv);
  return true;
}

bool post_window_key_event(int32_t device_id, int32_t key_code, int32_t meta_state, bool down, int64_t time) {
  std::shared_ptr<InputReceiver> recv = get_active_input_receiver();
  rt::Runtime* rt = nullptr;
  {
    std::lock_guard<std::mutex> guard(active_receiver_lock);
    rt = s_active_runtime;
  }
  if (!recv || !rt) return false;
  int32_t seq = next_input_seq();
  {
    std::lock_guard<std::mutex> guard(recv->lock);
    QueuedInputEvent qe;
    qe.seq = seq;
    QueuedInputEvent::Key k;
    k.device_id = device_id;
    k.key_code = key_code;
    k.meta_state = meta_state;
    k.down = down;
    k.time = time;
    qe.key = k;
    recv->pending_events.push_back(std::move(qe));
  }
  schedule_dispatch(*rt, recv);
  return true;
}

bool post_window_touch(int32_t action, float x, float y, int64_t down_time, int64_t event_time) {
  return post_window_motion_event(make_touch_event(action, x, y, down_time, event_time));
}

bool post_window_key(int32_t device_id, int32_t key_code, int32_t meta_state, bool down, int64_t time) {
  return post_window_key_event(device_id, key_code, meta_state, down, time);
}

}  // namespace aa::bridge

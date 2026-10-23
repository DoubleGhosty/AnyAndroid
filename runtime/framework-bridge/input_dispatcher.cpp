// The input dispatcher (J8 of ADR-006): the host's events reach the windows of
// the application as Android's InputDispatcher
// (frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp),
// InputPublisher/InputConsumer and android_view_InputEventReceiver.cpp
// deliver them, without a socket:
//  - the host's threads queue events (the inbound queue); a dispatcher thread
//    takes them in order, finds their window in the window manager's list
//    (aa.host.services.InputWindows, the WindowService's windows from the
//    top, as InputMonitor publishes gui::WindowInfo) and publishes them on the
//    window's connection, transformed to the window's space;
//  - a connection holds what its socket would: the published messages its
//    consumer has not read, and the wait queue of events not finished yet. A
//    message published before the application created the window's
//    InputEventReceiver waits there, as in the socket;
//  - the receiver's Looper reads the connection (a native callback posted to
//    its MessageQueue stands for the socket's fd callback), makes the Java
//    events (KeyEvent.obtain, android_view_MotionEvent_obtainAsCopy) and hands
//    them to InputEventReceiver.dispatchInputEvent; moves are batched until
//    the next frame consumes them (consumeBatchedInputEvents), as
//    InputConsumer does; finishInputEvent takes them out of the wait queue.
//
// As in InputDispatcher:
//  - a touch gesture goes to the window its ACTION_DOWN found (the topmost
//    visible touchable window whose touchable region holds the point; a
//    touch-modal window's region is the whole display, the window manager
//    sets it so); windows above it that watch outside touches get
//    ACTION_OUTSIDE; a scroll goes to the window under the pointer;
//  - keys and text go to the focused window, the topmost visible focusable
//    one; a window gets a focus event when it gains or loses the focus
//    (ViewRootImpl drops key events without it); a key waits for the events
//    sent before it to be finished, up to KEY_WAITING_FOR_EVENTS_TIMEOUT.
// Not done: touch occlusion and obscured flags, split touches and multiple
// pointers, injection permissions, the policy's key interception (global
// keys, fallback keys of unhandled keys), key repeats synthesized by the
// dispatcher (the host sends its own), resampling of batched moves (a batch
// is consumed whole), ANR timeouts, drag and drop, pointer capture and touch
// mode events.
#include "framework-bridge/input_dispatcher.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "common/clock.h"
#include "common/log.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/input_routing.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/motion_event.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;
using Type = InputEventInfo::Type;

// InputDispatcher's KEY_WAITING_FOR_EVENTS_TIMEOUT.
constexpr int64_t kKeyWaitingForEventsTimeout = 500'000'000;  // ns

constexpr int32_t kVirtualKeyboard = -1;  // KeyCharacterMap.VIRTUAL_KEYBOARD
// Touches and mouse events: the input service declares no such device; the
// shell's `input` command then sends them from device 0.
constexpr int32_t kPointerDevice = 0;
constexpr int32_t kDefaultDisplay = 0;   // Display.DEFAULT_DISPLAY
constexpr int32_t kInvalidDisplay = -1;  // Display.INVALID_DISPLAY: the keys of a keyboard
constexpr int32_t kKeyActionDown = 0, kKeyActionUp = 1, kKeyActionMultiple = 2;
// KeyEvent.FLAG_FROM_SYSTEM: keys of an input device, from the user.
constexpr int32_t kKeyFlagFromSystem = 0x8;

// An event of the inbound queue (InputDispatcher's EventEntry): an injected
// event, text still to be typed, or a focus change.
struct Entry {
  enum class Kind { kMotion, kKey, kText, kFocus } kind = Kind::kKey;
  InputEventInfo info;
  int64_t event_time = 0, down_time = 0;  // monotonic nanoseconds
  uint64_t connection = 0;                // focus: the window's connection
  bool has_focus = false;
};

// A message published on a connection (InputMessage), unread by its consumer.
struct Message {
  enum class Kind { kFocus, kKey, kMotion } kind = Kind::kKey;
  uint32_t seq = 0;
  bool has_focus = false;                 // focus
  int64_t down_time = 0, event_time = 0;  // key
  std::shared_ptr<MotionEvent> motion;    // motion: display coordinates, window transform
  InputEventInfo info;
};

// An event published and not finished yet (an entry of the wait queue).
struct InFlight {
  bool focus = false;  // focus events are not reported
  InputEventInfo info;
};

// InputConsumer's batch of moves of one device and source.
struct Batch {
  std::shared_ptr<MotionEvent> event;  // the first move, with the later ones as samples
  std::vector<uint32_t> seqs;
  InputEventInfo last;
};

// A window's input connection: the server end the window manager registered
// (the dispatcher's Connection) and the client end its InputEventReceiver
// reads (NativeInputEventReceiver and its InputConsumer). The GlobalRef is
// only made and released with the runtime's GIL held (natives, Looper).
struct Connection {
  uint64_t id = 0;
  std::string name;
  bool closed = false;                  // the window manager removed its channel
  int64_t receiver = 0;                 // InputEventReceiver handle, 0 before nativeInit
  std::shared_ptr<rt::GlobalRef> weak;  // WeakReference<InputEventReceiver>
  int64_t queue = 0;                    // MessageQueue.mPtr of its Looper
  std::deque<Message> socket;           // published, not read
  bool callback_posted = false;         // a read is posted to the Looper
  std::map<uint32_t, InFlight> wait_queue;
  std::map<uint32_t, std::vector<uint32_t>> chains;  // batch seq -> the seqs of its earlier samples
  std::optional<Batch> batch;
  bool batched_event_pending = false;  // NativeInputEventReceiver::mBatchedInputEventPending
};

struct Dispatcher {
  std::mutex lock;
  std::condition_variable changed;  // inbound queue, finished events, busy
  std::deque<Entry> inbound;
  std::vector<InputWindow> windows;  // from the top
  std::map<uint64_t, std::shared_ptr<Connection>> connections;
  std::map<int64_t, uint64_t> receivers;  // receiver handle -> connection
  Runtime* runtime = nullptr;             // of the receivers, set by the first one
  uint32_t next_seq = 1;
  uint64_t focused = 0;       // connection of the focused window
  uint64_t touch_target = 0;  // connection of the window of the current gesture
  bool thread_started = false;
  bool busy = false;  // the dispatcher thread works outside the lock
  std::optional<int64_t> key_wait_deadline;
  // Host side: the down time of the current gesture and of the keys held.
  std::optional<int64_t> gesture_down_time;
  std::map<int32_t, int64_t> key_down_times;
  std::shared_ptr<std::function<void(const InputFinished&)>> listener;
};

Dispatcher& dispatcher() {
  static auto* d = new Dispatcher;  // never destroyed: its thread outlives main
  return *d;
}

// A posted read of a connection: the Looper to post to.
struct Post {
  uint64_t connection = 0;
  int64_t receiver = 0, queue = 0;
};

// Sends reports to the listener; called without holding the lock.
void emit(std::vector<InputFinished>& reports) {
  if (reports.empty()) return;
  Dispatcher& d = dispatcher();
  std::shared_ptr<std::function<void(const InputFinished&)>> listener;
  {
    std::lock_guard<std::mutex> guard(d.lock);
    listener = d.listener;
  }
  if (listener) {
    for (const InputFinished& r : reports) (*listener)(r);
  }
  reports.clear();
}

InputFinished dropped(const InputEventInfo& info, uint32_t seq, std::string window, std::string reason) {
  InputFinished r;
  r.seq = seq;
  r.dropped = true;
  r.reason = std::move(reason);
  r.window = std::move(window);
  r.event = info;
  return r;
}

std::shared_ptr<Connection> connection_locked(Dispatcher& d, uint64_t id) {
  auto it = d.connections.find(id);
  return it == d.connections.end() ? nullptr : it->second;
}

std::shared_ptr<Connection> receiver_connection_locked(Dispatcher& d, int64_t receiver) {
  auto it = d.receivers.find(receiver);
  if (it == d.receivers.end()) return nullptr;
  auto c = connection_locked(d, it->second);
  return c && c->receiver == receiver ? c : nullptr;
}

std::shared_ptr<Connection> ensure_connection_locked(Dispatcher& d, uint64_t id) {
  auto& c = d.connections[id];
  if (!c) {
    c = std::make_shared<Connection>();
    c->id = id;
  }
  return c;
}

const InputWindow* window_locked(const Dispatcher& d, uint64_t connection) {
  for (const InputWindow& w : d.windows) {
    if (w.connection == connection) return &w;
  }
  return nullptr;
}

// Drops what a connection holds: its unread messages, its batch and its
// unfinished events (abortBrokenDispatchCycle).
void abort_connection_locked(Connection& c, const std::string& reason, std::vector<InputFinished>& reports) {
  for (auto& [seq, f] : c.wait_queue) {
    if (!f.focus) reports.push_back(dropped(f.info, seq, c.name, reason));
  }
  c.wait_queue.clear();
  c.socket.clear();
  c.chains.clear();
  c.batch.reset();
  c.batched_event_pending = false;
}

// InputPublisher: the message gets the next sequence number and joins the
// socket and the wait queue. A read of the connection is posted when its
// receiver exists and none is pending.
std::optional<Post> publish_locked(Dispatcher& d, Connection& c, Message m) {
  uint32_t seq = d.next_seq++;
  if (seq == 0) seq = d.next_seq++;  // 0 is not a sequence number
  m.seq = seq;
  c.wait_queue[seq] = InFlight{m.kind == Message::Kind::kFocus, m.info};
  c.socket.push_back(std::move(m));
  if (c.receiver == 0 || c.callback_posted) return std::nullopt;
  c.callback_posted = true;
  return Post{c.id, c.receiver, c.queue};
}

bool consume_events(Runtime& rt, int64_t receiver, bool consume_batches);

// Posts reads to the Looper of their receivers (the GIL held). A queue that
// is gone drops the connection's events.
void post_reads(Runtime& rt, const std::vector<Post>& posts) {
  for (const Post& p : posts) {
    try {
      post_queue_callback(rt, p.queue, clock::monotonic_nanos(), [receiver = p.receiver](Runtime& rt2) {
        consume_events(rt2, receiver, false);
      });
    } catch (const rt::JavaThrow&) {
      Dispatcher& d = dispatcher();
      std::vector<InputFinished> reports;
      {
        std::lock_guard<std::mutex> guard(d.lock);
        if (auto c = connection_locked(d, p.connection); c && c->receiver == p.receiver) {
          abort_connection_locked(*c, "boucle de la fenêtre arrêtée", reports);
          c->callback_posted = false;
        }
      }
      d.changed.notify_all();
      emit(reports);
    }
  }
}

// --- The consumer (the receiver's Looper thread) ---------------------------------------------

bool can_add_sample(const Batch& b, const MotionEvent& m) {
  const MotionEvent& head = *b.event;
  return head.device_id() == m.device_id() && head.source() == m.source() &&
         head.action_masked() == m.action_masked() && head.pointer_count() == m.pointer_count() &&
         head.pointer_id(0) == m.pointer_id(0);
}

bool batchable(const MotionEvent& m) {
  int32_t a = m.action_masked();
  return a == kMotionActionMove || a == kMotionActionHoverMove;
}

// The batch as one event: the seq of its last sample, the others chained.
Message take_batch_locked(Connection& c) {
  Batch b = std::move(*c.batch);
  c.batch.reset();
  Message m;
  m.kind = Message::Kind::kMotion;
  m.seq = b.seqs.back();
  m.motion = std::move(b.event);
  m.info = b.last;
  b.seqs.pop_back();
  if (!b.seqs.empty()) c.chains[m.seq] = std::move(b.seqs);
  return m;
}

// InputConsumer::consume: the next event to dispatch, moves gathered in the
// batch. `notify` is set when a new batch waits for the next frame.
std::optional<Message> take_next_locked(Connection& c, bool consume_batches, bool* consumed_batch, bool* notify,
                                        uint32_t* batch_source, std::vector<InputFinished>& reports) {
  while (!c.socket.empty()) {
    Message m = std::move(c.socket.front());
    c.socket.pop_front();
    if (m.kind == Message::Kind::kMotion) {
      if (c.batch) {
        if (batchable(*m.motion) && can_add_sample(*c.batch, *m.motion)) {
          c.batch->event->add_sample(m.motion->event_time(), &m.motion->raw_pointer_coords(0), m.motion->id());
          c.batch->seqs.push_back(m.seq);
          c.batch->last = m.info;
          continue;
        }
        const MotionEvent& head = *c.batch->event;
        if (m.motion->action_masked() == kMotionActionCancel && head.device_id() == m.motion->device_id() &&
            head.source() == m.motion->source()) {
          // No need to process the moves of a cancelled gesture: finished, not handled.
          for (uint32_t seq : c.batch->seqs) {
            auto it = c.wait_queue.find(seq);
            if (it == c.wait_queue.end()) continue;
            InputFinished r;
            r.seq = seq;
            r.window = c.name;
            r.event = it->second.info;
            reports.push_back(std::move(r));
            c.wait_queue.erase(it);
          }
          c.batch.reset();
          return m;
        }
        // It cannot join the batch: the batch goes first, the message waits.
        c.socket.push_front(std::move(m));
        return take_batch_locked(c);
      }
      if (batchable(*m.motion)) {
        Batch b;
        b.event = std::make_shared<MotionEvent>();
        b.event->copy_from(*m.motion, true);
        b.seqs.push_back(m.seq);
        b.last = m.info;
        c.batch = std::move(b);
        continue;
      }
    }
    return m;
  }
  if (c.batch) {
    if (consume_batches) {
      *consumed_batch = true;
      return take_batch_locked(c);
    }
    if (!c.batched_event_pending) {
      c.batched_event_pending = true;
      *notify = true;
      *batch_source = c.batch->event->source();
    }
  }
  return std::nullopt;
}

uint32_t lo(int64_t v) { return uint32_t(uint64_t(v)); }
uint32_t hi(int64_t v) { return uint32_t(uint64_t(v) >> 32); }

// android_view_KeyEvent_obtainAsCopy: KeyEvent.obtain(id, downTime, eventTime,
// action, code, repeat, metaState, deviceId, scanCode, flags, source,
// displayId, hmac, characters), times in nanoseconds.
Handle obtain_key_event(Runtime& rt, const Message& m) {
  const InputEventInfo& e = m.info;
  rt::GlobalRef hmac(rt, rt.new_array("[B", 32));  // INVALID_HMAC
  rt::GlobalRef characters(rt, e.type == Type::kCharacters ? rt.new_string(e.characters) : kNull);
  return Handle(rt.call_static("Landroid/view/KeyEvent;", "obtain",
                               "(IJJIIIIIIIII[BLjava/lang/String;)Landroid/view/KeyEvent;",
                               {uint32_t(next_input_event_id()), lo(m.down_time), hi(m.down_time), lo(m.event_time),
                                hi(m.event_time), uint32_t(e.action), uint32_t(e.key_code), uint32_t(e.repeat),
                                uint32_t(e.meta_state), uint32_t(kVirtualKeyboard), 0, uint32_t(kKeyFlagFromSystem),
                                kSourceKeyboard, uint32_t(kInvalidDisplay), hmac.get(), characters.get()}));
}

// An event published to a receiver that went away: out of the wait queue.
void abandon(int64_t receiver, uint32_t seq, const std::string& reason) {
  Dispatcher& d = dispatcher();
  std::vector<InputFinished> reports;
  {
    std::lock_guard<std::mutex> guard(d.lock);
    auto c = receiver_connection_locked(d, receiver);
    if (!c) return;
    std::vector<uint32_t> seqs{seq};
    if (auto chain = c->chains.find(seq); chain != c->chains.end()) {
      seqs.insert(seqs.begin(), chain->second.begin(), chain->second.end());
      c->chains.erase(chain);
    }
    for (uint32_t s : seqs) {
      auto it = c->wait_queue.find(s);
      if (it == c->wait_queue.end()) continue;
      if (!it->second.focus) reports.push_back(dropped(it->second.info, s, c->name, reason));
      c->wait_queue.erase(it);
    }
  }
  d.changed.notify_all();
  emit(reports);
}

// One event to Java: a focus event to InputEventReceiver.dispatchFocusEvent
// (which finishes it), the others made into Java events for
// dispatchInputEvent. In Android 17 both are static and take the
// WeakReference: false when the receiver is collected or disposed. An
// exception of the application goes up to its Looper, as
// raiseAndClearException does.
void deliver(Runtime& rt, int64_t receiver, const rt::GlobalRef& weak, const Message& m) {
  const char* ier = "Landroid/view/InputEventReceiver;";
  if (m.kind == Message::Kind::kFocus) {
    bool alive = (rt.call_static(ier, "dispatchFocusEvent", "(Ljava/lang/ref/WeakReference;IZ)Z",
                                 {weak.get(), m.seq, uint32_t(m.has_focus)}) & 1) != 0;
    if (!alive) abandon(receiver, m.seq, "récepteur disparu");
    return;
  }
  Handle event = kNull;
  try {
    event = m.kind == Message::Kind::kKey ? obtain_key_event(rt, m) : obtain_motion_event(rt, *m.motion);
  } catch (const rt::JavaThrow& t) {
    std::string why = rt.describe_throwable(t.exception);
    log::warn("input", "événement d'entrée non construit (" + describe_input_event(m.info) + ") : " +
                           why.substr(0, why.find('\n')));
    abandon(receiver, m.seq, "événement Java impossible à construire");
    return;
  }
  rt::GlobalRef keep(rt, event);
  bool alive = (rt.call_static(ier, "dispatchInputEvent", "(Ljava/lang/ref/WeakReference;ILandroid/view/InputEvent;)Z",
                               {weak.get(), m.seq, event}) & 1) != 0;
  if (!alive) abandon(receiver, m.seq, "récepteur disparu");
}

// NativeInputEventReceiver::consumeEvents: reads the connection and
// dispatches its events until it is empty; true when a batch was consumed.
bool consume_events(Runtime& rt, int64_t receiver, bool consume_batches) {
  Dispatcher& d = dispatcher();
  bool consumed_batch = false;
  std::shared_ptr<rt::GlobalRef> weak;
  {
    std::lock_guard<std::mutex> guard(d.lock);
    auto c = receiver_connection_locked(d, receiver);
    if (!c) return false;
    if (consume_batches) {
      c->batched_event_pending = false;
    } else {
      c->callback_posted = false;
    }
    weak = c->weak;
  }
  while (true) {
    std::optional<Message> next;
    bool notify = false;
    uint32_t source = 0;
    std::vector<InputFinished> reports;
    {
      std::lock_guard<std::mutex> guard(d.lock);
      auto c = receiver_connection_locked(d, receiver);
      if (!c) return consumed_batch;  // disposed by an event's handler
      next = take_next_locked(*c, consume_batches, &consumed_batch, &notify, &source, reports);
    }
    if (!reports.empty()) {
      d.changed.notify_all();
      emit(reports);
    }
    if (notify) {
      // A batch waits: the receiver asks its next frame to consume it.
      bool alive = (rt.call_static("Landroid/view/InputEventReceiver;", "dispatchBatchedInputEventPending",
                                   "(Ljava/lang/ref/WeakReference;I)Z", {weak->get(), source}) & 1) != 0;
      if (!alive) {
        std::lock_guard<std::mutex> guard(d.lock);
        if (auto c = receiver_connection_locked(d, receiver)) c->batched_event_pending = false;
      }
      return consumed_batch;
    }
    if (!next) return consumed_batch;
    deliver(rt, receiver, *weak, *next);
  }
}

// --- The dispatcher thread ---------------------------------------------------------------------

// A Java-free motion event for a window (InputDispatcher's dispatch entry):
// one pointer at display coordinates, the window's frame offset as its
// transform, the display's (identity) as the raw one. A touch's ACTION_UP
// has no pressure, as the shell's `input tap` sends it.
std::shared_ptr<MotionEvent> make_motion(const Entry& e, int32_t action, const InputRect& frame) {
  const bool scroll = e.info.type == Type::kScroll;
  PointerProperties props;
  props.id = 0;
  props.tool_type = scroll ? kToolTypeMouse : kToolTypeFinger;
  PointerCoords coords;
  coords.set_axis_value(kAxisX, e.info.x);
  coords.set_axis_value(kAxisY, e.info.y);
  if (scroll) {
    coords.set_axis_value(kAxisVScroll, e.info.vertical);
    coords.set_axis_value(kAxisHScroll, e.info.horizontal);
  } else {
    coords.set_axis_value(kAxisPressure, action == kMotionActionUp ? 0.0f : 1.0f);
    coords.set_axis_value(kAxisSize, 1.0f);
  }
  UiTransform transform;
  transform.set(-float(frame.left), -float(frame.top));
  UiTransform raw;  // the display: no rotation, no offset
  float cursor_x = scroll ? e.info.x : kInvalidCursorPosition;
  float cursor_y = scroll ? e.info.y : kInvalidCursorPosition;
  auto m = std::make_shared<MotionEvent>();
  m->initialize(next_input_event_id(), kPointerDevice, scroll ? kSourceMouse : kSourceTouchscreen, kDefaultDisplay,
                MotionEvent::Hmac{}, action, 0, 0, 0, 0, 0, 0, transform, 1.0f, 1.0f, cursor_x, cursor_y, raw,
                e.down_time, e.event_time, 1, &props, &coords);
  return m;
}

// Publishes an event on a window's connection, the motion transformed to the
// window; false (with a report) when the window or its connection is gone.
bool publish_event_locked(Dispatcher& d, const Entry& e, uint64_t connection, int32_t action,
                          std::vector<Post>& posts, std::vector<InputFinished>& reports) {
  InputEventInfo info = e.info;
  info.action = action;
  const InputWindow* w = window_locked(d, connection);
  auto c = connection_locked(d, connection);
  if (!w || !c || c->closed) {
    reports.push_back(dropped(info, 0, c ? c->name : std::string(), "fenêtre retirée"));
    return false;
  }
  Message m;
  m.info = info;
  if (e.kind == Entry::Kind::kMotion) {
    m.kind = Message::Kind::kMotion;
    m.motion = make_motion(e, action, w->frame);
  } else {
    m.kind = Message::Kind::kKey;
    m.down_time = e.down_time;
    m.event_time = e.event_time;
  }
  if (auto p = publish_locked(d, *c, std::move(m))) posts.push_back(*p);
  return true;
}

// InputDispatcher::dispatchMotionLocked: the windows of a touch or a scroll.
void dispatch_motion_locked(Dispatcher& d, const Entry& e, std::vector<Post>& posts,
                            std::vector<InputFinished>& reports) {
  const int32_t action = e.info.action;
  if (e.info.type == Type::kScroll) {
    int touched = find_touched_window(d.windows, e.info.x, e.info.y);
    if (touched < 0) {
      reports.push_back(dropped(e.info, 0, {}, "aucune fenêtre sous le pointeur"));
      return;
    }
    publish_event_locked(d, e, d.windows[size_t(touched)].connection, action, posts, reports);
    return;
  }
  if (action == kMotionActionDown) {
    int touched = find_touched_window(d.windows, e.info.x, e.info.y);
    for (size_t i : find_outside_targets(d.windows, touched)) {
      publish_event_locked(d, e, d.windows[i].connection, kMotionActionOutside, posts, reports);
    }
    if (touched < 0) {
      d.touch_target = 0;
      reports.push_back(dropped(e.info, 0, {}, "aucune fenêtre touchée"));
      return;
    }
    d.touch_target = d.windows[size_t(touched)].connection;
  } else if (d.touch_target == 0) {
    reports.push_back(dropped(e.info, 0, {}, "pas de geste en cours"));
    return;
  }
  uint64_t target = d.touch_target;
  if (action == kMotionActionUp || action == kMotionActionCancel) d.touch_target = 0;
  publish_event_locked(d, e, target, action, posts, reports);
}

// The key events of typed text, at the text's time.
std::vector<Entry> typed_entries(const KeyCharacterMap* map, const Entry& text) {
  std::vector<Entry> out;
  for (TextStroke& s : text_to_strokes(map, text.info.characters)) {
    Entry k;
    k.kind = Entry::Kind::kKey;
    k.event_time = k.down_time = text.event_time;
    if (s.multiple) {
      k.info.type = Type::kCharacters;
      k.info.action = kKeyActionMultiple;  // KEYCODE_UNKNOWN, no repeat
      k.info.characters = std::move(s.characters);
    } else {
      k.info.type = Type::kKey;
      k.info.action = s.down ? kKeyActionDown : kKeyActionUp;
      k.info.key_code = s.key_code;
      k.info.meta_state = s.meta_state;
    }
    out.push_back(std::move(k));
  }
  return out;
}

bool is_key(const Entry& e) { return e.kind == Entry::Kind::kKey || e.kind == Entry::Kind::kText; }

bool any_unfinished_locked(const Dispatcher& d) {
  for (const auto& [id, c] : d.connections) {
    if (!c->wait_queue.empty()) return true;
  }
  return false;
}

// The dispatcher thread (InputDispatcher::dispatchOnce): attached to the
// runtime only while it posts to a Looper or types text.
void run_dispatcher() {
  Dispatcher& d = dispatcher();
  std::unique_lock<std::mutex> guard(d.lock);
  while (true) {
    d.changed.wait(guard, [&] { return !d.inbound.empty(); });
    const Entry& head = d.inbound.front();
    // shouldWaitToSendKeyLocked: a key waits for the events sent before it,
    // which may move the focus.
    if (is_key(head) && any_unfinished_locked(d)) {
      int64_t now = clock::monotonic_nanos();
      if (!d.key_wait_deadline) d.key_wait_deadline = now + kKeyWaitingForEventsTimeout;
      if (now < *d.key_wait_deadline) {
        d.changed.wait_for(guard, std::chrono::nanoseconds(*d.key_wait_deadline - now));
        continue;
      }
      log::warn("input", "touche envoyée avant la fin des événements précédents");
    }
    if (is_key(head)) d.key_wait_deadline.reset();
    Entry e = std::move(d.inbound.front());
    d.inbound.pop_front();
    std::vector<Post> posts;
    std::vector<InputFinished> reports;
    std::optional<Entry> text;
    switch (e.kind) {
      case Entry::Kind::kFocus:
        if (auto c = connection_locked(d, e.connection); c && !c->closed) {
          Message m;
          m.kind = Message::Kind::kFocus;
          m.has_focus = e.has_focus;
          if (auto p = publish_locked(d, *c, std::move(m))) posts.push_back(*p);
        }
        break;
      case Entry::Kind::kKey:
        if (d.focused == 0) {
          reports.push_back(dropped(e.info, 0, {}, "aucune fenêtre n'a le focus"));
        } else {
          publish_event_locked(d, e, d.focused, e.info.action, posts, reports);
        }
        break;
      case Entry::Kind::kMotion:
        dispatch_motion_locked(d, e, posts, reports);
        break;
      case Entry::Kind::kText:
        if (d.focused == 0 || !d.runtime) {
          reports.push_back(dropped(e.info, 0, {}, "aucune fenêtre n'a le focus"));
        } else {
          text = std::move(e);
        }
        break;
    }
    if (!posts.empty() || text) {
      Runtime* rt = d.runtime;
      d.busy = true;
      guard.unlock();
      try {
        Runtime::Scope scope(*rt, "InputDispatcher");
        post_reads(*rt, posts);
        if (text) {
          // Typed text becomes key events at the head of the queue.
          std::vector<Entry> typed = typed_entries(virtual_key_character_map(*rt).get(), *text);
          std::lock_guard<std::mutex> relock(d.lock);
          d.inbound.insert(d.inbound.begin(), typed.begin(), typed.end());
        }
      } catch (const rt::JavaThrow&) {
        log::warn("input", "exception Java pendant l'envoi d'un événement d'entrée");
      }
      guard.lock();
      d.busy = false;
    }
    guard.unlock();
    emit(reports);
    d.changed.notify_all();
    guard.lock();
  }
}

// Queues an event (under the lock) and starts the dispatcher thread.
void enqueue_locked(Dispatcher& d, Entry e) {
  d.inbound.push_back(std::move(e));
  if (!d.thread_started) {
    d.thread_started = true;
    std::thread(run_dispatcher).detach();
  }
}

// enqueueFocusEventLocked: focus events go to the head of the queue, after
// the focus events already there.
void enqueue_focus_locked(Dispatcher& d, uint64_t connection, bool has_focus) {
  Entry f;
  f.kind = Entry::Kind::kFocus;
  f.connection = connection;
  f.has_focus = has_focus;
  f.event_time = clock::monotonic_nanos();
  auto it = d.inbound.begin();
  while (it != d.inbound.end() && it->kind == Entry::Kind::kFocus) ++it;
  d.inbound.insert(it, std::move(f));
  if (!d.thread_started) {
    d.thread_started = true;
    std::thread(run_dispatcher).detach();
  }
}

// The focus follows the window list (setFocusedWindow, onFocusChangedLocked).
void update_focus_locked(Dispatcher& d) {
  int index = find_focused_window(d.windows);
  uint64_t focused = index < 0 ? 0 : d.windows[size_t(index)].connection;
  if (focused == d.focused) return;
  if (d.focused != 0) enqueue_focus_locked(d, d.focused, false);
  if (focused != 0) enqueue_focus_locked(d, focused, true);
  d.focused = focused;
}

void notify_input(Dispatcher& d) { d.changed.notify_all(); }

}  // namespace

// --- Natives -------------------------------------------------------------------------------------

void input_receiver_attached(Runtime& rt, int64_t receiver, uint64_t connection, Handle weak_receiver,
                             int64_t queue_ptr) {
  Dispatcher& d = dispatcher();
  auto weak = std::make_shared<rt::GlobalRef>(rt, weak_receiver);
  std::shared_ptr<rt::GlobalRef> previous;  // released after the lock, with the GIL
  std::vector<Post> posts;
  {
    std::lock_guard<std::mutex> guard(d.lock);
    if (!d.runtime) d.runtime = &rt;
    auto c = ensure_connection_locked(d, connection);
    if (c->receiver != 0) d.receivers.erase(c->receiver);
    previous = std::move(c->weak);
    c->receiver = receiver;
    c->weak = std::move(weak);
    c->queue = queue_ptr;
    c->callback_posted = false;
    d.receivers[receiver] = connection;
    if (!c->socket.empty()) {
      c->callback_posted = true;
      posts.push_back(Post{connection, receiver, queue_ptr});
    }
  }
  post_reads(rt, posts);
}

void input_receiver_disposed(Runtime&, int64_t receiver) {
  Dispatcher& d = dispatcher();
  std::vector<InputFinished> reports;
  std::shared_ptr<rt::GlobalRef> weak;  // released after the lock, with the GIL
  {
    std::lock_guard<std::mutex> guard(d.lock);
    auto c = receiver_connection_locked(d, receiver);
    d.receivers.erase(receiver);
    if (!c) return;
    abort_connection_locked(*c, "récepteur fermé", reports);
    c->receiver = 0;
    c->queue = 0;
    c->callback_posted = false;
    weak = std::move(c->weak);
    if (c->closed) d.connections.erase(c->id);
  }
  notify_input(d);
  emit(reports);
}

void input_receiver_finished(int64_t receiver, uint32_t seq, bool handled) {
  Dispatcher& d = dispatcher();
  std::vector<InputFinished> reports;
  {
    std::lock_guard<std::mutex> guard(d.lock);
    auto c = receiver_connection_locked(d, receiver);
    if (!c) return;
    std::vector<uint32_t> seqs{seq};
    if (auto chain = c->chains.find(seq); chain != c->chains.end()) {
      seqs.insert(seqs.begin(), chain->second.begin(), chain->second.end());
      c->chains.erase(chain);
    }
    for (uint32_t s : seqs) {
      auto it = c->wait_queue.find(s);
      if (it == c->wait_queue.end()) continue;  // dropped meanwhile
      if (!it->second.focus) {
        InputFinished r;
        r.seq = s;
        r.handled = handled;
        r.window = c->name;
        r.event = it->second.info;
        reports.push_back(std::move(r));
      }
      c->wait_queue.erase(it);
    }
  }
  notify_input(d);
  emit(reports);
}

bool input_receiver_consume_batch(Runtime& rt, int64_t receiver, int64_t) {
  return consume_events(rt, receiver, true);
}

bool input_receiver_probably_has_input(int64_t receiver) {
  Dispatcher& d = dispatcher();
  std::lock_guard<std::mutex> guard(d.lock);
  auto c = receiver_connection_locked(d, receiver);
  return c && (!c->socket.empty() || c->batch);
}

void register_input_windows(rt::NativeRegistry& r) {
  const char* iw = "Laa/host/services/InputWindows;";
  // InputMonitor.updateInputWindowsLw: the windows from the top, as parallel
  // arrays (their server channel, title, then 12 values each: type, flags,
  // frame, touchable region bounds, visible, focusable).
  r.bind(iw, "nativeSetInputWindows", "([Landroid/view/InputChannel;[Ljava/lang/String;[I)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull || a.h(1) == kNull || a.h(2) == kNull) rt.throw_npe("fenêtres");
           constexpr int32_t kStride = 12;
           rt::ArrayObject& channels = rt.array(a.h(0), "InputChannel[]");
           rt::ArrayObject& names = rt.array(a.h(1), "String[]");
           rt::ArrayObject& values = rt.array(a.h(2), "int[]");
           if (names.length != channels.length || values.length != channels.length * kStride) {
             rt.throw_new("Ljava/lang/IllegalArgumentException;", "[AnyAndroid] fenêtres d'entrée incohérentes");
           }
           std::vector<InputWindow> windows;
           for (uint32_t i = 0; i < channels.length; i++) {
             Handle channel, name;
             std::memcpy(&channel, channels.data.data() + size_t(i) * 4, 4);
             std::memcpy(&name, names.data.data() + size_t(i) * 4, 4);
             int32_t v[kStride];
             std::memcpy(v, values.data.data() + size_t(i) * kStride * 4, sizeof v);
             InputWindow w;
             w.connection = input_channel_connection(rt, channel);
             if (w.connection == 0) continue;
             if (name != kNull) w.name = rt.string_utf8(name);
             w.type = v[0];
             w.layout_flags = v[1];
             w.frame = {v[2], v[3], v[4], v[5]};
             w.touchable = {v[6], v[7], v[8], v[9]};
             w.visible = v[10] != 0;
             w.focusable = v[11] != 0;
             windows.push_back(std::move(w));
           }
           Dispatcher& d = dispatcher();
           {
             std::lock_guard<std::mutex> guard(d.lock);
             for (const InputWindow& w : windows) ensure_connection_locked(d, w.connection)->name = w.name;
             d.windows = std::move(windows);
             update_focus_locked(d);
           }
           notify_input(d);
           return 0;
         }, S::kPartial, "fenêtres d'entrée sans occultation ni découpage des gestes");
  // InputManagerService.removeInputChannel: the window is gone; what its
  // connection holds is dropped.
  r.bind(iw, "nativeRemoveInputChannel", "(Landroid/view/InputChannel;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint64_t connection = input_channel_connection(rt, a.h(0));
    if (connection == 0) return 0;
    Dispatcher& d = dispatcher();
    std::vector<InputFinished> reports;
    std::shared_ptr<Connection> erased;  // its GlobalRef released after the lock, with the GIL
    {
      std::lock_guard<std::mutex> guard(d.lock);
      auto c = connection_locked(d, connection);
      if (!c) return 0;
      abort_connection_locked(*c, "fenêtre retirée", reports);
      c->closed = true;
      if (d.touch_target == connection) d.touch_target = 0;
      if (c->receiver == 0) {
        erased = c;
        d.connections.erase(connection);
      }
    }
    notify_input(d);
    emit(reports);
    return 0;
  });
}

// --- Host side --------------------------------------------------------------------------------

void inject_touch(TouchAction action, float x, float y) {
  Entry e;
  e.kind = Entry::Kind::kMotion;
  e.info.type = Type::kTouch;
  e.info.action = int32_t(action);
  e.info.x = x;
  e.info.y = y;
  e.event_time = clock::monotonic_nanos();
  Dispatcher& d = dispatcher();
  {
    std::lock_guard<std::mutex> guard(d.lock);
    if (action == TouchAction::kDown) d.gesture_down_time = e.event_time;
    e.down_time = d.gesture_down_time.value_or(e.event_time);
    if (action == TouchAction::kUp || action == TouchAction::kCancel) d.gesture_down_time.reset();
    enqueue_locked(d, std::move(e));
  }
  notify_input(d);
}

void inject_key(int32_t key_code, bool down, int32_t meta_state, int32_t repeat) {
  Entry e;
  e.kind = Entry::Kind::kKey;
  e.info.type = Type::kKey;
  e.info.action = down ? kKeyActionDown : kKeyActionUp;
  e.info.key_code = key_code;
  e.info.meta_state = meta_state;
  e.info.repeat = down && repeat > 0 ? repeat : 0;
  e.event_time = clock::monotonic_nanos();
  Dispatcher& d = dispatcher();
  {
    std::lock_guard<std::mutex> guard(d.lock);
    auto held = d.key_down_times.find(key_code);
    if (down) {
      // An auto-repeated down keeps the time of the first one.
      if (e.info.repeat == 0 || held == d.key_down_times.end()) {
        held = d.key_down_times.insert_or_assign(key_code, e.event_time).first;
      }
      e.down_time = held->second;
    } else {
      e.down_time = held != d.key_down_times.end() ? held->second : e.event_time;
      if (held != d.key_down_times.end()) d.key_down_times.erase(held);
    }
    enqueue_locked(d, std::move(e));
  }
  notify_input(d);
}

void inject_text(std::u16string text) {
  if (text.empty()) return;
  Entry e;
  e.kind = Entry::Kind::kText;
  e.info.type = Type::kCharacters;
  e.info.action = kKeyActionMultiple;
  e.info.characters = std::move(text);
  e.event_time = e.down_time = clock::monotonic_nanos();
  Dispatcher& d = dispatcher();
  {
    std::lock_guard<std::mutex> guard(d.lock);
    enqueue_locked(d, std::move(e));
  }
  notify_input(d);
}

void inject_scroll(float x, float y, float horizontal, float vertical) {
  Entry e;
  e.kind = Entry::Kind::kMotion;
  e.info.type = Type::kScroll;
  e.info.action = kMotionActionScroll;
  e.info.x = x;
  e.info.y = y;
  e.info.horizontal = horizontal;
  e.info.vertical = vertical;
  e.event_time = e.down_time = clock::monotonic_nanos();
  Dispatcher& d = dispatcher();
  {
    std::lock_guard<std::mutex> guard(d.lock);
    enqueue_locked(d, std::move(e));
  }
  notify_input(d);
}

void set_input_finished_listener(std::function<void(const InputFinished&)> listener) {
  Dispatcher& d = dispatcher();
  std::lock_guard<std::mutex> guard(d.lock);
  d.listener = listener ? std::make_shared<std::function<void(const InputFinished&)>>(std::move(listener)) : nullptr;
}

bool wait_for_input_idle(int64_t timeout_ms) {
  Dispatcher& d = dispatcher();
  std::unique_lock<std::mutex> guard(d.lock);
  return d.changed.wait_for(guard, std::chrono::milliseconds(timeout_ms),
                            [&] { return d.inbound.empty() && !d.busy && !any_unfinished_locked(d); });
}

}  // namespace aa::bridge

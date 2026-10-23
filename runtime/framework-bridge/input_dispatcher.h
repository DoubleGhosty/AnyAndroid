#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "dex-engine/runtime.h"
#include "framework-bridge/key_character_map.h"

// What the input dispatcher (input_dispatcher.cpp) shares with the other
// natives of the bridge: the InputChannel and InputEventReceiver natives
// (framework_surface.cpp) and the key character maps (framework_input.cpp).
// The host's side is in bridge.h.
namespace aa::bridge {

// --- InputChannel (framework_surface.cpp) ---------------------------------------

// The connection of a Java InputChannel (the pair openInputChannelPair made,
// shared by both ends and their dups); 0 for null or a disposed channel.
uint64_t input_channel_connection(rt::Runtime& rt, rt::Handle input_channel);

// --- InputEventReceiver (framework_surface.cpp) ---------------------------------

// nativeInit, on the receiver's Looper thread: the client end of
// `connection` is read by the receiver `receiver` (its native handle);
// `weak_receiver` is the WeakReference<InputEventReceiver>, `queue_ptr` the
// MessageQueue.mPtr of its Looper. Events already published on the
// connection are delivered from now on. The first receiver gives the
// dispatcher its runtime.
void input_receiver_attached(rt::Runtime& rt, int64_t receiver, uint64_t connection, rt::Handle weak_receiver,
                             int64_t queue_ptr);
// nativeDispose: the events of the connection not finished yet are dropped.
void input_receiver_disposed(rt::Runtime& rt, int64_t receiver);
// nativeFinishInputEvent (InputConsumer::sendFinishedSignal).
void input_receiver_finished(int64_t receiver, uint32_t seq, bool handled);
// nativeConsumeBatchedInputEvents, on the Looper thread: delivers the
// pending batch of moves; true when one was delivered.
bool input_receiver_consume_batch(rt::Runtime& rt, int64_t receiver, int64_t frame_time);
// nativeProbablyHasInput: events published and not consumed yet, or a batch.
bool input_receiver_probably_has_input(int64_t receiver);

// --- Key character maps (framework_input.cpp) -------------------------------------

// The virtual keyboard's map (device -1, /system/usr/keychars/Virtual.kcm),
// the one the input service loads, cached; loaded here when the service has
// not done it yet (InputReader loads it at boot). Null when unreadable. Call
// inside a Runtime::Scope (the KEYCODE_ labels come from KeyEvent).
std::shared_ptr<const KeyCharacterMap> virtual_key_character_map(rt::Runtime& rt);

// --- Natives of the host's window list ---------------------------------------------

// aa.host.services.InputWindows: the window manager's input windows
// (registered by register_framework_surface).
void register_input_windows(rt::NativeRegistry& r);

}  // namespace aa::bridge

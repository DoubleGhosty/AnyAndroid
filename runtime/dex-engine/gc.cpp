// Stop-the-world mark-sweep collector.
//
// Runs on the thread that holds the GIL, so every other Java thread is parked
// (or outside the VM). Roots:
//  - precise: class mirrors and reference-typed statics, resolved strings,
//    interned strings, native caches, global references, host root providers,
//    thread objects, JNI local references;
//  - conservative: interpreter register files and native frame arguments
//    (any 32-bit value that is a live handle), and the native stacks of the
//    threads inside the VM (32-bit handles and 64-bit Object pointers).
// Heap objects are traced precisely (reference fields, reference arrays,
// NativePeer::trace). Objects never move, so conservative roots are safe:
// they can only retain garbage, never free a live object.

#include <chrono>
#include <cstring>

#include "common/log.h"
#include "dex-engine/runtime.h"

namespace aa::rt {

void Runtime::collect_garbage() {
  if (in_gc_ || !running_) return;
  in_gc_ = true;
  gc_requested_ = false;
  auto start = std::chrono::steady_clock::now();

  std::vector<uint8_t> marks(slots_.size(), 0);
  std::vector<uint32_t> work;
  auto mark = [&](Handle h) {
    if (!is_valid_handle(h)) return;
    uint32_t idx = h & 0xFFFFFF;
    if (!marks[idx]) {
      marks[idx] = 1;
      work.push_back(idx);
    }
  };

  // Object pointers found on native stacks are mapped back to their slot.
  std::unordered_map<const Object*, uint32_t> by_pointer;
  by_pointer.reserve(live_objects_);
  for (uint32_t i = 1; i < slots_.size(); ++i) {
    if (slots_[i].obj) by_pointer.emplace(slots_[i].obj.get(), i);
  }
  auto scan_range = [&](const char* lo, const char* hi) {
    if (!lo || !hi || hi <= lo) return;
    const char* p = reinterpret_cast<const char*>((reinterpret_cast<uintptr_t>(lo) + 3) & ~uintptr_t(3));
    for (; p + 4 <= hi; p += 4) {
      uint32_t v;
      std::memcpy(&v, p, 4);
      mark(v);
      if ((reinterpret_cast<uintptr_t>(p) & 7) == 0 && p + 8 <= hi) {
        const Object* ptr;
        std::memcpy(&ptr, p, 8);
        auto it = by_pointer.find(ptr);
        if (it != by_pointer.end() && !marks[it->second]) {
          marks[it->second] = 1;
          work.push_back(it->second);
        }
      }
    }
  };

  // --- Roots ---------------------------------------------------------------------
  for (const auto& [name, cls] : classes_) {
    mark(cls->mirror);
    for (const auto& f : cls->fields) {
      if (f->is_static() && f->is_reference() && f->slot < cls->statics.size()) mark(cls->statics[f->slot]);
    }
  }
  for (const auto& cache : dex_caches_) {
    for (Handle h : cache->strings) mark(h);
  }
  for (const auto& [s, h] : interned_) mark(h);
  for (const auto& [k, h] : object_cache_) mark(h);
  for (const auto& [h, n] : globals_) mark(h);
  std::vector<Handle> extra;
  for (const auto& [id, provider] : root_providers_) provider(extra);
  for (Handle h : extra) mark(h);

  std::jmp_buf here;
  setjmp(here);
  volatile char marker = 0;
  for (const auto& t : threads_) {
    mark(t->peer);
    for (Handle h : t->jni_locals) mark(h);
    for (const Frame& f : t->frames) {
      for (uint32_t i = 0; i < f.nregs; ++i) mark(f.regs[i]);
    }
    if (!t->in_vm) continue;
    if (t.get() == running_) {
      scan_range(reinterpret_cast<const char*>(&here), reinterpret_cast<const char*>(&here) + sizeof here);
      scan_range(const_cast<const char*>(&marker), t->stack_high);
    } else if (t->parked_sp) {
      scan_range(reinterpret_cast<const char*>(&t->parked_regs),
                 reinterpret_cast<const char*>(&t->parked_regs) + sizeof t->parked_regs);
      scan_range(t->parked_sp, t->stack_high);
    }
  }

  // --- Mark ------------------------------------------------------------------------
  std::vector<Handle> traced;
  while (!work.empty()) {
    uint32_t idx = work.back();
    work.pop_back();
    Object* o = slots_[idx].obj.get();
    if (o->klass) {
      for (uint32_t s : o->klass->ref_slots) {
        if (s < o->fields.size()) mark(o->fields[s]);
      }
      if (auto* a = dynamic_cast<ArrayObject*>(o); a && a->klass->component && !a->klass->component->primitive) {
        for (uint32_t i = 0; i < a->length; ++i) {
          Handle h;
          std::memcpy(&h, &a->data[size_t(i) * 4], 4);
          mark(h);
        }
      }
    }
    if (o->peer) {
      traced.clear();
      o->peer->trace(traced);
      for (Handle h : traced) mark(h);
    }
  }

  // --- Weak references, then sweep ----------------------------------------------------
  auto is_live = [&](Handle h) { return is_valid_handle(h) && marks[h & 0xFFFFFF] != 0; };
  std::vector<uint32_t> dying;
  for (uint32_t i = 1; i < slots_.size(); ++i) {
    if (!slots_[i].obj) continue;
    if (marks[i]) {
      if (slots_[i].obj->peer) slots_[i].obj->peer->sweep_weak(is_live);
    } else {
      dying.push_back(i);
    }
  }
  for (uint32_t i : dying) {
    if (slots_[i].obj->peer) slots_[i].obj->peer->on_collect(is_live);
  }
  for (uint32_t i : dying) {
    Slot& s = slots_[i];
    heap_bytes_ -= std::min<size_t>(heap_bytes_, s.bytes);
    s.obj.reset();
    s.bytes = 0;
    s.gen = uint8_t(s.gen + 1);
    if (s.gen == 0) s.gen = 1;
    free_slots_.push_back(i);
  }
  live_objects_ -= dying.size();
  gc_object_threshold_ = std::max(options_.gc_min_objects, live_objects_ * 2);
  gc_byte_threshold_ = std::max(options_.gc_min_bytes, heap_bytes_ * 2);

  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  gc_stats_.collections++;
  gc_stats_.freed += dying.size();
  gc_stats_.last_live = live_objects_;
  gc_stats_.last_ms = ms;
  if (options_.verbose_gc || log::enabled(log::Level::Debug)) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "collecte #%llu : %zu objets libérés, %zu vivants, %.1f Mio, %.2f ms",
                  static_cast<unsigned long long>(gc_stats_.collections), dying.size(), live_objects_,
                  double(heap_bytes_) / (1 << 20), ms);
    log::write(options_.verbose_gc ? log::Level::Info : log::Level::Debug, "gc", buf);
  }
  in_gc_ = false;
}

}  // namespace aa::rt

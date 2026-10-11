#include "framework-bridge/bridge.h"

#include "framework-bridge/internal.h"

namespace aa::bridge {

void install(rt::NativeRegistry& registry, Host& host) {
  register_java_lang(registry, host);
  register_java_util(registry, host);
  register_java_reflect(registry, host);
  register_android_app(registry, host);
  register_android_widget(registry, host);
  register_android_res(registry, host);  // after the classes it extends
  register_framework_graphics(registry);
  register_libcore_natives(registry);
}

void ViewPeer::trace(std::vector<rt::Handle>& out) const {
  out.push_back(click_listener);
  out.push_back(context);
  out.push_back(tag);
  out.push_back(layout_params);
  for (const ui::Node* child : node->children) out.push_back(child->java_handle);
}

void ViewPeer::on_collect(const std::function<bool(rt::Handle)>& is_live) {
  // A collected parent must not leave dangling parent pointers in children
  // that are still referenced elsewhere.
  for (ui::Node* child : node->children) {
    if (is_live(child->java_handle)) child->parent = nullptr;
  }
}

ActivityPeer* activity_peer(rt::Runtime& rt, rt::Handle activity) { return rt.peer<ActivityPeer>(activity); }
ViewPeer* view_peer(rt::Runtime& rt, rt::Handle view) { return rt.peer<ViewPeer>(view); }
IntentPeer* intent_peer(rt::Runtime& rt, rt::Handle intent) { return rt.peer<IntentPeer>(intent); }

ui::Node* find_view_by_id(ui::Node* root, int32_t id) {
  if (!root || id == -1) return nullptr;
  if (root->id == id) return root;
  for (ui::Node* c : root->children) {
    if (ui::Node* n = find_view_by_id(c, id)) return n;
  }
  return nullptr;
}

}  // namespace aa::bridge

// java.util subset: List/ArrayList, Map/HashMap and their iterators.

#include <cstring>

#include "common/text.h"
#include "dex-engine/jlang.h"
#include "framework-bridge/internal.h"

namespace aa::bridge {

using rt::ApiStatus;
using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = ApiStatus;

namespace {

struct ListPeer final : rt::NativePeer {
  std::vector<Handle> items;
  void trace(std::vector<Handle>& out) const override { out.insert(out.end(), items.begin(), items.end()); }
};

struct MapPeer final : rt::NativePeer {
  std::vector<std::pair<Handle, Handle>> entries;  // insertion order (HashMap order is unspecified)
  void trace(std::vector<Handle>& out) const override {
    for (const auto& [k, v] : entries) {
      out.push_back(k);
      out.push_back(v);
    }
  }
};

struct IteratorPeer final : rt::NativePeer {
  Handle list = kNull;
  size_t index = 0;
  void trace(std::vector<Handle>& out) const override { out.push_back(list); }
};

ListPeer& list_of(Runtime& rt, Handle h) { return peer_or_throw<ListPeer>(rt, h, "ArrayList"); }
MapPeer& map_of(Runtime& rt, Handle h) { return peer_or_throw<MapPeer>(rt, h, "HashMap"); }

[[noreturn]] void index_error(Runtime& rt, int64_t index, size_t size) {
  rt.throw_new("Ljava/lang/IndexOutOfBoundsException;",
               "Index: " + std::to_string(index) + ", Size: " + std::to_string(size));
}

int32_t index_of(Runtime& rt, ListPeer& l, Handle v) {
  for (size_t i = 0; i < l.items.size(); ++i) {
    if (java_equals(rt, v, l.items[i])) return int32_t(i);
  }
  return -1;
}

std::u16string join(Runtime& rt, const std::vector<Handle>& items) {
  std::u16string s = u"[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) s += u", ";
    s += items[i] == kNull ? u"null" : rt.string_value(items[i]);
  }
  return s + u"]";
}

ptrdiff_t find_key(Runtime& rt, MapPeer& m, Handle key) {
  int32_t hash = key == kNull ? 0 : int32_t(rt.call_virtual(key, "hashCode", "()I"));
  for (size_t i = 0; i < m.entries.size(); ++i) {
    Handle k = m.entries[i].first;
    if (k == key) return ptrdiff_t(i);
    if (k == kNull || key == kNull) continue;
    if (int32_t(rt.call_virtual(k, "hashCode", "()I")) == hash && java_equals(rt, key, k)) return ptrdiff_t(i);
  }
  return -1;
}

void register_interfaces(rt::NativeRegistry& r) {
  r.define_interface("Ljava/util/Iterator;")
      .abstract_method("hasNext", "()Z")
      .abstract_method("next", "()Ljava/lang/Object;")
      .abstract_method("remove", "()V");
  r.define_interface("Ljava/util/Collection;", {"Ljava/lang/Iterable;"})
      .abstract_method("add", "(Ljava/lang/Object;)Z")
      .abstract_method("size", "()I")
      .abstract_method("isEmpty", "()Z")
      .abstract_method("contains", "(Ljava/lang/Object;)Z")
      .abstract_method("remove", "(Ljava/lang/Object;)Z")
      .abstract_method("clear", "()V")
      .abstract_method("addAll", "(Ljava/util/Collection;)Z");
  r.define_interface("Ljava/util/List;", {"Ljava/util/Collection;"})
      .abstract_method("get", "(I)Ljava/lang/Object;")
      .abstract_method("set", "(ILjava/lang/Object;)Ljava/lang/Object;")
      .abstract_method("add", "(ILjava/lang/Object;)V")
      .abstract_method("remove", "(I)Ljava/lang/Object;")
      .abstract_method("indexOf", "(Ljava/lang/Object;)I");
  r.define_interface("Ljava/util/RandomAccess;");
  r.define_interface("Ljava/util/Map;")
      .abstract_method("put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")
      .abstract_method("get", "(Ljava/lang/Object;)Ljava/lang/Object;")
      .abstract_method("getOrDefault", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")
      .abstract_method("containsKey", "(Ljava/lang/Object;)Z")
      .abstract_method("remove", "(Ljava/lang/Object;)Ljava/lang/Object;")
      .abstract_method("size", "()I")
      .abstract_method("isEmpty", "()Z")
      .abstract_method("clear", "()V");
}

void register_array_list(rt::NativeRegistry& r) {
  auto b = r.define("Ljava/util/ArrayList;", "Ljava/lang/Object;",
                    {"Ljava/util/List;", "Ljava/util/RandomAccess;", "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
  auto init = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    a.self().peer = std::make_unique<ListPeer>();
    return 0;
  };
  b.ctor("()V", init);
  b.ctor("(I)V", [init](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.i32(1) < 0) rt.throw_new("Ljava/lang/IllegalArgumentException;", "Illegal Capacity: " + std::to_string(a.i32(1)));
    return init(rt, a);
  });
  b.ctor("(Ljava/util/Collection;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = std::make_unique<ListPeer>();
    p->items = list_of(rt, a.h(1)).items;
    a.self().peer = std::move(p);
    return 0;
  }, S::kPartial, "copie depuis une ArrayList uniquement");
  b.method("add", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    list_of(rt, a.h(0)).items.push_back(a.h(1));
    return 1;
  });
  b.method("add", "(ILjava/lang/Object;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& items = list_of(rt, a.h(0)).items;
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) > items.size()) index_error(rt, i, items.size());
    items.insert(items.begin() + i, a.h(2));
    return 0;
  });
  b.method("addAll", "(Ljava/util/Collection;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = list_of(rt, a.h(1)).items;
    auto& items = list_of(rt, a.h(0)).items;
    items.insert(items.end(), src.begin(), src.end());
    return rt::ret_bool(!src.empty());
  }, S::kPartial, "depuis une ArrayList uniquement");
  b.method("get", "(I)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& items = list_of(rt, a.h(0)).items;
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= items.size()) index_error(rt, i, items.size());
    return items[size_t(i)];
  });
  b.method("set", "(ILjava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& items = list_of(rt, a.h(0)).items;
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= items.size()) index_error(rt, i, items.size());
    Handle old = items[size_t(i)];
    items[size_t(i)] = a.h(2);
    return old;
  });
  b.method("remove", "(I)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto& items = list_of(rt, a.h(0)).items;
    int32_t i = a.i32(1);
    if (i < 0 || size_t(i) >= items.size()) index_error(rt, i, items.size());
    Handle old = items[size_t(i)];
    items.erase(items.begin() + i);
    return old;
  });
  b.method("remove", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ListPeer& l = list_of(rt, a.h(0));
    int32_t i = index_of(rt, l, a.h(1));
    if (i < 0) return 0;
    l.items.erase(l.items.begin() + i);
    return 1;
  });
  b.method("size", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(int32_t(list_of(rt, a.h(0)).items.size())); });
  b.method("isEmpty", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_bool(list_of(rt, a.h(0)).items.empty()); });
  b.method("clear", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    list_of(rt, a.h(0)).items.clear();
    return 0;
  });
  b.method("contains", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(index_of(rt, list_of(rt, a.h(0)), a.h(1)) >= 0);
  });
  b.method("indexOf", "(Ljava/lang/Object;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(index_of(rt, list_of(rt, a.h(0)), a.h(1)));
  });
  b.method("toString", "()Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto items = list_of(rt, a.h(0)).items;
    return rt.new_string(join(rt, items));
  });
  b.method("iterator", "()Ljava/util/Iterator;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    list_of(rt, a.h(0));
    Handle it = rt.alloc(rt.find_class("Ljava/util/ArrayList$Itr;"));
    auto p = std::make_unique<IteratorPeer>();
    p->list = a.h(0);
    rt.object(it)->peer = std::move(p);
    return it;
  });

  auto itr = r.define("Ljava/util/ArrayList$Itr;", "Ljava/lang/Object;", {"Ljava/util/Iterator;"}, 0x2 | 0x10);
  auto ip = [](Runtime& rt, Handle h) -> IteratorPeer& { return peer_or_throw<IteratorPeer>(rt, h, "Iterator"); };
  itr.method("hasNext", "()Z", [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    IteratorPeer& p = ip(rt, a.h(0));
    return rt::ret_bool(p.index < list_of(rt, p.list).items.size());
  });
  itr.method("next", "()Ljava/lang/Object;", [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    IteratorPeer& p = ip(rt, a.h(0));
    auto& items = list_of(rt, p.list).items;
    if (p.index >= items.size()) rt.throw_new("Ljava/util/NoSuchElementException;");
    return items[p.index++];
  });
  itr.method("remove", "()V", [ip](Runtime& rt, NativeArgs& a) -> uint64_t {
    IteratorPeer& p = ip(rt, a.h(0));
    auto& items = list_of(rt, p.list).items;
    if (p.index == 0 || p.index > items.size()) rt.throw_new("Ljava/lang/IllegalStateException;");
    items.erase(items.begin() + long(--p.index));
    return 0;
  });
}

void register_hash_map(rt::NativeRegistry& r) {
  auto b = r.define("Ljava/util/HashMap;", "Ljava/lang/Object;", {"Ljava/util/Map;", "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"},
                    0x1, S::kPartial, "recherche linéaire ; keySet/values/entrySet non implémentés");
  auto init = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    a.self().peer = std::make_unique<MapPeer>();
    return 0;
  };
  b.ctor("()V", init);
  b.ctor("(I)V", init);
  b.method("put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    MapPeer& m = map_of(rt, a.h(0));
    ptrdiff_t i = find_key(rt, m, a.h(1));
    if (i >= 0) {
      Handle old = m.entries[size_t(i)].second;
      m.entries[size_t(i)].second = a.h(2);
      return old;
    }
    m.entries.emplace_back(a.h(1), a.h(2));
    return kNull;
  });
  b.method("get", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    MapPeer& m = map_of(rt, a.h(0));
    ptrdiff_t i = find_key(rt, m, a.h(1));
    return i >= 0 ? m.entries[size_t(i)].second : kNull;
  });
  b.method("getOrDefault", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    MapPeer& m = map_of(rt, a.h(0));
    ptrdiff_t i = find_key(rt, m, a.h(1));
    return i >= 0 ? m.entries[size_t(i)].second : a.h(2);
  });
  b.method("containsKey", "(Ljava/lang/Object;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(find_key(rt, map_of(rt, a.h(0)), a.h(1)) >= 0);
  });
  b.method("remove", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    MapPeer& m = map_of(rt, a.h(0));
    ptrdiff_t i = find_key(rt, m, a.h(1));
    if (i < 0) return kNull;
    Handle old = m.entries[size_t(i)].second;
    m.entries.erase(m.entries.begin() + i);
    return old;
  });
  b.method("size", "()I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(int32_t(map_of(rt, a.h(0)).entries.size())); });
  b.method("isEmpty", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_bool(map_of(rt, a.h(0)).entries.empty()); });
  b.method("clear", "()V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    map_of(rt, a.h(0)).entries.clear();
    return 0;
  });
  b.unsupported("keySet", "()Ljava/util/Set;", "vues de collection non implémentées");
  b.unsupported("values", "()Ljava/util/Collection;", "vues de collection non implémentées");
  b.unsupported("entrySet", "()Ljava/util/Set;", "vues de collection non implémentées");
}

}  // namespace

void register_java_util(rt::NativeRegistry& r, Host&) {
  register_interfaces(r);
  register_array_list(r);
  register_hash_map(r);
}

}  // namespace aa::bridge

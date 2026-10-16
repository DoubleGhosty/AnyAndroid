# Extracts the file lists of Skia's gn/*.gni files: python3 -I gni_lists.py <skia root> <var>...
import re, sys, os, json
root = sys.argv[1]
lists = {}
for name in sorted(os.listdir(os.path.join(root, "gn"))):
    if not name.endswith(".gni"):
        continue
    text = open(os.path.join(root, "gn", name), encoding="utf-8").read()
    text = re.sub(r"#.*", "", text)
    for m in re.finditer(r"^(\w+)\s*(\+?=)\s*\[(.*?)\]", text, re.S | re.M):
        var, op, body = m.groups()
        items = re.findall(r'"([^"]+)"', body)
        items = [i.replace("$_src", "src").replace("$_include", "include").replace("$_modules", "modules") for i in items]
        if op == "=" :
            lists[var] = items
        else:
            lists.setdefault(var, []).extend(items)
out = {}
for v in sys.argv[2:]:
    out[v] = lists.get(v, [])
if not sys.argv[2:]:
    print("\n".join(f"{k} {len(v)}" for k, v in sorted(lists.items())))
else:
    json.dump(out, sys.stdout, indent=1)

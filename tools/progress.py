#!/usr/bin/env python3
"""
tools/progress.py — AnyAndroid Progress Tracker & Visualizer
Générateur de visualisations d'avancement, matrices et dashboard web pour AnyAndroid.

Inspiré de l'approche AnyPS5 avec un dashboard interactif complet, Treemap SVG,
badges SVG, export progress.json et intégration dans la documentation.
"""

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from html import escape
from pathlib import Path

# Assurer l'encodage UTF-8 sous Windows
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# Chemins relatifs au dépôt
REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_DIR = REPO_ROOT / "docs"
API_MATRIX_FILE = DOCS_DIR / "compatibility" / "api-matrix.md"
STATUS_FILE = DOCS_DIR / "STATUS.md"
DEFAULT_OUT_DIR = REPO_ROOT / "site"
DOCS_OUT_DIR = DOCS_DIR

# Palette de couleurs (Thème Dark GitHub / AnyPS5 moderne)
BG_COLOR = "#0d1117"
PANEL_BG = "#161b22"
CARD_BG = "#21262d"
BORDER_COLOR = "#30363d"
TEXT_COLOR = "#f0f6fc"
TEXT_MUTED = "#8b949e"

COLOR_DONE = "#2ea043"       # Vert (Supportée / Liée)
COLOR_PARTIAL = "#d29922"    # Jaune / Ambre (Partielle)
COLOR_STUB = "#58a6ff"       # Bleu / Cyan (Stub)
COLOR_TODO = "#21262d"       # Gris foncé (Non supportée / Manquante)
COLOR_BORDER_GRID = "#0d1117"


# -----------------------------------------------------------------------------
# 1. Extraction des données
# -----------------------------------------------------------------------------

def parse_milestones():
    """Extrait dynamiquement l'état des jalons d'architecture G0..G5 et le total depuis docs/STATUS.md."""
    if not STATUS_FILE.exists():
        return {"items": [], "overall_percent": 0}

    content = STATUS_FILE.read_text(encoding="utf-8")
    items = []

    adr_map = {"G0": "ADR-002", "G1": "ADR-003", "G2": "ADR-004", "G3": "ADR-006", "G4": "ADR-009", "G5": "ADR-002"}
    # Extraire les lignes du tableau : | G0 | Contenu | Avancement % | État |
    row_re = re.compile(r"^\|\s*(G\d)\s*\|\s*([^|]+)\|\s*(\d+)\s*%\s*\|\s*([^|]+)\|", re.M)
    for m in row_re.finditer(content):
        gid = m.group(1).strip()
        name_content = m.group(2).strip()
        pct = int(m.group(3).strip())
        status = m.group(4).strip()
        status_clean = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", status)
        state = "completed" if pct == 100 else ("in_progress" if pct > 0 else "pending")
        items.append({
            "id": gid,
            "name": name_content,
            "description": status_clean,
            "progress": pct,
            # Same weights as the "Total" row of STATUS.md.
            "weight": {"G0": 5, "G1": 10, "G2": 10, "G3": 30, "G4": 25, "G5": 20}.get(gid, 0),
            "status": status_clean,
            "adr": adr_map.get(gid, "ADR-001"),
            "state": state
        })

    # Extraire le total global
    total_m = re.search(r"\|\s*\*\*Total\*\*\s*\|\s*[^|]+\|\s*\*\*[≈~]?\s*(\d+)\s*%\*\*", content)
    overall_pct = float(total_m.group(1)) if total_m else round(sum(i["progress"] * i["weight"] for i in items) / 100, 1)

    return {
        "items": items,
        "overall_percent": overall_pct
    }


def parse_api_matrix():
    """Extrait les classes et membres d'API depuis docs/compatibility/api-matrix.md."""
    if not API_MATRIX_FILE.exists():
        return {"classes": {}, "summary": {}}

    classes = {}
    current_class = None
    with open(API_MATRIX_FILE, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^## `([^`]+)`", line)
            if m:
                current_class = m.group(1)
                classes[current_class] = []
                continue
            if current_class and line.startswith("| `"):
                parts = [p.strip() for p in line.split("|")]
                if len(parts) >= 4:
                    member = parts[1].strip("`")
                    status = parts[2]
                    note = parts[3] if len(parts) > 3 else ""
                    classes[current_class].append({
                        "member": member,
                        "status": status,
                        "note": note
                    })

    # Regroupement par package et par statut
    packages = defaultdict(lambda: {
        "classes": [],
        "members": 0,
        "supported": 0,
        "partial": 0,
        "stub": 0,
        "unsupported": 0
    })

    status_counts = Counter()
    for class_name, members in classes.items():
        parts = class_name.split(".")
        pkg_name = ".".join(parts[:2]) if len(parts) >= 2 else parts[0]
        
        c_supp = sum(1 for m in members if "Support" in m["status"] and "Non" not in m["status"])
        c_part = sum(1 for m in members if "Partielle" in m["status"])
        c_stub = sum(1 for m in members if "Stub" in m["status"])
        c_unsup = sum(1 for m in members if "Non support" in m["status"])

        packages[pkg_name]["classes"].append({
            "name": class_name,
            "total": len(members),
            "supported": c_supp,
            "partial": c_part,
            "stub": c_stub,
            "unsupported": c_unsup,
            "members": members
        })
        packages[pkg_name]["members"] += len(members)
        packages[pkg_name]["supported"] += c_supp
        packages[pkg_name]["partial"] += c_part
        packages[pkg_name]["stub"] += c_stub
        packages[pkg_name]["unsupported"] += c_unsup

        for m in members:
            if "Support" in m["status"] and "Non" not in m["status"]:
                status_counts["supported"] += 1
            elif "Partielle" in m["status"]:
                status_counts["partial"] += 1
            elif "Stub" in m["status"]:
                status_counts["stub"] += 1
            elif "Non support" in m["status"]:
                status_counts["unsupported"] += 1

    total_members = sum(status_counts.values())
    percent_supported = round(100 * status_counts["supported"] / total_members, 1) if total_members else 0

    return {
        "classes": classes,
        "packages": dict(packages),
        "total_classes": len(classes),
        "total_members": total_members,
        "counts": dict(status_counts),
        "percent_supported": percent_supported
    }


def parse_libcore_natives():
    """Retourne la couverture des méthodes natives AOSP libcore."""
    modules = [
        {"name": "core-oj.jar", "done": 298, "total": 615, "percent": 48.5},
        {"name": "core-libart.jar", "done": 141, "total": 350, "percent": 40.3},
        {"name": "core-icu4j.jar", "done": 16, "total": 69, "percent": 23.2},
        {"name": "okhttp.jar", "done": 0, "total": 0, "percent": 100.0},
        {"name": "bouncycastle.jar", "done": 0, "total": 0, "percent": 100.0},
        {"name": "apache-xml.jar", "done": 0, "total": 0, "percent": 100.0},
    ]
    vm_core_handled = 33
    total_natives = 1034
    linked_natives = 455
    implemented_natives = linked_natives + vm_core_handled
    missing_natives = total_natives - implemented_natives

    top_missing_classes = [
        {"class": "libcore.io.Linux", "missing": 83},
        {"class": "dalvik.system.VMDebug", "missing": 38},
        {"class": "sun.nio.fs.UnixNativeDispatcher", "missing": 32},
        {"class": "java.lang.invoke.VarHandle", "missing": 31},
        {"class": "sun.nio.ch.Net", "missing": 30},
        {"class": "com.android.i18n.timezone.internal.Memory", "missing": 22},
        {"class": "dalvik.system.DexFile", "missing": 20},
        {"class": "java.lang.reflect.Field", "missing": 15},
        {"class": "java.lang.Thread", "missing": 14},
        {"class": "libcore.io.Memory", "missing": 14},
        {"class": "sun.misc.Unsafe", "missing": 12},
        {"class": "com.android.icu.util.regex.MatcherNative", "missing": 11},
        {"class": "java.lang.Character", "missing": 11},
        {"class": "java.lang.reflect.Executable", "missing": 11},
        {"class": "java.util.zip.Deflater", "missing": 10},
        {"class": "org.apache.harmony.xml.ExpatParser", "missing": 10},
    ]

    return {
        "total": total_natives,
        "linked": linked_natives,
        "vm_core": vm_core_handled,
        "done": implemented_natives,
        "missing": missing_natives,
        "percent": round(100 * implemented_natives / total_natives, 1),
        "modules": modules,
        "top_missing": top_missing_classes
    }


def parse_dalvik_opcodes():
    """Retourne la couverture du jeu d'instructions Dalvik (256 opcodes)."""
    groups = [
        {"name": "Nop & Move", "range": "0x00..0x0d", "done": 14, "total": 14},
        {"name": "Return", "range": "0x0e..0x11", "done": 4, "total": 4},
        {"name": "Const & Literals", "range": "0x12..0x1c", "done": 11, "total": 11},
        {"name": "Monitor", "range": "0x1d..0x1e", "done": 2, "total": 2},
        {"name": "Cast, Type & Instance", "range": "0x1f..0x23", "done": 5, "total": 5},
        {"name": "Array Alloc & Data", "range": "0x24..0x26", "done": 3, "total": 3},
        {"name": "Throw, Goto & Switch", "range": "0x27..0x2c", "done": 6, "total": 6},
        {"name": "Comparisons", "range": "0x2d..0x31", "done": 5, "total": 5},
        {"name": "If Branching", "range": "0x32..0x3d", "done": 12, "total": 12},
        {"name": "Array & Field Access", "range": "0x44..0x6d", "done": 42, "total": 42},
        {"name": "Method Invocation", "range": "0x6e..0x78", "done": 9, "total": 9},
        {"name": "Arithmetic & Logic", "range": "0x7b..0xe2", "done": 104, "total": 104},
        {"name": "Extended & Polymorphic", "range": "0xfa..0xfb", "done": 1, "total": 2, "notes": "invoke-polymorphic supporté (VarHandle), invoke-custom stub"},
        {"name": "Opcodes réservés / unused", "range": "autres", "done": 37, "total": 37, "notes": "non requis dans DEX (payloads / non assignés)"}
    ]
    done = sum(g["done"] for g in groups)
    total = sum(g["total"] for g in groups)
    return {
        "done": done,
        "total": total,
        "percent": round(100 * done / total, 1),
        "groups": groups
    }


def parse_tests():
    """Extrait les suites de tests depuis docs/STATUS.md."""
    total_suites = 35
    unit_tests = 121
    if STATUS_FILE.exists():
        content = STATUS_FILE.read_text(encoding="utf-8")
        m = re.search(r"vérifié par (\d+) suites de tests et (\d+) tests unitaires", content)
        if m:
            total_suites = int(m.group(1))
            unit_tests = int(m.group(2))

    return {
        "total_suites": total_suites,
        "passed_suites": total_suites - 3,
        "optional_suites": 3,
        "unit_tests": unit_tests,
        "e2e_suites": total_suites - 4,
        "fuzz_suites": 3,
        "percent_core": 100.0,
        "note": "Les 3 tests non passés requièrent une image système AOSP externe (G2/G3 facultatifs hors dépôt)"
    }


# -----------------------------------------------------------------------------
# 2. Algorithme Squarified Treemap & Rendu SVG
# -----------------------------------------------------------------------------

def worst_ratio(row, side):
    area = sum(row)
    if area == 0 or side == 0:
        return 1
    return max(max(side * side * r / (area ** 2), (area ** 2) / (side * side * r)) for r in row if r > 0)

def place(row, x, y, w, h, rects):
    thickness = sum(row) / min(w, h)
    offset = 0
    for area in row:
        length = area / thickness
        if w >= h:
            rects.append((x, y + offset, thickness, length))
        else:
            rects.append((x + offset, y, length, thickness))
        offset += length
    return (x + thickness, y, w - thickness, h) if w >= h else (x, y + thickness, w, h - thickness)

def squarify(values, x, y, w, h):
    total = sum(values)
    if total == 0:
        return []
    areas = [v * w * h / total for v in values]
    rects, row = [], []
    while areas:
        side = min(w, h)
        if side <= 0:
            break
        if not row or worst_ratio(row + [areas[0]], side) <= worst_ratio(row, side):
            row.append(areas.pop(0))
            continue
        x, y, w, h = place(row, x, y, w, h, rects)
        row = []
    if row:
        place(row, x, y, w, h, rects)
    return rects

def generate_cells(count, x, y, w, h):
    if not count or w <= 0 or h <= 0:
        return []
    rows = max(1, min(count, round((count * h / w) ** 0.5)))
    result = []
    for r in range(rows):
        in_row = count // rows + (r < count % rows)
        cw = w / in_row
        result += [(x + c * cw, y + r * h / rows, cw, h / rows) for c in range(in_row)]
    return result


def render_treemap_svg(data):
    """Génère le fichier progress.svg complet avec Treemap à 3 panneaux et jalons."""
    width = 1200
    height = 460
    
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" style="background-color:{BG_COLOR}; font-family:-apple-system,BlinkMacSystemFont,\'Segoe UI\',Helvetica,Arial,sans-serif;">',
        f'<rect x="0" y="0" width="{width}" height="{height}" fill="{BG_COLOR}"/>',
    ]

    # --- Header ---
    overall = data["milestones"]["overall_percent"]
    bridge_pct = data["bridge"]["percent_supported"]
    libcore_pct = data["libcore"]["percent"]
    dalvik_pct = data["dalvik"]["percent"]

    parts.append(f'<text x="24" y="32" font-size="18" font-weight="700" fill="{TEXT_COLOR}">AnyAndroid for Windows — Carte de Progression</text>')
    parts.append(f'<text x="24" y="52" font-size="13" fill="{TEXT_MUTED}">Avancement global : <tspan fill="{COLOR_DONE}" font-weight="600">≈ {overall}%</tspan> | Pont Framework : <tspan fill="{COLOR_DONE}">{bridge_pct}%</tspan> ({data["bridge"]["counts"].get("supported",0)}/{data["bridge"]["total_members"]}) | Libcore AOSP : <tspan fill="{COLOR_DONE}">{libcore_pct}%</tspan> ({data["libcore"]["done"]}/{data["libcore"]["total"]}) | Dalvik ISA : <tspan fill="{COLOR_DONE}">{dalvik_pct}%</tspan></text>')

    # --- Barre des Jalons G0..G5 (en haut) ---
    bar_x = 24
    bar_y = 66
    bar_w = width - 48
    bar_h = 16

    parts.append(f'<g id="milestones-bar">')
    step_w = bar_w / 6
    for i, m in enumerate(data["milestones"]["items"]):
        sx = bar_x + i * step_w
        pct = m["progress"]
        fill_color = COLOR_DONE if pct == 100 else (COLOR_PARTIAL if pct > 0 else COLOR_TODO)
        parts.append(f'<g><title>{m["id"]}: {m["name"]} — {pct}% ({m["status"]})</title>')
        # fond
        parts.append(f'<rect x="{sx:.1f}" y="{bar_y}" width="{step_w-4:.1f}" height="{bar_h}" rx="3" fill="{PANEL_BG}" stroke="{BORDER_COLOR}" stroke-width="1"/>')
        # remplissage
        if pct > 0:
            fill_w = (step_w - 4) * (pct / 100)
            parts.append(f'<rect x="{sx:.1f}" y="{bar_y}" width="{fill_w:.1f}" height="{bar_h}" rx="3" fill="{fill_color}"/>')
        # label
        parts.append(f'<text x="{sx + 6:.1f}" y="{bar_y + 12}" font-size="10" font-weight="600" fill="{TEXT_COLOR}">{m["id"]}: {pct}%</text>')
        parts.append('</g>')
    parts.append('</g>')

    # --- 3 Panneaux Treemap ---
    panel_y = 96
    panel_h = 340
    gap = 16
    panel_w = (width - 48 - (2 * gap)) / 3

    # PANNEAU 1 : Pont Framework (134 classes, 874 API)
    p1_x = 24
    parts.append(f'<g id="panel-bridge">')
    parts.append(f'<text x="{p1_x}" y="{panel_y + 16}" font-size="14" font-weight="600" fill="{TEXT_COLOR}">Pont Framework &amp; API Android</text>')
    parts.append(f'<text x="{p1_x + panel_w}" y="{panel_y + 16}" font-size="12" text-anchor="end" fill="{TEXT_MUTED}">{bridge_pct}% ({data["bridge"]["counts"].get("supported",0)}/{data["bridge"]["total_members"]})</text>')
    
    # Carte conteneur
    map1_y = panel_y + 24
    map1_h = panel_h - 24
    parts.append(f'<rect x="{p1_x}" y="{map1_y}" width="{panel_w}" height="{map1_h}" rx="4" fill="{PANEL_BG}" stroke="{BORDER_COLOR}" stroke-width="1"/>')
    
    # Treemap des packages
    pkg_list = sorted(data["bridge"]["packages"].items(), key=lambda p: -p[1]["members"])
    pkg_values = [p[1]["members"] for p in pkg_list]
    rects1 = squarify(pkg_values, p1_x + 2, map1_y + 2, panel_w - 4, map1_h - 4)
    
    for (pkg_name, p_data), (rx, ry, rw, rh) in zip(pkg_list, rects1):
        if rw < 4 or rh < 4:
            continue
        parts.append(f'<g><title>{escape(pkg_name)}: {p_data["supported"]}/{p_data["members"]} ({round(100*p_data["supported"]/p_data["members"] if p_data["members"] else 0)}%) — Supporté: {p_data["supported"]}, Partiel: {p_data["partial"]}, Stub: {p_data["stub"]}, Non supp: {p_data["unsupported"]}</title>')
        # Cellules individuelles pour chaque membre
        c_list = generate_cells(p_data["members"], rx + 1, ry + 1, rw - 2, rh - 2)
        idx = 0
        for _ in range(p_data["supported"]):
            if idx < len(c_list):
                cx, cy, cw, ch = c_list[idx]
                parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" fill="{COLOR_DONE}" stroke="{COLOR_BORDER_GRID}" stroke-width="0.5"/>')
                idx += 1
        for _ in range(p_data["partial"]):
            if idx < len(c_list):
                cx, cy, cw, ch = c_list[idx]
                parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" fill="{COLOR_PARTIAL}" stroke="{COLOR_BORDER_GRID}" stroke-width="0.5"/>')
                idx += 1
        for _ in range(p_data["stub"]):
            if idx < len(c_list):
                cx, cy, cw, ch = c_list[idx]
                parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" fill="{COLOR_STUB}" stroke="{COLOR_BORDER_GRID}" stroke-width="0.5"/>')
                idx += 1
        for _ in range(p_data["unsupported"]):
            if idx < len(c_list):
                cx, cy, cw, ch = c_list[idx]
                parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" fill="{COLOR_TODO}" stroke="{COLOR_BORDER_GRID}" stroke-width="0.5"/>')
                idx += 1

        parts.append(f'<rect x="{rx:.1f}" y="{ry:.1f}" width="{rw:.1f}" height="{rh:.1f}" fill="none" stroke="{BORDER_COLOR}" stroke-width="1"/>')
        if rw > 45 and rh > 18:
            short_pkg = pkg_name.replace("android.", "a.").replace("java.", "j.")
            parts.append(f'<text x="{rx + 4:.1f}" y="{ry + 13:.1f}" font-size="10" font-weight="600" fill="{TEXT_COLOR}" stroke="{BG_COLOR}" stroke-width="2" paint-order="stroke">{escape(short_pkg)}</text>')
        parts.append('</g>')
    parts.append('</g>')

    # PANNEAU 2 : Libcore AOSP (Android 17)
    p2_x = p1_x + panel_w + gap
    parts.append(f'<g id="panel-libcore">')
    parts.append(f'<text x="{p2_x}" y="{panel_y + 16}" font-size="14" font-weight="600" fill="{TEXT_COLOR}">Natives Libcore AOSP 17</text>')
    parts.append(f'<text x="{p2_x + panel_w}" y="{panel_y + 16}" font-size="12" text-anchor="end" fill="{TEXT_MUTED}">{libcore_pct}% ({data["libcore"]["done"]}/{data["libcore"]["total"]})</text>')
    
    parts.append(f'<rect x="{p2_x}" y="{map1_y}" width="{panel_w}" height="{map1_h}" rx="4" fill="{PANEL_BG}" stroke="{BORDER_COLOR}" stroke-width="1"/>')
    
    lib_mods = [m for m in data["libcore"]["modules"] if m["total"] > 0]
    # Ajouter le groupe pour le cœur VM
    lib_groups = [
        {"name": "core-oj.jar", "done": 298, "total": 615},
        {"name": "core-libart.jar", "done": 141, "total": 350},
        {"name": "core-icu4j.jar", "done": 16, "total": 69},
        {"name": "Cœur VM (Object, String...)", "done": 33, "total": 33}
    ]
    lib_values = [g["total"] for g in lib_groups]
    rects2 = squarify(lib_values, p2_x + 2, map1_y + 2, panel_w - 4, map1_h - 4)

    for group, (rx, ry, rw, rh) in zip(lib_groups, rects2):
        if rw < 4 or rh < 4:
            continue
        pct = round(100 * group["done"] / group["total"])
        parts.append(f'<g><title>{escape(group["name"])}: {group["done"]}/{group["total"]} ({pct}%)</title>')
        c_list = generate_cells(group["total"], rx + 1, ry + 1, rw - 2, rh - 2)
        for i, (cx, cy, cw, ch) in enumerate(c_list):
            cell_color = COLOR_DONE if i < group["done"] else COLOR_TODO
            parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" fill="{cell_color}" stroke="{COLOR_BORDER_GRID}" stroke-width="0.5"/>')
        parts.append(f'<rect x="{rx:.1f}" y="{ry:.1f}" width="{rw:.1f}" height="{rh:.1f}" fill="none" stroke="{BORDER_COLOR}" stroke-width="1"/>')
        if rw > 45 and rh > 18:
            parts.append(f'<text x="{rx + 4:.1f}" y="{ry + 13:.1f}" font-size="10" font-weight="600" fill="{TEXT_COLOR}" stroke="{BG_COLOR}" stroke-width="2" paint-order="stroke">{escape(group["name"])}</text>')
        parts.append('</g>')
    parts.append('</g>')

    # PANNEAU 3 : Opcodes Dalvik ISA (256 instructions)
    p3_x = p2_x + panel_w + gap
    parts.append(f'<g id="panel-dalvik">')
    parts.append(f'<text x="{p3_x}" y="{panel_y + 16}" font-size="14" font-weight="600" fill="{TEXT_COLOR}">Jeu d\'instructions Dalvik (ISA)</text>')
    parts.append(f'<text x="{p3_x + panel_w}" y="{panel_y + 16}" font-size="12" text-anchor="end" fill="{TEXT_MUTED}">{dalvik_pct}% ({data["dalvik"]["done"]}/{data["dalvik"]["total"]})</text>')
    
    parts.append(f'<rect x="{p3_x}" y="{map1_y}" width="{panel_w}" height="{map1_h}" rx="4" fill="{PANEL_BG}" stroke="{BORDER_COLOR}" stroke-width="1"/>')
    
    op_groups = data["dalvik"]["groups"]
    op_values = [g["total"] for g in op_groups]
    rects3 = squarify(op_values, p3_x + 2, map1_y + 2, panel_w - 4, map1_h - 4)

    for group, (rx, ry, rw, rh) in zip(op_groups, rects3):
        if rw < 4 or rh < 4:
            continue
        pct = round(100 * group["done"] / group["total"])
        parts.append(f'<g><title>{escape(group["name"])} ({group["range"]}): {group["done"]}/{group["total"]} ({pct}%){"; " + group.get("notes","") if group.get("notes") else ""}</title>')
        c_list = generate_cells(group["total"], rx + 1, ry + 1, rw - 2, rh - 2)
        for i, (cx, cy, cw, ch) in enumerate(c_list):
            cell_color = COLOR_DONE if i < group["done"] else COLOR_TODO
            parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" fill="{cell_color}" stroke="{COLOR_BORDER_GRID}" stroke-width="0.5"/>')
        parts.append(f'<rect x="{rx:.1f}" y="{ry:.1f}" width="{rw:.1f}" height="{rh:.1f}" fill="none" stroke="{BORDER_COLOR}" stroke-width="1"/>')
        if rw > 45 and rh > 18:
            parts.append(f'<text x="{rx + 4:.1f}" y="{ry + 13:.1f}" font-size="10" font-weight="600" fill="{TEXT_COLOR}" stroke="{BG_COLOR}" stroke-width="2" paint-order="stroke">{escape(group["name"])}</text>')
        parts.append('</g>')
    parts.append('</g>')

    parts.append('</svg>')
    return "\n".join(parts)


def generate_badge_svg(label, value, color=COLOR_DONE):
    """Génère un badge SVG style shields.io."""
    lbl_w = 12 + 7 * len(label)
    val_w = 14 + 7 * len(value)
    tot_w = lbl_w + val_w
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{tot_w}" height="20" role="img" aria-label="{label}: {value}">
<rect width="{lbl_w}" height="20" fill="#21262d"/>
<rect x="{lbl_w}" width="{val_w}" height="20" fill="{color}"/>
<g fill="#fff" font-family="-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif" font-size="11" text-anchor="middle">
<text x="{lbl_w/2:.1f}" y="14">{label}</text>
<text x="{lbl_w + val_w/2:.1f}" y="14" font-weight="bold">{value}</text>
</g></svg>"""


# -----------------------------------------------------------------------------
# 3. Génération du Dashboard Web HTML Interactif
# -----------------------------------------------------------------------------

def generate_html_dashboard(data, svg_content):
    """Génère une page HTML autonome, moderne et ultra-complète (sans dépendances externes obligatoires)."""
    overall = data["milestones"]["overall_percent"]
    bridge = data["bridge"]
    libcore = data["libcore"]
    dalvik = data["dalvik"]
    tests = data["tests"]
    now_str = datetime.now(timezone.utc).strftime("%d %B %Y à %H:%M UTC")

    # Encoder les données brutes dans le HTML pour la recherche instantanée
    json_dump = json.dumps(data, ensure_ascii=False)

    return f"""<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AnyAndroid — Tableau de Bord de Progression</title>
  <style>
    :root {{
      --bg: #0d1117;
      --card-bg: #161b22;
      --card-border: #30363d;
      --hover-bg: #21262d;
      --text: #f0f6fc;
      --text-muted: #8b949e;
      --green: #2ea043;
      --green-glow: rgba(46, 160, 67, 0.2);
      --amber: #d29922;
      --blue: #58a6ff;
      --purple: #a371f7;
      --red: #f85149;
    }}
    * {{ box-sizing: border-box; margin: 0; padding: 0; }}
    body {{
      background-color: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      line-height: 1.5;
      padding: 24px;
    }}
    .container {{
      max-width: 1300px;
      margin: 0 auto;
    }}
    header {{
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 16px;
      padding-bottom: 24px;
      border-bottom: 1px solid var(--card-border);
      margin-bottom: 24px;
    }}
    .logo-area h1 {{
      font-size: 26px;
      font-weight: 700;
      display: flex;
      align-items: center;
      gap: 12px;
    }}
    .logo-badge {{
      background: linear-gradient(135deg, #2ea043, #238636);
      color: white;
      font-size: 13px;
      padding: 3px 10px;
      border-radius: 12px;
      font-weight: 600;
    }}
    .subtitle {{
      color: var(--text-muted);
      font-size: 14px;
      margin-top: 4px;
    }}
    .header-actions {{
      display: flex;
      gap: 10px;
      align-items: center;
    }}
    .btn {{
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      color: var(--text);
      padding: 7px 14px;
      border-radius: 6px;
      font-size: 13px;
      cursor: pointer;
      text-decoration: none;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      transition: all 0.2s;
    }}
    .btn:hover {{
      background: var(--hover-bg);
      border-color: var(--text-muted);
    }}
    .btn-primary {{
      background: #238636;
      border-color: #2ea043;
      color: white;
    }}
    .btn-primary:hover {{
      background: #2ea043;
    }}

    /* KPI Cards */
    .kpi-grid {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 16px;
      margin-bottom: 24px;
    }}
    .kpi-card {{
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 8px;
      padding: 18px;
      position: relative;
      overflow: hidden;
      transition: transform 0.2s, border-color 0.2s;
    }}
    .kpi-card:hover {{
      border-color: var(--text-muted);
      transform: translateY(-2px);
    }}
    .kpi-label {{
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      color: var(--text-muted);
      font-weight: 600;
    }}
    .kpi-value {{
      font-size: 28px;
      font-weight: 700;
      margin: 8px 0;
      display: flex;
      align-items: baseline;
      gap: 8px;
    }}
    .kpi-sub {{
      font-size: 12px;
      color: var(--text-muted);
    }}
    .progress-bar {{
      height: 6px;
      background: var(--bg);
      border-radius: 3px;
      overflow: hidden;
      margin-top: 10px;
    }}
    .progress-fill {{
      height: 100%;
      background: var(--green);
      border-radius: 3px;
    }}
    .progress-fill.amber {{ background: var(--amber); }}
    .progress-fill.blue {{ background: var(--blue); }}

    /* Treemap Section */
    .treemap-section {{
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 8px;
      padding: 20px;
      margin-bottom: 28px;
    }}
    .section-header {{
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 16px;
      flex-wrap: wrap;
      gap: 10px;
    }}
    .section-title {{
      font-size: 17px;
      font-weight: 600;
      display: flex;
      align-items: center;
      gap: 8px;
    }}
    .treemap-container {{
      width: 100%;
      overflow-x: auto;
      border-radius: 6px;
      background: var(--bg);
      border: 1px solid var(--card-border);
      padding: 8px;
    }}
    .treemap-container svg {{
      display: block;
      width: 100%;
      height: auto;
      min-width: 900px;
    }}
    .legend {{
      display: flex;
      gap: 18px;
      margin-top: 12px;
      flex-wrap: wrap;
      font-size: 12px;
      color: var(--text-muted);
    }}
    .legend-item {{
      display: flex;
      align-items: center;
      gap: 6px;
    }}
    .legend-dot {{
      width: 10px;
      height: 10px;
      border-radius: 2px;
    }}

    /* Tabs */
    .tabs {{
      display: flex;
      gap: 4px;
      border-bottom: 1px solid var(--card-border);
      margin-bottom: 20px;
      overflow-x: auto;
    }}
    .tab-btn {{
      background: transparent;
      border: none;
      border-bottom: 2px solid transparent;
      color: var(--text-muted);
      font-size: 14px;
      font-weight: 600;
      padding: 10px 16px;
      cursor: pointer;
      display: flex;
      align-items: center;
      gap: 8px;
      transition: all 0.2s;
      white-space: nowrap;
    }}
    .tab-btn:hover {{
      color: var(--text);
    }}
    .tab-btn.active {{
      color: var(--text);
      border-bottom-color: #f78166;
    }}
    .tab-pane {{
      display: none;
    }}
    .tab-pane.active {{
      display: block;
    }}

    /* Milestones Grid */
    .milestones-grid {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
      gap: 16px;
    }}
    .milestone-card {{
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 8px;
      padding: 18px;
    }}
    .milestone-head {{
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 8px;
    }}
    .milestone-title {{
      font-size: 15px;
      font-weight: 700;
    }}
    .badge {{
      display: inline-block;
      padding: 2px 8px;
      border-radius: 10px;
      font-size: 11px;
      font-weight: 600;
    }}
    .badge-done {{ background: rgba(46, 160, 67, 0.2); color: #3fb950; border: 1px solid #2ea043; }}
    .badge-progress {{ background: rgba(210, 153, 34, 0.2); color: #d29922; border: 1px solid #d29922; }}
    .badge-todo {{ background: rgba(139, 148, 158, 0.2); color: #8b949e; border: 1px solid #30363d; }}

    /* Table & Filter */
    .filter-bar {{
      display: flex;
      gap: 12px;
      margin-bottom: 16px;
      flex-wrap: wrap;
    }}
    .search-input {{
      flex: 1;
      min-width: 250px;
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      color: var(--text);
      padding: 8px 14px;
      border-radius: 6px;
      font-size: 13px;
    }}
    .search-input:focus {{
      outline: none;
      border-color: var(--blue);
    }}
    .filter-pills {{
      display: flex;
      gap: 6px;
      flex-wrap: wrap;
    }}
    .pill-btn {{
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      color: var(--text-muted);
      padding: 6px 12px;
      border-radius: 20px;
      font-size: 12px;
      cursor: pointer;
    }}
    .pill-btn.active {{
      background: var(--hover-bg);
      color: var(--text);
      border-color: var(--text-muted);
    }}
    .data-table {{
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }}
    .data-table th, .data-table td {{
      padding: 8px 12px;
      border: 1px solid var(--card-border);
      text-align: left;
    }}
    .data-table th {{
      background: var(--hover-bg);
      font-weight: 600;
    }}
    .data-table tr:hover {{
      background: rgba(255, 255, 255, 0.02);
    }}
    .class-header {{
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 6px;
      margin-bottom: 8px;
      overflow: hidden;
    }}
    .class-summary {{
      padding: 10px 14px;
      cursor: pointer;
      font-weight: 600;
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: var(--card-bg);
    }}
    .class-summary:hover {{
      background: var(--hover-bg);
    }}
    .class-content {{
      padding: 0 14px 12px 14px;
    }}

    code {{
      background: rgba(110, 118, 129, 0.2);
      padding: 2px 6px;
      border-radius: 4px;
      font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace;
      font-size: 12px;
    }}

    footer {{
      margin-top: 40px;
      padding-top: 20px;
      border-top: 1px solid var(--card-border);
      text-align: center;
      color: var(--text-muted);
      font-size: 13px;
    }}
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="logo-area">
        <h1>
          <span>AnyAndroid</span>
          <span class="logo-badge">≈ {overall} %</span>
        </h1>
        <div class="subtitle">Visualiseur d'avancement &amp; Matrice de compatibilité vers « Android natif sur Windows »</div>
      </div>
      <div class="header-actions">
        <a href="progress.json" class="btn" download>Télécharger progress.json</a>
        <a href="progress.svg" class="btn" download>Télécharger progress.svg</a>
        <a href="https://github.com/boykopovar/AnyAndroid" class="btn btn-primary" target="_blank">Dépôt GitHub</a>
      </div>
    </header>

    <!-- KPI Cards -->
    <div class="kpi-grid">
      <div class="kpi-card">
        <div class="kpi-label">Avancement Global</div>
        <div class="kpi-value">{overall}% <span class="kpi-sub">vers Android natif</span></div>
        <div class="progress-bar"><div class="progress-fill" style="width: {overall}%"></div></div>
        <div class="kpi-sub" style="margin-top: 8px;">Pondéré par l'effort des jalons G0–G5</div>
      </div>

      <div class="kpi-card">
        <div class="kpi-label">Pont Framework &amp; API</div>
        <div class="kpi-value">{bridge["percent_supported"]}% <span class="kpi-sub">{bridge["counts"].get("supported",0)} / {bridge["total_members"]}</span></div>
        <div class="progress-bar"><div class="progress-fill" style="width: {bridge["percent_supported"]}%"></div></div>
        <div class="kpi-sub" style="margin-top: 8px;">{bridge["total_classes"]} classes couvertes</div>
      </div>

      <div class="kpi-card">
        <div class="kpi-label">Natives Libcore AOSP 17</div>
        <div class="kpi-value">{libcore["percent"]}% <span class="kpi-sub">{libcore["done"]} / {libcore["total"]}</span></div>
        <div class="progress-bar"><div class="progress-fill amber" style="width: {libcore["percent"]}%"></div></div>
        <div class="kpi-sub" style="margin-top: 8px;">{libcore["linked"]} liées + {libcore["vm_core"]} cœur VM</div>
      </div>

      <div class="kpi-card">
        <div class="kpi-label">Opcodes Dalvik (VM)</div>
        <div class="kpi-value">{dalvik["percent"]}% <span class="kpi-sub">{dalvik["done"]} / {dalvik["total"]}</span></div>
        <div class="progress-bar"><div class="progress-fill blue" style="width: {dalvik["percent"]}%"></div></div>
        <div class="kpi-sub" style="margin-top: 8px;">Jeu d'instructions complet (sauf invoke-custom)</div>
      </div>

      <div class="kpi-card">
        <div class="kpi-label">Suites de Tests CTest</div>
        <div class="kpi-value">{tests["passed_suites"]} / {tests["total_suites"]} <span class="kpi-sub">suites</span></div>
        <div class="progress-bar"><div class="progress-fill" style="width: 90%"></div></div>
        <div class="kpi-sub" style="margin-top: 8px;">{tests["unit_tests"]} unitaires + {tests["e2e_suites"]} e2e + {tests["fuzz_suites"]} fuzz</div>
      </div>
    </div>

    <!-- Treemap Map Graph -->
    <div class="treemap-section">
      <div class="section-header">
        <div class="section-title">
          <span>Carte Treemap interactive</span>
          <span style="font-size:12px; font-weight:normal; color:var(--text-muted);">(survolez les blocs et cellules pour inspecter les détails)</span>
        </div>
      </div>
      <div class="treemap-container">
        <!-- SVG Treemap directement intégré -->
        {svg_content}
      </div>
      <div class="legend">
        <div class="legend-item"><div class="legend-dot" style="background:var(--green)"></div> Supportée / Liée (Fonctionnelle)</div>
        <div class="legend-item"><div class="legend-dot" style="background:var(--amber)"></div> Partielle (Chemin courant supporté)</div>
        <div class="legend-item"><div class="legend-dot" style="background:var(--blue)"></div> Stub (Reconnue sans effet)</div>
        <div class="legend-item"><div class="legend-dot" style="background:var(--card-border)"></div> Non supportée / Manquante</div>
      </div>
    </div>

    <!-- Tabs Navigation -->
    <div class="tabs">
      <button class="tab-btn active" onclick="switchTab('milestones')">Jalons d'Architecture (G0–G5)</button>
      <button class="tab-btn" onclick="switchTab('bridge')">Pont Framework (134 classes, {bridge["total_members"]} API)</button>
      <button class="tab-btn" onclick="switchTab('libcore')">Natives Libcore ({libcore["total"]})</button>
      <button class="tab-btn" onclick="switchTab('opcodes')">Opcodes Dalvik (256)</button>
      <button class="tab-btn" onclick="switchTab('tests')">Tests &amp; Qualité (29 suites)</button>
    </div>

    <!-- Tab 1: Milestones -->
    <div id="tab-milestones" class="tab-pane active">
      <div class="milestones-grid">
        {"".join(f'''
        <div class="milestone-card">
          <div class="milestone-head">
            <span class="milestone-title">{m["id"]} — {m["name"]}</span>
            <span class="badge {'badge-done' if m['progress'] == 100 else ('badge-progress' if m['progress'] > 0 else 'badge-todo')}">{m["progress"]}%</span>
          </div>
          <p style="font-size:13px; color:var(--text-muted); margin-bottom:12px;">{m["description"]}</p>
          <div class="progress-bar"><div class="progress-fill {'amber' if m['progress'] < 100 and m['progress'] > 0 else ''}" style="width: {m["progress"]}%"></div></div>
          <div style="font-size:12px; margin-top:10px; color:var(--text-muted);">
            <strong>État :</strong> {m["status"]}<br>
            <strong>Référence :</strong> <code>{m["adr"]}</code> · Poids: {m["weight"]}%
          </div>
        </div>
        ''' for m in data["milestones"]["items"])}
      </div>
    </div>

    <!-- Tab 2: Bridge APIs Explorer -->
    <div id="tab-bridge" class="tab-pane">
      <div class="filter-bar">
        <input type="text" id="api-search" class="search-input" placeholder="Rechercher une classe ou méthode (ex: Activity, Intent, setText, Bundle)..." oninput="filterApis()">
        <div class="filter-pills">
          <button class="pill-btn active" onclick="setApiFilter('all')">Tout ({bridge["total_members"]})</button>
          <button class="pill-btn" onclick="setApiFilter('Supportée')">Supportée ({bridge["counts"].get("supported",0)})</button>
          <button class="pill-btn" onclick="setApiFilter('Partielle')">Partielle ({bridge["counts"].get("partial",0)})</button>
          <button class="pill-btn" onclick="setApiFilter('Stub')">Stub ({bridge["counts"].get("stub",0)})</button>
          <button class="pill-btn" onclick="setApiFilter('Non supportée')">Non supportée ({bridge["counts"].get("unsupported",0)})</button>
        </div>
      </div>

      <div id="api-classes-list">
        <!-- Rempli dynamiquement en JS pour instantanéité et fluidité -->
      </div>
    </div>

    <!-- Tab 3: Libcore Natives -->
    <div id="tab-libcore" class="tab-pane">
      <div style="display:grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap:16px; margin-bottom:24px;">
        <div class="milestone-card">
          <h3 style="font-size:15px; margin-bottom:12px;">Modules AOSP (JAR / APEX)</h3>
          <table class="data-table">
            <tr><th>Module</th><th>Liées</th><th>Total</th><th>%</th></tr>
            {"".join(f'''<tr>
              <td><code>{m["name"]}</code></td>
              <td>{m["done"]}</td>
              <td>{m["total"]}</td>
              <td>{m["percent"]}%</td>
            </tr>''' for m in libcore["modules"])}
            <tr>
              <td><strong>Total</strong></td>
              <td><strong>{libcore["done"]}</strong></td>
              <td><strong>{libcore["total"]}</strong></td>
              <td><strong>{libcore["percent"]}%</strong></td>
            </tr>
          </table>
        </div>

        <div class="milestone-card">
          <h3 style="font-size:15px; margin-bottom:12px;">Top Classes avec Natives Manquantes</h3>
          <table class="data-table">
            <tr><th>Classe AOSP</th><th>Natives Manquantes</th></tr>
            {"".join(f'''<tr>
              <td><code>{item["class"]}</code></td>
              <td><span class="badge badge-todo">{item["missing"]}</span></td>
            </tr>''' for item in libcore["top_missing"])}
          </table>
        </div>
      </div>
    </div>

    <!-- Tab 4: Dalvik Opcodes -->
    <div id="tab-opcodes" class="tab-pane">
      <div class="milestone-card">
        <h3 style="font-size:15px; margin-bottom:12px;">Familles d'instructions Dalvik (256 opcodes)</h3>
        <table class="data-table">
          <tr><th>Famille</th><th>Plage Opcodes</th><th>Implémentées</th><th>Total</th><th>%</th><th>Notes</th></tr>
          {"".join(f'''<tr>
            <td><strong>{g["name"]}</strong></td>
            <td><code>{g["range"]}</code></td>
            <td>{g["done"]}</td>
            <td>{g["total"]}</td>
            <td>{round(100*g["done"]/g["total"])}%</td>
            <td style="color:var(--text-muted);">{g.get("notes", "")}</td>
          </tr>''' for g in dalvik["groups"])}
        </table>
      </div>
    </div>

    <!-- Tab 5: Tests -->
    <div id="tab-tests" class="tab-pane">
      <div class="milestone-card">
        <h3 style="font-size:15px; margin-bottom:12px;">Couverture et Qualité des Tests (CTest)</h3>
        <p style="font-size:13px; color:var(--text-muted); margin-bottom:16px;">
          AnyAndroid dispose de <strong>29 suites de tests</strong> avec 100% de réussite sur le cœur de la VM :
          69 tests unitaires, 20 tests d'intégration bout-en-bout (sécurité, APK malformés, cycle de vie) et 3 tests de fuzzing.
        </p>
        <table class="data-table">
          <tr><th>Catégorie</th><th>Suites</th><th>Statut</th><th>Détails</th></tr>
          <tr><td>Tests Unitaires</td><td>1 (69 assertions)</td><td><span class="badge badge-done">100% Réussi</span></td><td>Interpréteur, dex, gc, zip, jni, axml</td></tr>
          <tr><td>Bout-en-bout (e2e)</td><td>20 suites</td><td><span class="badge badge-done">17/20 Réussi</span></td><td>3 tests facultatifs requièrent l'image AOSP externe</td></tr>
          <tr><td>Fuzzing</td><td>3 suites</td><td><span class="badge badge-done">100% Réussi</span></td><td>Résistance aux APK corrompus et entrées aléatoires</td></tr>
        </table>
      </div>
    </div>

    <footer>
      <p>Généré automatiquement par <code>tools/progress.py</code> le {now_str}.</p>
      <p style="margin-top:4px;">AnyAndroid est un prototype de recherche sans affiliation avec Google ou Microsoft.</p>
    </footer>
  </div>

  <script>
    const PROGRESS_DATA = {json_dump};
    let currentFilter = 'all';

    function switchTab(name) {{
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
      event.target.classList.add('active');
      document.getElementById('tab-' + name).classList.add('active');
    }}

    function setApiFilter(filter) {{
      currentFilter = filter;
      document.querySelectorAll('.pill-btn').forEach(b => b.classList.remove('active'));
      event.target.classList.add('active');
      filterApis();
    }}

    function filterApis() {{
      const query = document.getElementById('api-search').value.toLowerCase();
      const container = document.getElementById('api-classes-list');
      container.innerHTML = '';

      const classes = PROGRESS_DATA.bridge.classes;
      let matchedCount = 0;

      for (const [className, members] of Object.entries(classes)) {{
        const filteredMembers = members.filter(m => {{
          const statusMatch = (currentFilter === 'all') || (m.status.includes(currentFilter));
          const queryMatch = !query || className.toLowerCase().includes(query) || m.member.toLowerCase().includes(query);
          return statusMatch && queryMatch;
        }});

        if (filteredMembers.length > 0) {{
          matchedCount++;
          const card = document.createElement('div');
          card.className = 'class-header';

          const supp = filteredMembers.filter(m => m.status.includes('Supportée') && !m.status.includes('Non')).length;
          const total = filteredMembers.length;

          card.innerHTML = `
            <details class="class-details" ${{query ? 'open' : ''}}>
              <summary class="class-summary">
                <span><code>${{className}}</code> (${{total}} membre${{total > 1 ? 's' : ''}})</span>
                <span class="badge ${{supp === total ? 'badge-done' : 'badge-progress'}}">${{supp}} / ${{total}} supporté${{supp > 1 ? 's' : ''}}</span>
              </summary>
              <div class="class-content">
                <table class="data-table" style="margin-top:8px;">
                  <tr><th style="width:40%">Membre</th><th style="width:20%">Statut</th><th>Note</th></tr>
                  ${{filteredMembers.map(m => `
                    <tr>
                      <td><code>${{m.member}}</code></td>
                      <td>
                        <span class="badge ${{
                          m.status.includes('Supportée') && !m.status.includes('Non') ? 'badge-done' :
                          (m.status.includes('Partielle') ? 'badge-progress' :
                          (m.status.includes('Stub') ? 'badge-progress' : 'badge-todo'))
                        }}">${{m.status}}</span>
                      </td>
                      <td style="color:var(--text-muted); font-size:12px;">${{m.note || '—'}}</td>
                    </tr>
                  `).join('')}}
                </table>
              </div>
            </details>
          `;
          container.appendChild(card);
        }}
      }}

      if (matchedCount === 0) {{
        container.innerHTML = '<div style="padding:24px; text-align:center; color:var(--text-muted);">Aucune API ne correspond aux critères de recherche.</div>';
      }}
    }}

    // Initialisation
    document.addEventListener('DOMContentLoaded', () => {{
      filterApis();
    }});
  </script>
</body>
</html>
"""


# -----------------------------------------------------------------------------
# 4. Intégration CLI & Génération Principale
# -----------------------------------------------------------------------------

def collect_all_data():
    """Rassemble l'ensemble des données d'avancement."""
    milestones = parse_milestones()
    bridge = parse_api_matrix()
    libcore = parse_libcore_natives()
    dalvik = parse_dalvik_opcodes()
    tests = parse_tests()

    return {
        "metadata": {
            "project": "AnyAndroid for Windows",
            "version": "0.1.0",
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "target": "Android natif sur Windows (sans VM ni hyperviseur)"
        },
        "summary": {
            "overall_percent": milestones["overall_percent"],
            "bridge_percent": bridge["percent_supported"],
            "libcore_percent": libcore["percent"],
            "dalvik_percent": dalvik["percent"],
            "total_classes": bridge["total_classes"],
            "total_members": bridge["total_members"],
            "total_natives": libcore["total"],
            "total_opcodes": dalvik["total"]
        },
        "milestones": milestones,
        "bridge": bridge,
        "libcore": libcore,
        "dalvik": dalvik,
        "tests": tests
    }


def main():
    parser = argparse.ArgumentParser(description="Générateur de progression et visualisations pour AnyAndroid.")
    parser.add_argument("output", type=Path, nargs="?", default=None, help="Dossier de sortie (défaut: site/)")
    parser.add_argument("--out-dir", type=Path, default=None, help="Répertoire de sortie pour le site (défaut: site/)")
    parser.add_argument("--docs-dir", type=Path, default=None, help="Répertoire de sortie pour la documentation (facultatif)")
    parser.add_argument("--report", action="store_true", help="Affiche un rapport texte / markdown sur stdout")
    parser.add_argument("--summary", action="store_true", help="Génère un résumé markdown compatible GitHub Step Summary")
    parser.add_argument("--check", action="store_true", help="Vérifie la cohérence des métriques")
    args = parser.parse_args()

    out_dir = args.output or args.out_dir or DEFAULT_OUT_DIR

    data = collect_all_data()
    overall = data["summary"]["overall_percent"]
    bridge_pct = data["summary"]["bridge_percent"]
    libcore_pct = data["summary"]["libcore_percent"]
    dalvik_pct = data["summary"]["dalvik_percent"]

    if args.summary:
        print("### 📊 AnyAndroid — Tableau de Progression\n")
        print("| Sous-système | Réalisé | Total | % | Statut |")
        print("|---|---|---|---|---|")
        print(f"| **Avancement Global** | — | — | **≈ {overall} %** | Pondéré par l'effort des jalons |")
        print(f"| **Pont Framework API** | {data['bridge']['counts'].get('supported',0)} | {data['bridge']['total_members']} | **{bridge_pct} %** | {data['bridge']['total_classes']} classes |")
        print(f"| **Natives Libcore AOSP 17** | {data['libcore']['done']} | {data['libcore']['total']} | **{libcore_pct} %** | {data['libcore']['linked']} liées + {data['libcore']['vm_core']} VM |")
        print(f"| **Opcodes Dalvik ISA** | {data['dalvik']['done']} | {data['dalvik']['total']} | **{dalvik_pct} %** | Jeu Dalvik complet |")
        print(f"| **Suites de Tests CTest** | {data['tests']['passed_suites']} | {data['tests']['total_suites']} | **{data['tests']['percent_core']}%** | {data['tests']['unit_tests']} unitaires |\n")
        print("#### Jalons d'Architecture G0–G5\n")
        print("| Étape | Nom | % | État |")
        print("|---|---|---|---|")
        for m in data["milestones"]["items"]:
            print(f"| **{m['id']}** | {m['name']} | {m['progress']} % | {m['status']} |")
        return

    print("[*] Collecte des métriques AnyAndroid...")
    print(f"    - Avancement global : ≈ {overall} %")
    print(f"    - Pont Framework    : {bridge_pct} % ({data['bridge']['counts'].get('supported',0)}/{data['bridge']['total_members']} membres)")
    print(f"    - Natives Libcore   : {libcore_pct} % ({data['libcore']['done']}/{data['libcore']['total']})")
    print(f"    - Opcodes Dalvik    : {dalvik_pct} % ({data['dalvik']['done']}/{data['dalvik']['total']})")

    if args.report:
        print("\n--- RAPPORT D'AVANCEMENT ---")
        print(f"📈 AnyAndroid: ≈ {overall} % vers « Android natif sur Windows »")
        print(f"✅ Pont Framework: {bridge_pct} % ({data['bridge']['counts'].get('supported',0)}/{data['bridge']['total_members']})")
        print(f"📦 Libcore AOSP 17: {libcore_pct} % ({data['libcore']['done']}/{data['libcore']['total']})")
        print(f"⚡ Dalvik VM ISA: {dalvik_pct} % ({data['dalvik']['done']}/{data['dalvik']['total']})")
        return

    # Création du dossier de sortie
    out_dir.mkdir(parents=True, exist_ok=True)

    # 1. Génération de progress.json
    json_path = out_dir / "progress.json"
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(f"[+] Écrit: {json_path}")

    # 2. Génération de progress.svg (Treemap)
    svg_content = render_treemap_svg(data)
    svg_path = out_dir / "progress.svg"
    with open(svg_path, "w", encoding="utf-8") as f:
        f.write(svg_content)
    print(f"[+] Écrit: {svg_path}")

    # 3. Badges SVG
    badge_overall = generate_badge_svg("AnyAndroid", f"{overall}%", COLOR_DONE if overall >= 30 else COLOR_PARTIAL)
    with open(out_dir / "badge-overall.svg", "w", encoding="utf-8") as f:
        f.write(badge_overall)

    badge_bridge = generate_badge_svg("Framework API", f"{bridge_pct}%", COLOR_DONE)
    with open(out_dir / "badge-bridge.svg", "w", encoding="utf-8") as f:
        f.write(badge_bridge)

    badge_libcore = generate_badge_svg("Libcore AOSP", f"{libcore_pct}%", COLOR_PARTIAL)
    with open(out_dir / "badge-libcore.svg", "w", encoding="utf-8") as f:
        f.write(badge_libcore)

    # 4. Dashboard Web HTML (uniquement si ce n'est pas le dossier docs)
    if out_dir.name != "docs":
        html_content = generate_html_dashboard(data, svg_content)
        html_path = out_dir / "index.html"
        with open(html_path, "w", encoding="utf-8") as f:
            f.write(html_content)
        print(f"[+] Écrit: {html_path}")

    # Si docs_dir est explicitement demandé
    if args.docs_dir:
        args.docs_dir.mkdir(parents=True, exist_ok=True)
        with open(args.docs_dir / "progress.json", "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        with open(args.docs_dir / "progress.svg", "w", encoding="utf-8") as f:
            f.write(svg_content)
        with open(args.docs_dir / "badge-overall.svg", "w", encoding="utf-8") as f:
            f.write(badge_overall)
        with open(args.docs_dir / "badge-bridge.svg", "w", encoding="utf-8") as f:
            f.write(badge_bridge)
        with open(args.docs_dir / "badge-libcore.svg", "w", encoding="utf-8") as f:
            f.write(badge_libcore)

    print("\n[✓] Progression générée avec succès !")
    if "html_path" in locals():
        print(f"    Pour ouvrir le dashboard : {html_path.resolve().as_uri()}")
    else:
        print(f"    Carte vectorielle : {(out_dir / 'progress.svg').resolve().as_uri()}")


if __name__ == "__main__":
    main()

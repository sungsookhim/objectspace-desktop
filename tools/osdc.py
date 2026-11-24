#!/usr/bin/env python3
"""osdc - generate C bindings from Objectspace class interface files (.osdi).

A .osdi file describes one class:

    # comment
    class OsdFolder : OsdFileSystem       # no parent means OsdoRoot
    version 1.2                         # major.minor
    include "something.h"               # copied into the header (optional)
    data {                              # this class's instance data (C)
        int sort_order;
    }
    classdata { ... }                   # data of the class object (optional)
    method osdPopulate() -> pending async
    method osdSetTitle(string title) -> bool
    classmethod osdclsQueryTitle() -> string
    override osdQueryTitle               # an inherited method
    classoverride osdclsNew              # an inherited class method
    property icon_font : text = "" page "Icon view" label "Icon font" set osdSetIconFont
    property sort_key : choice "none,name,type" = none page View index
    event open, close                   # events its objects raise
    types image/png, image/*            # data files of these types are its objects

A property (object-properties-design.md 2.1): NAME : TYPE, then any of
    = DEFAULT        its default, as text ("" if none; a choice: the option)
    "a,b,c"          (after choice) its options; after file, its kind (image)
    page "P"  label "L"
    get M  set M     methods giving / taking its value (else the state keeps it)
    show M           a method saying whether its field is shown
    store CLASS      kept in that class's state (default: the declaring class)
    options M        a method giving a choice's options now ("a,b,c")
    backing B        where Linux keeps it (mimeapps, desktop:KEY, gsettings:SCHEMA:KEY): the service's
    text index readonly nofield first last     flags (OSDO_P_*)
Types: text number checkbox choice file color icon info hidden. Values with
spaces or "#" are quoted.

Types: void bool int double string object pending. Argument strings are
borrowed (const char *); returned strings and objects belong to the caller.

For each class, osdc writes NAME.h (data struct, call wrappers, parent-call
wrappers for overrides, prototypes of the implementations the class author
writes as NAME__method) and NAME.reg.c (apply stubs, method tables and
NAME_register()).

usage: osdc.py -I DIR [-I DIR ...] -o OUTDIR FILE.osdi [...]
Parent classes are looked up as PARENT.osdi in the -I directories.
"""
import argparse
import re
import sys
from pathlib import Path

TYPES = {  # name: (letter, C argument type, C return type)
    "void": ("v", None, "void"),
    "bool": ("b", "bool", "bool"),
    "int": ("i", "int64_t", "int64_t"),
    "double": ("d", "double", "double"),
    "string": ("s", "const char *", "char *"),
    "object": ("o", "osdo_object *", "osdo_object *"),
    "pending": ("p", None, "osdo_pending *"),
}
UNION = {"b": "b", "i": "i", "d": "d", "s": "s", "o": "o"}
TO_VALUE = {"v": None, "b": "osdo_v_bool", "i": "osdo_v_int", "d": "osdo_v_double",
            "s": "osdo_v_string_take", "o": "osdo_v_object_take", "p": "osdo_v_pending_take"}


class OsdiError(Exception):
    pass


def param(ctype, name):
    """'const char *name' or 'int64_t name'."""
    return f"{ctype}{name}" if ctype.endswith("*") else f"{ctype} {name}"


class Method:
    def __init__(self, name, args, ret, is_async=False):
        self.name, self.args, self.ret, self.is_async = name, args, ret, is_async

    @property
    def sig(self):
        return TYPES[self.ret][0] + "(" + "".join(TYPES[t][0] for t, _ in self.args) + ")"

    def c_ret(self):
        return TYPES[self.ret][2]

    def c_decl(self, fname):
        """'char *fname' or 'bool fname'."""
        r = self.c_ret()
        return f"{r}{fname}" if r.endswith("*") else f"{r} {fname}"

    def c_params(self, self_type="osdo_object *"):
        return ", ".join([param(self_type, "self")] + [param(TYPES[t][1], n) for t, n in self.args])

    def c_fnptr(self):
        return f"{self.c_ret()} (*)(" + ", ".join(["osdo_object *"] + [TYPES[t][1] for t, _ in self.args]) + ")"

    def arg_names(self):
        return ", ".join(["self"] + [n for _, n in self.args])


class Cls:
    def __init__(self, name):
        self.name = name
        self.parent = "OsdoRoot"
        self.major = self.minor = None
        self.includes, self.data, self.cdata = [], [], []
        self.methods, self.cmethods = [], []
        self.overrides, self.coverrides = [], []
        self.props, self.events, self.types = [], [], []
        self.builtin = False


def builtin_classes():
    root = Cls("OsdoRoot")
    root.parent, root.major, root.minor, root.builtin = None, 1, 0, True
    root.methods = [Method("osdInitData", [], "void"), Method("osdUnInitData", [], "void")]
    root.cmethods = [Method("osdclsNew", [], "object")]   # introduced by the metaclass OsdoClass
    return {"OsdoRoot": root}


METHOD_RE = re.compile(r"^(method|classmethod)\s+(\w+)\s*\(([^)]*)\)\s*->\s*(\w+)\s*(async)?\s*$")


PROP_TYPES = {"text", "number", "checkbox", "choice", "file", "color", "icon", "info", "hidden"}
PROP_FLAGS = {"text": "OSDO_P_TEXT", "index": "OSDO_P_INDEX", "readonly": "OSDO_P_READONLY", "nofield": "OSDO_P_NOFIELD",
              "first": "OSDO_P_FIRST", "last": "OSDO_P_LAST"}
PROP_KEYS = {"page", "label", "get", "set", "show", "store", "options", "backing"}


def parse_property(raw, where):
    import shlex
    try:
        t = shlex.split(raw, comments=True)
    except ValueError as e:
        raise OsdiError(f"{where}: {e}")
    if len(t) < 4 or t[2] != ":" or t[3] not in PROP_TYPES:
        raise OsdiError(f"{where}: property NAME : TYPE ...")
    p = {"name": t[1], "type": t[3], "def": "", "choices": None, "flags": []}
    i = 4
    if p["type"] in ("choice", "file") and i < len(t) and t[i] not in PROP_KEYS | set(PROP_FLAGS) | {"="}:
        p["choices"] = t[i]
        i += 1
    while i < len(t):
        k = t[i]
        if k == "=" and i + 1 < len(t):
            p["def"] = t[i + 1]
            i += 2
        elif k in PROP_KEYS and i + 1 < len(t):
            p[k] = t[i + 1]
            i += 2
        elif k in PROP_FLAGS:
            p["flags"].append(PROP_FLAGS[k])
            i += 1
        else:
            raise OsdiError(f"{where}: property {p['name']}: cannot parse '{k}'")
    if p["type"] == "choice" and not p["choices"] and not p.get("options") and not p.get("backing"):
        raise OsdiError(f"{where}: property {p['name']}: a choice needs its options")
    if not re.match(r"^[A-Za-z_][\w.:-]*$", p["name"]):
        raise OsdiError(f"{where}: bad property name {p['name']}")
    return p


def parse(path):
    text = Path(path).read_text()
    c, block, lines = None, None, text.splitlines()
    for no, raw in enumerate(lines, 1):
        if block is None and raw.strip().startswith("property "):
            if c is None:
                raise OsdiError(f"{path}:{no}: expected 'class'")
            c.props.append(parse_property(raw.strip(), f"{path}:{no}"))
            continue
        line = raw.split("#", 1)[0].rstrip() if block is None else raw.rstrip()
        s = line.strip()
        if block is not None:
            if s == "}":
                block = None
            else:
                getattr(c, block).append(raw.rstrip())
            continue
        if not s:
            continue
        where = f"{path}:{no}"
        if s.startswith("class "):
            m = re.match(r"^class\s+(\w+)(?:\s*:\s*(\w+))?$", s)
            if not m:
                raise OsdiError(f"{where}: bad class line")
            c = Cls(m.group(1))
            if m.group(2):
                c.parent = m.group(2)
            continue
        if c is None:
            raise OsdiError(f"{where}: expected 'class'")
        if s.startswith("version "):
            m = re.match(r"^version\s+(\d+)\.(\d+)$", s)
            if not m:
                raise OsdiError(f"{where}: version must be MAJOR.MINOR")
            c.major, c.minor = int(m.group(1)), int(m.group(2))
        elif s.startswith("include "):
            c.includes.append(s[len("include "):].strip())
        elif s in ("data {", "classdata {"):
            block = "data" if s.startswith("data") else "cdata"
        elif s.startswith(("method ", "classmethod ")):
            m = METHOD_RE.match(s)
            if not m:
                raise OsdiError(f"{where}: bad method line")
            args = []
            for a in filter(None, (x.strip() for x in m.group(3).split(","))):
                parts = a.split()
                if len(parts) != 2 or parts[0] not in TYPES or parts[0] in ("void", "pending"):
                    raise OsdiError(f"{where}: bad argument '{a}'")
                args.append((parts[0], parts[1]))
            if m.group(4) not in TYPES:
                raise OsdiError(f"{where}: unknown type {m.group(4)}")
            meth = Method(m.group(2), args, m.group(4), bool(m.group(5)))
            if meth.is_async and meth.ret != "pending":
                raise OsdiError(f"{where}: async methods return pending")
            (c.methods if m.group(1) == "method" else c.cmethods).append(meth)
        elif s.startswith("override "):
            c.overrides.append(s.split()[1])
        elif s.startswith("classoverride "):
            c.coverrides.append(s.split()[1])
        elif s.startswith("event "):
            c.events += [e.strip() for e in s[len("event "):].split(",") if e.strip()]
        elif s.startswith("types "):
            c.types += [e.strip() for e in s[len("types "):].split(",") if e.strip()]
        else:
            raise OsdiError(f"{where}: cannot parse '{s}'")
    if c is None or c.major is None:
        raise OsdiError(f"{path}: needs a class line and a version line")
    return c


class World:
    def __init__(self, incdirs):
        self.incdirs = [Path(d) for d in incdirs]
        self.classes = builtin_classes()

    def get(self, name):
        if name not in self.classes:
            for d in self.incdirs:
                p = d / f"{name}.osdi"
                if p.exists():
                    self.classes[name] = parse(p)
                    break
            else:
                raise OsdiError(f"class {name}: no {name}.osdi in the include path")
        return self.classes[name]

    def chain(self, c):
        out = []
        while c:
            out.append(c)
            c = self.get(c.parent) if c.parent else None
        return out

    def find(self, c, name, kind):
        """(introducing class, index, Method) for an inherited method."""
        for anc in (self.chain(self.get(c.parent)) if c.parent else []):
            lst = anc.methods if kind == "method" else anc.cmethods
            for i, m in enumerate(lst):
                if m.name == name:
                    return anc, i, m
        raise OsdiError(f"{c.name}: no inherited {kind} {name} to override")


def intro_ptr(cls, meta):
    if cls.builtin:
        return "OsdoClassClass" if meta else "OsdoRootClass"
    return f"M_{cls.name}Class" if meta else f"{cls.name}Class"


def intro_index(cls, i, m, meta):
    if cls.builtin:
        return ("OsdoClass_" if meta else "OsdoRoot_") + m.name
    return str(i)


def gen_header(w, c):
    n = c.name
    o = [f"/* Generated by osdc from {n}.osdi. Do not edit. */",
         f"#ifndef {n.upper()}_OSDI_H", f"#define {n.upper()}_OSDI_H", "", '#include "osdo.h"']
    if c.parent != "OsdoRoot":
        o.append(f'#include "{c.parent}.h"')
    o += [f"#include {inc}" for inc in c.includes]
    o += ["", f"#define {n}_MAJOR {c.major}", f"#define {n}_MINOR {c.minor}", "",
          f"extern osdo_class *{n}Class;", f"extern osdo_class *M_{n}Class;", ""]
    if c.data:
        o += ["typedef struct {"] + c.data + [f"}} {n}Data;", "",
              f"static inline {n}Data *{n}_data(osdo_object *self)",
              f"{{ return ({n}Data *)osdo_data(self, {n}Class); }}", ""]
    if c.cdata:
        o += ["typedef struct {"] + c.cdata + [f"}} M_{n}Data;", "",
              f"static inline M_{n}Data *{n}_cdata(osdo_class *cls)",
              f"{{ return (M_{n}Data *)osdo_data(&cls->hdr, M_{n}Class); }}", ""]
    if c.methods:
        o.append("/* Calls (dispatch through the object's class). */")
    for i, m in enumerate(c.methods):
        ret = "" if m.ret == "void" else "return "
        o += [f"static inline {m.c_decl(f'{n}_{m.name}')}({m.c_params()})",
              f"{{ {ret}(({m.c_fnptr()})osdo_slot(self, {n}Class, {i}))({m.arg_names()}); }}"]
    if c.cmethods:
        o.append("/* Class method calls: pass the class (or a subclass). */")
    for i, m in enumerate(c.cmethods):
        ret = "" if m.ret == "void" else "return "
        params = ", ".join(["osdo_class *cls"] + [param(TYPES[t][1], a) for t, a in m.args])
        args = ", ".join(["&cls->hdr"] + [a for _, a in m.args])
        o += [f"static inline {m.c_decl(f'{n}_{m.name}')}({params})",
              f"{{ {ret}(({m.c_fnptr()})osdo_slot(&cls->hdr, M_{n}Class, {i}))({args}); }}"]
    o += ["", "/* Implemented by the class author. For class methods, self is the class. */"]
    for m in c.methods + c.cmethods:
        o.append(f"{m.c_decl(f'{n}__{m.name}')}({m.c_params()});")
    for kind, names in (("method", c.overrides), ("classmethod", c.coverrides)):
        for name in names:
            _, _, m = w.find(c, name, kind)
            o.append(f"{m.c_decl(f'{n}__{name}')}({m.c_params()});")
    if c.overrides or c.coverrides:
        o += ["", "/* The parent's version of each overridden method. */"]
    for kind, names in (("method", c.overrides), ("classmethod", c.coverrides)):
        meta = kind == "classmethod"
        for name in names:
            anc, i, m = w.find(c, name, kind)
            ret = "" if m.ret == "void" else "return "
            defining = f"M_{n}Class" if meta else f"{n}Class"
            o += [f"static inline {m.c_decl(f'{n}_parent_{name}')}({m.c_params()})",
                  f"{{ {ret}(({m.c_fnptr()})osdo_parent_slot({defining}, {intro_ptr(anc, meta)}, "
                  f"{intro_index(anc, i, m, meta)}))({m.arg_names()}); }}"]
    o += ["", f"osdo_status {n}_register(void);", "", "#endif", ""]
    return "\n".join(o)


def apply_stub(n, m):
    args = ", ".join(["self"] + [f"a[{k}].u.{UNION[TYPES[t][0]]}" for k, (t, _) in enumerate(m.args)])
    call = f"(({m.c_fnptr()})fn)({args})"
    conv = TO_VALUE[TYPES[m.ret][0]]
    body = f"    {call};\n    *ret = osdo_v_void();" if conv is None else f"    *ret = {conv}({call});"
    return (f"static osdo_status {n}_apply_{m.name}(osdo_fn fn, osdo_object *self, const osdo_value *a, osdo_value *ret)\n"
            f"{{\n    (void)a;\n{body}\n    return OSDO_OK;\n}}\n")


def gen_reg(w, c):
    n = c.name
    o = [f"/* Generated by osdc from {n}.osdi. Do not edit. */", f'#include "{n}.h"', "",
         f"osdo_class *{n}Class;", f"osdo_class *M_{n}Class;", ""]
    for m in c.methods + c.cmethods:
        o.append(apply_stub(n, m))

    def table(kind, ms):
        if not ms:
            return "NULL", "0"
        rows = [f'    {{ "{m.name}", "{m.sig}", {"OSDO_M_ASYNC" if m.is_async else "0"}, '
                f"{n}_apply_{m.name}, (osdo_fn){n}__{m.name} }}," for m in ms]
        o.extend([f"static const osdo_method_def {n}_{kind}[] = {{"] + rows + ["};", ""])
        return f"{n}_{kind}", str(len(ms))

    def otable(kind, names):
        if not names:
            return "NULL", "0"
        rows = [f'    {{ "{x}", (osdo_fn){n}__{x} }},' for x in names]
        o.extend([f"static const osdo_override_def {n}_{kind}[] = {{"] + rows + ["};", ""])
        return f"{n}_{kind}", str(len(names))

    def cstr(v):
        if v is None:
            return "NULL"
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'

    pt, pn = "NULL", "0"
    if c.props:
        rows = []
        for p in c.props:
            fl = " | ".join(p["flags"]) or "0"
            rows.append("    { %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s }," % (
                cstr(p["name"]), cstr(p["type"]), cstr(p["def"]), cstr(p.get("page")), cstr(p.get("label")),
                cstr(p["choices"]), cstr(p.get("get")), cstr(p.get("set")), cstr(p.get("show")), cstr(p.get("store")),
                cstr(p.get("options")), cstr(p.get("backing")), fl))
        o.extend([f"static const osdo_property_def {n}_props[] = {{"] + rows + ["};", ""])
        pt, pn = f"{n}_props", str(len(c.props))
    et, en = "NULL", "0"
    if c.events:
        o.extend([f"static const char *const {n}_events[] = {{ " + ", ".join(cstr(e) for e in c.events) + " };", ""])
        et, en = f"{n}_events", str(len(c.events))

    tt, tn = "NULL", "0"
    if c.types:
        o.extend([f"static const char *const {n}_types[] = {{ " + ", ".join(cstr(e) for e in c.types) + " };", ""])
        tt, tn = f"{n}_types", str(len(c.types))

    mt, mn = table("methods", c.methods)
    cmt, cmn = table("cmethods", c.cmethods)
    ot, on = otable("overrides", c.overrides)
    cot, con = otable("coverrides", c.coverrides)
    p = w.get(c.parent)
    pmaj = f"{p.name}_MAJOR" if not p.builtin else "1"
    pmin = f"{p.name}_MINOR" if not p.builtin else "0"
    o += [f"osdo_status {n}_register(void)", "{",
          "    static const osdo_class_def def = {",
          f'        .name = "{n}", .parent = "{c.parent}",',
          f"        .major = {n}_MAJOR, .minor = {n}_MINOR,",
          f"        .parent_major = {pmaj}, .parent_minor = {pmin},",
          f"        .data_size = {f'sizeof({n}Data)' if c.data else '0'},",
          f"        .methods = {mt}, .nmethods = {mn},",
          f"        .overrides = {ot}, .noverrides = {on},",
          f"        .cmethods = {cmt}, .ncmethods = {cmn},",
          f"        .coverrides = {cot}, .ncoverrides = {con},",
          f"        .cdata_size = {f'sizeof(M_{n}Data)' if c.cdata else '0'},",
          f"        .props = {pt}, .nprops = {pn},",
          f"        .events = {et}, .nevents = {en},",
          f"        .types = {tt}, .ntypes = {tn},",
          "    };",
          f"    osdo_status st = osdo_register_class(&def, &{n}Class);",
          f"    if (st == OSDO_OK) M_{n}Class = {n}Class->hdr.cls;",
          "    return st;", "}", ""]
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser(description="generate C bindings from .osdi class files")
    ap.add_argument("-I", dest="inc", action="append", default=[])
    ap.add_argument("-o", dest="out", required=True)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        for f in a.files:
            w = World(a.inc + [str(Path(f).parent)])
            c = parse(f)
            w.classes[c.name] = c
            w.chain(c)   # resolve parents now, for clear errors
            (out / f"{c.name}.h").write_text(gen_header(w, c))
            (out / f"{c.name}.reg.c").write_text(gen_reg(w, c))
    except OsdiError as e:
        sys.exit(f"osdc: {e}")


if __name__ == "__main__":
    main()

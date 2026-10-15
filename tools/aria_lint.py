#!/usr/bin/env python3
"""
aria_lint.py - lightweight static checker for Enforce Script mods.

It cannot replace the Workbench compiler, but it catches the mistakes that
matter most when writing a mod without the game installed:

  * unknown classes / types / enums / enum values
  * calls to methods or fields that do not exist on the receiver's class
    (inheritance aware, using the vanilla script dump as the source of truth)
  * wrong argument counts
  * unbalanced braces / parentheses, ternary operators (unsupported)

Usage:
  python3 tools/aria_lint.py <vanilla_scripts_dir> <mod_scripts_dir>

The vanilla scripts come from Bohemia's public repository
https://github.com/BohemiaInteractive/Arma-Reforger-Script-Diff (folder "scripts").
"""
import os
import re
import sys
from collections import defaultdict

KEYWORDS = {
    'if', 'else', 'while', 'for', 'foreach', 'switch', 'case', 'default', 'break', 'continue',
    'return', 'new', 'delete', 'class', 'enum', 'typedef', 'modded', 'sealed', 'static', 'const',
    'ref', 'out', 'inout', 'notnull', 'protected', 'private', 'override', 'proto', 'native',
    'external', 'event', 'owned', 'volatile', 'autoptr', 'local', 'reference', 'thread', 'null',
    'true', 'false', 'this', 'super', 'auto', 'typename', 'vanilla', 'extends', 'func',
}
MODIFIERS = {
    'proto', 'native', 'external', 'static', 'protected', 'private', 'override', 'event',
    'sealed', 'owned', 'volatile', 'const', 'ref', 'notnull', 'autoptr', 'reference', 'local',
    'out', 'inout', 'vanilla'
}
PRIMITIVES = {'int', 'float', 'bool', 'string', 'vector', 'void', 'auto', 'typename', 'func', 'Class', 'Managed'}

TOKEN_RE = re.compile(r'''
    (?P<ws>\s+) |
    (?P<str>"(?:\\.|[^"\\])*") |
    (?P<chr>'(?:\\.|[^'\\])*') |
    (?P<num>0x[0-9a-fA-F]+|\d+\.\d*f?|\.\d+f?|\d+f?) |
    (?P<id>[A-Za-z_]\w*) |
    (?P<op>::|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|\+=|-=|\*=|/=|&=|\|=|\^=|[{}()\[\];,.<>=+\-*/%!&|^~?:#@\\])
''', re.X)


def strip_comments(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            if j < 0:
                break
            i = j
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            seg = src[i:(j + 2 if j >= 0 else n)]
            out.append('\n' * seg.count('\n'))
            i = j + 2 if j >= 0 else n
            continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == '\\':
                    j += 1
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def strip_preprocessor(src):
    # Keep code from both branches of #ifdef blocks, drop directive lines
    return '\n'.join('' if l.lstrip().startswith('#') else l for l in src.split('\n'))


class Tok:
    __slots__ = ('kind', 'val', 'line')

    def __init__(self, kind, val, line):
        self.kind, self.val, self.line = kind, val, line

    def __repr__(self):
        return f'{self.val}'


def tokenize(src):
    toks = []
    line = 1
    pos = 0
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m:
            pos += 1
            continue
        kind = m.lastgroup
        val = m.group(kind)
        if kind != 'ws':
            toks.append(Tok(kind, val, line))
        line += val.count('\n')
        pos = m.end()
    return toks


def match_close(toks, i, open_='{', close='}'):
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].val == open_:
            depth += 1
        elif toks[j].val == close:
            depth -= 1
            if depth == 0:
                return j
    return len(toks) - 1


def skip_template(toks, i):
    """toks[i] == '<' : return index after matching '>' (handles >>)."""
    depth = 0
    j = i
    while j < len(toks):
        v = toks[j].val
        if v == '<':
            depth += 1
        elif v == '>':
            depth -= 1
            if depth == 0:
                return j + 1
        elif v == '>>':
            depth -= 2
            if depth <= 0:
                return j + 1
        elif v in (';', '{', '}', '(', ')'):
            return i + 1
        j += 1
    return j


class TypeRef:
    def __init__(self, name, args=None, is_array=False):
        self.name = name
        self.args = args or []
        self.is_array = is_array  # static array like vector mat[4]

    def __repr__(self):
        s = self.name
        if self.args:
            s += '<' + ','.join(map(repr, self.args)) + '>'
        if self.is_array:
            s += '[]'
        return s


def parse_type(toks, i):
    """Parse a type starting at i (after modifiers). Returns (TypeRef, next_index) or (None, i)."""
    if i >= len(toks) or toks[i].kind != 'id':
        return None, i
    name = toks[i].val
    j = i + 1
    args = []
    if j < len(toks) and toks[j].val == '<':
        end = skip_template(toks, j)
        # parse inner types separated by commas at depth 1
        k = j + 1
        while k < end - 1:
            while k < end - 1 and toks[k].val in MODIFIERS:
                k += 1
            t, k2 = parse_type(toks, k)
            if t:
                args.append(t)
                k = k2
            while k < end - 1 and toks[k].val not in (',',):
                k += 1
            k += 1
        j = end
    return TypeRef(name, args), j


class Method:
    def __init__(self, name, ret, params, static, owner, line, file):
        self.name, self.ret, self.params, self.static = name, ret, params, static
        self.owner, self.line, self.file = owner, line, file
        self.min_args = sum(1 for p in params if not p[2])
        self.max_args = len(params)
        self.varargs = any(p[1] == '...' for p in params)


class ClassInfo:
    def __init__(self, name, parent, file, line, template_params=None):
        self.name, self.parent, self.file, self.line = name, parent, file, line
        self.methods = defaultdict(list)
        self.fields = {}
        self.template_params = template_params or []


class EnumInfo:
    def __init__(self, name, parent):
        self.name, self.parent = name, parent
        self.values = set()


class Model:
    def __init__(self):
        self.classes = {}
        self.enums = {}
        self.typedefs = {}
        self.functions = defaultdict(list)  # global functions
        self.globals = {}

    # ------------------------------------------------------------------
    def parse_file(self, path, collect_bodies=None):
        try:
            src = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            return
        src = strip_preprocessor(strip_comments(src))
        toks = tokenize(src)
        i = 0
        n = len(toks)
        while i < n:
            t = toks[i]
            if t.val == 'typedef':
                j = i + 1
                ty, j = parse_type(toks, j)
                if j < n and toks[j].kind == 'id':
                    self.typedefs[toks[j].val] = ty
                    i = j + 1
                    continue
                # function typedef: typedef func Name;
                i += 1
                continue
            if t.val == 'enum' and i + 1 < n and toks[i + 1].kind == 'id':
                name = toks[i + 1].val
                j = i + 2
                parent = None
                if toks[j].val == ':':
                    parent = toks[j + 1].val
                    j += 2
                if toks[j].val == '{':
                    end = match_close(toks, j)
                    e = self.enums.get(name) or EnumInfo(name, parent)
                    expect_name = True
                    depth = 0
                    for k in range(j + 1, end):
                        v = toks[k].val
                        if v in '([':
                            depth += 1
                        elif v in ')]':
                            depth -= 1
                        if depth == 0 and expect_name and toks[k].kind == 'id':
                            e.values.add(v)
                            expect_name = False
                        elif depth == 0 and v == ',':
                            expect_name = True
                    self.enums[name] = e
                    i = end + 1
                    continue
            if t.val == 'class':
                # class Name<Class T> : Parent {
                j = i + 1
                if j >= n or toks[j].kind != 'id':
                    i += 1
                    continue
                name = toks[j].val
                j += 1
                tparams = []
                if toks[j].val == '<':
                    end_t = skip_template(toks, j)
                    for k in range(j + 1, end_t - 1):
                        if toks[k].kind == 'id' and toks[k - 1].kind == 'id' and toks[k - 1].val in ('Class', 'typename'):
                            tparams.append(toks[k].val)
                    j = end_t
                parent = None
                if toks[j].val in (':', 'extends'):
                    parent = toks[j + 1].val
                    j += 2
                    if j < n and toks[j].val == '<':
                        j = skip_template(toks, j)
                if j < n and toks[j].val == '{':
                    end = match_close(toks, j)
                    modded = i > 0 and toks[i - 1].val == 'modded'
                    ci = self.classes.get(name)
                    if ci is None:
                        ci = ClassInfo(name, parent, path, t.line, tparams)
                        self.classes[name] = ci
                    elif not modded and parent and not ci.parent:
                        ci.parent = parent
                    self.parse_class_body(ci, toks, j + 1, end, path)
                    if collect_bodies is not None:
                        collect_bodies.append((ci, toks, j + 1, end, path))
                    i = end + 1
                    continue
                i = j
                continue
            # global function declaration at depth 0
            m = self.try_parse_member(toks, i, n, path, None)
            if m:
                kind, obj, nxt = m
                if kind == 'method':
                    self.functions[obj.name].append(obj)
                elif kind == 'field':
                    for fname, fty in obj:
                        self.globals[fname] = fty
                i = nxt
                continue
            i += 1

    # ------------------------------------------------------------------
    def try_parse_member(self, toks, i, end, path, owner):
        """Parse a field or method declaration at toks[i]. Returns (kind, obj, next) or None."""
        n = end
        j = i
        # attributes [..]
        while j < n and toks[j].val == '[':
            j = match_close(toks, j, '[', ']') + 1
        if j >= n:
            return None
        start_line = toks[j].line
        is_static = False
        is_hidden = False
        while j < n and toks[j].val in MODIFIERS:
            if toks[j].val == 'static':
                is_static = True
            if toks[j].val in ('protected', 'private'):
                is_hidden = True
            j += 1
        if j >= n or toks[j].kind != 'id' or toks[j].val in KEYWORDS - {'auto', 'typename', 'func'}:
            return None
        # constructor / destructor: Name(  or ~Name(
        if owner and toks[j].val == owner.name and j + 1 < n and toks[j + 1].val == '(':
            ret = TypeRef(owner.name)
            name_idx = j
        else:
            ty, k = parse_type(toks, j)
            if not ty:
                return None
            # array return type  e.g. "vector[4]" rare; skip
            if k < n and toks[k].val == '~':
                k += 1
            if k >= n or toks[k].kind != 'id':
                return None
            ret = ty
            name_idx = k
        name = toks[name_idx].val
        k = name_idx + 1
        if k < n and toks[k].val == '(':
            close = match_close(toks, k, '(', ')')
            params = self.parse_params(toks, k + 1, close)
            after = close + 1
            # skip trailing modifiers like 'const'
            while after < n and toks[after].val in ('const',):
                after += 1
            meth = Method(name, ret, params, is_static, owner.name if owner else None, start_line, path)
            meth.hidden = is_hidden
            if after < n and toks[after].val == '{':
                body_end = match_close(toks, after)
                meth.body = (after, body_end)
                return 'method', meth, body_end + 1
            if after < n and toks[after].val == ';':
                meth.body = None
                return 'method', meth, after + 1
            return None
        # field(s): Type a = x, b;   or  Type a[4];
        if k >= n or toks[k].val not in ('=', ';', ',', '['):
            return None
        fields = []
        while k <= n:
            is_arr = False
            if k < n and toks[k].val == '[':
                k = match_close(toks, k, '[', ']') + 1
                is_arr = True
            fields.append((name, TypeRef(ret.name, ret.args, is_arr)))
            # skip initializer
            depth = 0
            if k < n and toks[k].val not in ('=', ';', ','):
                return None
            while k < n:
                v = toks[k].val
                if v in ('class', 'enum', 'typedef', 'modded'):
                    return None
                if v in '([{':
                    depth += 1
                elif v in ')]}':
                    depth -= 1
                elif depth == 0 and v in (',', ';'):
                    break
                k += 1
            if k < n and toks[k].val == ',' and k + 1 < n and toks[k + 1].kind == 'id':
                name = toks[k + 1].val
                k += 2
                continue
            break
        if k < n and toks[k].val == ';':
            return 'field', fields, k + 1
        return None

    def parse_params(self, toks, s, e):
        params = []
        if s >= e:
            return params
        if e - s == 1 and toks[s].val == 'void':
            return params
        depth = 0
        cur = []
        groups = []
        for k in range(s, e):
            v = toks[k].val
            if v in '([{<':
                depth += 1
            elif v in ')]}>':
                depth -= 1
            elif v == '>>':
                depth -= 2
            if v == ',' and depth == 0:
                groups.append(cur)
                cur = []
            else:
                cur.append(toks[k])
        if cur:
            groups.append(cur)
        for g in groups:
            if not g:
                continue
            if g[0].val == '...' or (len(g) >= 1 and g[0].val == '.'):
                params.append(('...', '...', True))
                continue
            has_default = any(t.val == '=' for t in g)
            gi = 0
            while gi < len(g) and g[gi].val in MODIFIERS:
                gi += 1
            ty, gj = parse_type(g, gi)
            pname = g[gj].val if gj < len(g) and g[gj].kind == 'id' else '?'
            is_arr = gj + 1 < len(g) and g[gj + 1].val == '['
            if ty:
                ty.is_array = is_arr
            params.append((pname, ty, has_default))
        return params

    def parse_class_body(self, ci, toks, s, e, path):
        i = s
        while i < e:
            if toks[i].val == ';':
                i += 1
                continue
            m = self.try_parse_member(toks, i, e, path, ci)
            if m:
                kind, obj, nxt = m
                if kind == 'method':
                    ci.methods[obj.name].append(obj)
                else:
                    for fname, fty in obj:
                        ci.fields[fname] = fty
                i = nxt
            else:
                # skip to next ';' or block at this depth
                v = toks[i].val
                if v == '{':
                    i = match_close(toks, i) + 1
                else:
                    i += 1

    # ------------------------------------------------------------------
    def resolve_typedef(self, ty):
        seen = 0
        while ty and ty.name in self.typedefs and seen < 10:
            base = self.typedefs[ty.name]
            ty = TypeRef(base.name, base.args, ty.is_array) if base else ty
            seen += 1
        return ty

    def hierarchy(self, name):
        out = []
        seen = set()
        while name and name in self.classes and name not in seen:
            seen.add(name)
            out.append(self.classes[name])
            name = self.classes[name].parent
        # everything ultimately derives from Class/Managed
        for base in ('Managed', 'Class'):
            if base in self.classes and base not in seen:
                out.append(self.classes[base])
                seen.add(base)
        return out

    def find_methods(self, cls, mname):
        res = []
        for c in self.hierarchy(cls):
            res.extend(c.methods.get(mname, []))
        return res

    def find_field(self, cls, fname):
        for c in self.hierarchy(cls):
            if fname in c.fields:
                return c.fields[fname], c
        return None, None

    def type_exists(self, name):
        return (name in PRIMITIVES or name in self.classes or name in self.enums or
                name in self.typedefs)

    def enum_values(self, name):
        vals = set()
        seen = set()
        while name and name in self.enums and name not in seen:
            seen.add(name)
            vals |= self.enums[name].values
            name = self.enums[name].parent
        return vals


# ======================================================================
class Checker:
    def __init__(self, model, mod_files):
        self.m = model
        self.mod_files = mod_files
        self.issues = []

    def report(self, path, line, msg):
        self.issues.append((path, line, msg))

    def subst(self, ty, owner_ty):
        """Replace template params (T, TKey, TValue) using owner type args."""
        if not ty or not owner_ty:
            return ty
        ci = self.m.classes.get(owner_ty.name)
        if ci and ci.template_params and owner_ty.args:
            mapping = dict(zip(ci.template_params, owner_ty.args))
            if ty.name in mapping:
                return mapping[ty.name]
        return ty

    # ------------------------------------------------------------------
    def check_all(self):
        for path in self.mod_files:
            src_raw = open(path, encoding='utf-8', errors='replace').read()
            self.check_balance(path, src_raw)
            src = strip_preprocessor(strip_comments(src_raw))
            toks = tokenize(src)
            for t in toks:
                if t.val == '?':
                    self.report(path, t.line, "ternary operator '?' is not supported in Enforce Script")
            self.check_file(path, toks)

    def check_balance(self, path, src):
        src = strip_comments(src)
        src = re.sub(r'"(?:\\.|[^"\\])*"', '""', src)
        for o, c in (('{', '}'), ('(', ')'), ('[', ']')):
            if src.count(o) != src.count(c):
                self.report(path, 0, f'unbalanced {o}{c}: {src.count(o)} vs {src.count(c)}')

    def check_file(self, path, toks):
        n = len(toks)
        i = 0
        while i < n:
            t = toks[i]
            if t.val == 'class' and i + 1 < n and toks[i + 1].kind == 'id':
                name = toks[i + 1].val
                j = i + 2
                while j < n and toks[j].val != '{':
                    j += 1
                end = match_close(toks, j)
                ci = self.m.classes.get(name)
                if ci and ci.parent and not self.m.type_exists(ci.parent):
                    self.report(path, t.line, f'class {name}: unknown parent {ci.parent}')
                self.check_class(path, ci, toks, j + 1, end)
                i = end + 1
                continue
            i += 1

    def check_class(self, path, ci, toks, s, e):
        i = s
        while i < e:
            if toks[i].val == ';':
                i += 1
                continue
            m = self.m.try_parse_member(toks, i, e, path, ci)
            if not m:
                v = toks[i].val
                i = (match_close(toks, i) + 1) if v == '{' else i + 1
                continue
            kind, obj, nxt = m
            if kind == 'field':
                for fname, fty in obj:
                    self.check_type(path, toks[i].line, fty)
            else:
                self.check_type(path, obj.line, obj.ret)
                for pname, pty, _ in obj.params:
                    if pty != '...':
                        self.check_type(path, obj.line, pty)
                self.check_override(path, ci, obj, toks, i)
                if obj.body:
                    self.check_body(path, ci, obj, toks, obj.body[0], obj.body[1])
            i = nxt

    def check_override(self, path, ci, meth, toks, i):
        j = i
        is_override = False
        while j < len(toks) and toks[j].val not in ('(',):
            if toks[j].val == 'override':
                is_override = True
            j += 1
        if not is_override:
            return
        parent = ci.parent
        found = []
        for c in self.m.hierarchy(parent):
            found.extend(c.methods.get(meth.name, []))
        # modded classes: base is the class itself in vanilla
        if not found and ci.name in self.m.classes:
            for c in self.m.hierarchy(ci.name):
                for mm in c.methods.get(meth.name, []):
                    if mm.file != path:
                        found.append(mm)
        if not found:
            self.report(path, meth.line, f'override {ci.name}.{meth.name}: no such method in parents')
            return
        if not any(f.max_args == meth.max_args for f in found):
            sigs = ', '.join(str(f.max_args) for f in found)
            self.report(path, meth.line, f'override {ci.name}.{meth.name}: {meth.max_args} params, base has {sigs}')

    def check_type(self, path, line, ty):
        if not ty or ty == '...':
            return
        name = ty.name
        if name in ('T', 'TKey', 'TValue', 'Class', 'void'):
            return
        if not self.m.type_exists(name):
            self.report(path, line, f'unknown type {name}')
        for a in ty.args:
            self.check_type(path, line, a)

    # ------------------------------------------------------------------
    def check_body(self, path, ci, meth, toks, s, e):
        scope = {}
        for pname, pty, _ in meth.params:
            if pty != '...':
                scope[pname] = pty
        i = s + 1
        while i < e:
            t = toks[i]
            # local declarations: Type name [=;,)]   (also inside foreach / for)
            if t.kind == 'id' and t.val not in KEYWORDS and (i == 0 or toks[i - 1].val not in ('.',)):
                ty, j = parse_type(toks, i)
                if ty and j < e and toks[j].kind == 'id' and toks[j].val not in KEYWORDS and \
                        j + 1 < e and toks[j + 1].val in ('=', ';', ',', ':', '[', ')') and \
                        self.m.type_exists(self.m.resolve_typedef(ty).name if self.m.resolve_typedef(ty) else ty.name):
                    is_arr = toks[j + 1].val == '['
                    scope[toks[j].val] = TypeRef(ty.name, ty.args, is_arr)
                    self.check_type(path, t.line, ty)
                    i = j
                    continue
            if t.kind == 'id' and t.val[0].isupper() and t.val not in scope and i + 2 < e and \
                    toks[i + 1].kind == 'id' and toks[i + 2].val in ('=', ';') and toks[i - 1].val in (';', '{', '}', '(') and \
                    not self.m.type_exists(t.val) and not (ci and self.m.find_field(ci.name, t.val)[0]):
                self.report(path, t.line, f'unknown type {t.val}')
            if t.val == 'new' and i + 1 < e and toks[i + 1].kind == 'id':
                cname = toks[i + 1].val
                if not self.m.type_exists(cname):
                    self.report(path, t.line, f'new: unknown class {cname}')
                elif cname in self.m.classes and i + 2 < e and toks[i + 2].val == '(':
                    close = match_close(toks, i + 2, '(', ')')
                    nargs = self.count_args(toks, i + 3, close)
                    ctors = self.m.classes[cname].methods.get(cname, [])
                    if ctors and not any(c.min_args <= nargs <= c.max_args or c.varargs for c in ctors):
                        rng = ', '.join(f'{c.min_args}-{c.max_args}' for c in ctors)
                        self.report(path, t.line, f'new {cname}: {nargs} args, constructor takes {rng}')
            # expression chains starting at an identifier
            if t.kind == 'id' and (i == 0 or toks[i - 1].val != '.') and t.val not in KEYWORDS - {'this', 'super'}:
                i = self.check_chain(path, ci, meth, scope, toks, i, e)
                continue
            i += 1

    def count_args(self, toks, s, e):
        if s >= e:
            return 0
        depth = 0
        count = 1
        for k in range(s, e):
            v = toks[k].val
            if v in '([{':
                depth += 1
            elif v in ')]}':
                depth -= 1
            elif v == ',' and depth == 0:
                count += 1
        return count

    def check_chain(self, path, ci, meth, scope, toks, i, e):
        t = toks[i]
        name = t.val
        cur = None          # TypeRef of current value
        static_ctx = None   # class/enum name when accessing statics
        j = i + 1

        if name == 'this':
            cur = TypeRef(ci.name)
        elif name == 'super':
            cur = TypeRef(ci.parent) if ci.parent else None
        elif name in scope:
            cur = scope[name]
        else:
            fty, _ = self.m.find_field(ci.name, name) if ci else (None, None)
            if fty:
                cur = fty
            elif name in self.m.globals:
                cur = self.m.globals[name]
            elif name in self.m.classes or name in self.m.enums or name in self.m.typedefs:
                static_ctx = name
            elif j < e and toks[j].val == '(':
                # free function call or own method
                close = match_close(toks, j, '(', ')')
                nargs = self.count_args(toks, j + 1, close)
                cands = (self.m.find_methods(ci.name, name) if ci else []) + self.m.functions.get(name, [])
                if not cands:
                    if name not in ('Print', 'PrintFormat', 'Vector', 'GetGame', 'typeof', 'sizeof', 'Debug'):
                        self.report(path, t.line, f'unknown function {name}()')
                    return j
                self.check_args(path, t.line, name, cands, nargs)
                cur = cands[0].ret
                j = close + 1
            else:
                # unknown identifier (could be a local we failed to parse) - stay silent
                return j

        while j < e:
            v = toks[j].val
            if v == '[' :
                close = match_close(toks, j, '[', ']')
                if cur:
                    cur = self.index_type(cur)
                j = close + 1
                continue
            if v != '.' or j + 1 >= e or toks[j + 1].kind != 'id':
                break
            member = toks[j + 1].val
            j += 2
            is_call = j < e and toks[j].val == '('
            if static_ctx:
                ctx = static_ctx
                static_ctx = None
                if ctx in self.m.enums:
                    if member not in self.m.enum_values(ctx):
                        self.report(path, toks[j - 1].line, f'enum {ctx} has no value {member}')
                    cur = TypeRef('int')
                    continue
                rty = self.m.resolve_typedef(TypeRef(ctx))
                ctx_name = rty.name if rty else ctx
                if is_call:
                    close = match_close(toks, j, '(', ')')
                    nargs = self.count_args(toks, j + 1, close)
                    if member == 'Cast':
                        cur = TypeRef(ctx_name)
                    elif member in ('ClassName', 'Type', 'IsInherited', 'ToString', 'EnumToString', 'StringToEnum'):
                        cur = None
                    else:
                        cands = self.m.find_methods(ctx_name, member)
                        if not cands:
                            self.report(path, toks[j - 1].line, f'{ctx_name}.{member}(): no such static method')
                            cur = None
                        else:
                            self.check_args(path, toks[j - 1].line, f'{ctx_name}.{member}', cands, nargs)
                            cur = cands[0].ret
                    j = close + 1
                else:
                    fty, _ = self.m.find_field(ctx_name, member)
                    if fty is None and ctx_name not in ('vector', 'float', 'int', 'string', 'typename'):
                        self.report(path, toks[j - 1].line, f'{ctx_name}.{member}: no such static field/const')
                    cur = fty
                continue

            if cur is None:
                # unknown receiver type: skip rest of chain
                if is_call:
                    j = match_close(toks, j, '(', ')') + 1
                continue
            rcur = self.m.resolve_typedef(cur)
            cname = rcur.name if rcur else cur.name
            if cname in ('int', 'float', 'bool', 'void', 'auto', 'func', 'typename') :
                if is_call:
                    j = match_close(toks, j, '(', ')') + 1
                cur = None
                continue
            if cname in self.m.enums:
                cur = None
                if is_call:
                    j = match_close(toks, j, '(', ')') + 1
                continue
            if cname not in self.m.classes:
                if is_call:
                    j = match_close(toks, j, '(', ')') + 1
                cur = None
                continue
            if is_call:
                close = match_close(toks, j, '(', ')')
                nargs = self.count_args(toks, j + 1, close)
                cands = self.m.find_methods(cname, member)
                if not cands:
                    self.report(path, toks[j - 1].line, f'{cname}.{member}(): no such method')
                    cur = None
                else:
                    self.check_args(path, toks[j - 1].line, f'{cname}.{member}', cands, nargs)
                    if all(getattr(c, 'hidden', False) for c in cands) and name not in ('this', 'super') and \
                            not (ci and cname in [h.name for h in self.m.hierarchy(ci.name)]):
                        self.report(path, toks[j - 1].line, f'{cname}.{member}(): protected/private, not accessible here')
                    cur = self.subst(cands[0].ret, rcur)
                j = close + 1
            else:
                fty, _ = self.m.find_field(cname, member)
                if fty is None:
                    if self.m.find_methods(cname, member):
                        cur = None  # method used as callback
                    else:
                        self.report(path, toks[j - 1].line, f'{cname}.{member}: no such field')
                        cur = None
                else:
                    cur = self.subst(fty, rcur)
        return j

    def index_type(self, ty):
        if ty.is_array:
            return TypeRef(ty.name, ty.args)
        rty = self.m.resolve_typedef(ty)
        if rty and rty.name in ('array', 'set') and rty.args:
            return rty.args[0]
        if rty and rty.name == 'map' and len(rty.args) == 2:
            return rty.args[1]
        if rty and rty.name == 'vector':
            return TypeRef('float')
        if rty and rty.name == 'string':
            return TypeRef('string')
        return None

    def check_args(self, path, line, what, cands, nargs):
        if any(c.varargs or c.min_args <= nargs <= c.max_args for c in cands):
            return
        rng = ', '.join(f'{c.min_args}-{c.max_args}' for c in cands)
        self.report(path, line, f'{what}(): called with {nargs} args, expects {rng}')


def collect(root):
    files = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.endswith('.c'):
                files.append(os.path.join(dp, f))
    return sorted(files)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    vanilla_root, mod_root = sys.argv[1], sys.argv[2]
    model = Model()
    for f in collect(vanilla_root):
        model.parse_file(f)
    mod_files = collect(mod_root)
    for f in mod_files:
        model.parse_file(f)
    checker = Checker(model, mod_files)
    checker.check_all()
    for path, line, msg in sorted(set(checker.issues)):
        print(f'{os.path.relpath(path, mod_root)}:{line}: {msg}')
    print(f'\n{len(set(checker.issues))} issue(s) in {len(mod_files)} file(s); '
          f'{len(model.classes)} classes, {len(model.enums)} enums indexed')
    sys.exit(1 if checker.issues else 0)


if __name__ == '__main__':
    main()

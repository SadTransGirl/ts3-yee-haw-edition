"""Prove a refactor only moved code and added comments: the mod's C++ before (a git ref) and now
(the working tree) must hold exactly the same top-level definitions, token for token.

    python -I tools/code_equiv.py <git ref>            whole src\\ tree: every definition moved, none changed
    python -I tools/code_equiv.py <git ref> --files    also: each file's code tokens are identical
                                                       (for a comments-only pass, no files moved)
    ... --now <dir>                                    compare against another src dir (for testing)

What it compares. Each .cpp/.h/.inc under src\\ is tokenized with comments dropped (string, char
and raw-string literals are single tokens; a preprocessor line is one token), then cut into
top-level items: a function with its body, a struct/enum/typedef, a variable with its
initializer, a #define/#if/#pragma line. extern "C" { } blocks are transparent. The two sides
must have the same multiset of items, after these normalisations, which are what splitting one
file into modules needs and nothing more:
  - a leading "static" is ignored (file-local -> shared linkage);
  - declarations without a body are ignored: function prototypes, "extern" declarations;
  - #include and #pragma once lines are ignored;
  - a plain multi-name declaration ("bool a, b;") counts as one per name.
Everything else (every token inside every function, initializer and struct) must match, so a
logic change, a dropped or duplicated function, or an edited constant shows up as an item only
on one side. Order and file don't matter. A header that didn't exist before may only hold
declarations, types and constants (a mutable static or a function body there would be copied
into every module that includes it). Exit status 0 = equivalent.
"""
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))      # harness\
SRC = "src"
EXTS = (".cpp", ".h", ".inc")
TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<raw>R"(?P<d>[^(\s]*)\(.*?\)(?P=d)")
  | (?P<str>(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")
  | (?P<chr>(?:u8|u|U|L)?'(?:\\.|[^'\\\n])*')
  | (?P<num>\.?\d(?:[\w.]|[eEpP][+-])*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>\S)
""", re.S | re.X)


def tokens(text):
    """Code tokens, comments dropped; each preprocessor line (with continuations) is one token."""
    out, i, bol = [], 0, True
    while i < len(text):
        if bol:
            m = re.match(r"[ \t]*#", text[i:])
            if m:                                           # a directive: up to an unescaped newline
                j = i
                while True:
                    nl = text.find("\n", j)
                    if nl < 0:
                        nl = len(text)
                        break
                    if text[nl - 1:nl] == "\\" or text[nl - 2:nl] == "\\\r":
                        j = nl + 1
                        continue
                    break
                line = re.sub(r"/\*.*?\*/|//.*", "", text[i:nl].replace("\\\n", " "))
                out.append("#" + " ".join(line.strip()[1:].split()))
                i, bol = nl, True
                continue
        m = TOKEN.match(text, i)
        if not m:
            raise ValueError("can't tokenize at %r" % text[i:i + 30])
        kind = m.lastgroup if m.lastgroup != "d" else "raw"
        s = m.group(0)
        if kind == "ws":
            if "\n" in s:
                bol = True
        elif kind in ("lc", "bc"):
            if kind == "bc" and "\n" in s:
                bol = bol                                   # a block comment doesn't start a line
        else:
            out.append(s)
            bol = False
        i = m.end()
    return out


def items(toks):
    """Top-level items, each a tuple of tokens."""
    res, cur, depth, i = [], [], 0, 0
    transparent = []                                        # depths of extern "C" { blocks
    while i < len(toks):
        t = toks[i]
        if depth == 0 and not cur and t.startswith("#"):
            res.append((t,))
            i += 1
            continue
        if depth == 0 and not cur and t == "extern" and i + 2 < len(toks) and toks[i + 1] == '"C"' and toks[i + 2] == "{":
            transparent.append(depth)
            i += 3
            continue
        if t == "}" and transparent and depth == transparent[-1] and not cur:
            transparent.pop()
            i += 1
            continue
        if t.startswith("#"):                               # a directive inside an item (rare): keep it in place
            cur.append(t)
            i += 1
            continue
        cur.append(t)
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
            if depth == 0:
                aggregate = cur[0] in ("struct", "class", "enum", "union", "typedef") or (
                    "=" in cur[:cur.index("{")] if "{" in cur else False) or (
                    len(cur) > 1 and cur[0] == "static" and cur[1] in ("struct", "class", "enum", "union"))
                if not aggregate:
                    res.append(tuple(cur))
                    cur = []
        elif t == ";" and depth == 0:
            res.append(tuple(cur))
            cur = []
        i += 1
    if cur:
        res.append(tuple(cur))
    return res


def normal(item):
    """The comparable form of an item, or None for an item the refactor may add or drop."""
    if len(item) == 1 and item[0].startswith("#"):
        d = item[0]
        if d.startswith("#include") or d == "#pragma once":
            return None
        return d
    toks = list(item)
    while toks and toks[0] == "static":
        toks = toks[1:]
    if not toks:
        return None
    if toks[0] == "extern":
        return None
    if toks[-1] == ";" and toks[0] not in ("typedef", "using", "struct", "class", "enum", "union", "template") \
            and "(" in toks and "=" not in toks and "{" not in toks:
        # a function prototype (also catches "T* name(args);" forms); variables have no "(" unless
        # they're function pointers with an initializer ("= ..."), which keep "=".
        if not re.match(r"^\w[\w:<>,*& ]*\(\s*\*", " ".join(toks)):   # "PFN (*name)(...);" is a variable
            return None
    return " ".join(toks)


def split_declarators(norm):
    """'T a , * b ;' -> ['T a ;', 'T * b ;']: a moved variable may leave a shared declaration.
    Only plain declarations (no initializer, call or body); anything else is returned as is."""
    toks = norm.split(" ")
    if toks[-1] != ";" or "," not in toks or any(t in toks for t in ("=", "(", "{", "<")):
        return [norm]
    parts, cur = [], []
    for t in toks[:-1]:
        if t == ",":
            parts.append(cur)
            cur = []
        else:
            cur.append(t)
    parts.append(cur)
    first = parts[0]
    k = max(i for i, t in enumerate(first) if re.match(r"[A-Za-z_]\w*$", t))   # the first name
    while k > 0 and first[k - 1] in ("*", "&"):
        k -= 1
    base = first[:k]
    if not base:
        return [norm]
    return [" ".join(base + p + [";"]) for p in [first[k:]] + parts[1:]]


def tree_now(root=None):
    root = root or os.path.join(HERE, SRC)
    out = {}
    for dp, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(EXTS):
                p = os.path.join(dp, f)
                out[os.path.relpath(p, root).replace("\\", "/")] = open(p, encoding="utf-8", errors="replace").read()
    return out


def tree_at(ref):
    names = subprocess.run(["git", "-C", HERE, "ls-tree", "-r", "--name-only", ref, SRC + "/"],
                           capture_output=True, text=True, check=True).stdout.split()
    out = {}
    for n in names:
        if n.endswith(EXTS):
            out[n[len(SRC) + 1:]] = subprocess.run(["git", "-C", HERE, "show", "%s:%s" % (ref, n)],
                                                   capture_output=True, check=True).stdout.decode("utf-8", "replace")
    return out


def multiset(tree):
    c, where = collections.Counter(), {}
    for name, text in sorted(tree.items()):
        for it in items(tokens(text)):
            n = normal(it)
            if n is None:
                continue
            for d in split_declarators(n):
                c[d] += 1
                where.setdefault(d, name)
    return c, where


def short(s, n=160):
    return s if len(s) <= n else s[:n] + " ..."


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ref = sys.argv[1]
    other = sys.argv[sys.argv.index("--now") + 1] if "--now" in sys.argv else None   # (testing: another src dir)
    before, now = tree_at(ref), tree_now(other)
    bad = 0
    if "--files" in sys.argv:
        for name in sorted(set(before) | set(now)):
            a = tokens(before.get(name, "")) if name in before else None
            b = tokens(now.get(name, "")) if name in now else None
            if a != b:
                bad += 1
                if a is None or b is None:
                    print("FILE %s: %s" % (name, "new" if a is None else "gone"))
                else:
                    k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                    print("FILE %s: code tokens differ at token %d: was %r, now %r" % (name, k, " ".join(a[k:k + 8]), " ".join(b[k:k + 8])))
    # A header is compiled into every file that includes it, but counted once above. So a new
    # header may hold only things that are safe to have one copy of per module: declarations,
    # types, constants. A mutable static variable or a function body there would become a separate
    # copy in each module (each with its own state) without changing the count.
    for name in sorted(now):
        if name in before or not name.endswith((".h", ".inc")):
            continue
        for it in items(tokens(now[name])):
            t = list(it)
            if t[0].startswith("#"):
                continue
            body = t[-1] == "}" and "(" in t and t[0] not in ("struct", "class", "enum", "union", "typedef")
            mutable_static = t[0] == "static" and t[1] not in ("const", "constexpr") and not body
            if body or mutable_static:
                bad += 1
                print("HEADER %s: %s in a new header (one copy per module): %s" % (
                    name, "a function body" if body else "a mutable static", short(" ".join(t))))
    cb, wb = multiset(before)
    cn, wn = multiset(now)
    gone = cb - cn
    new = cn - cb
    for s, k in sorted(gone.items(), key=lambda x: wb[x[0]]):
        print("ONLY BEFORE (%s) x%d: %s" % (wb[s], k, short(s)))
    for s, k in sorted(new.items(), key=lambda x: wn[x[0]]):
        print("ONLY NOW (%s) x%d: %s" % (wn[s], k, short(s)))
    bad += len(gone) + len(new)
    total = sum(cb.values())
    print("%s: %d definitions before, %d now, %d differ%s" % (
        "EQUIVALENT" if not bad else "NOT EQUIVALENT", total, sum(cn.values()), len(gone) + len(new),
        "" if "--files" not in sys.argv else "; per-file code tokens checked too"))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())

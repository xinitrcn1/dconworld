#!/usr/bin/env python3
"""Replays the vanilla static world init to build src/relationships.cpp's
CreatureTemplate.relationships table (plus the bodySize and name arrays).

It reads the decompiled game code and interprets the parts of it that decide
the relationships, with Downpour (DLCShared, MSC) and the Watcher on and
Remix (MMF) off:

  * StaticWorld.InitStaticWorld with every Init<Creature> helper it calls and
    LizardBreeds.BreedTemplate: every CreatureTemplate that is made, in order,
    its ancestor, its constructor's default relationship (copied into every
    column) and its bodySize (inherited from the ancestor, 1 by default);
  * the ExtEnum<CreatureTemplate.Type> index order (CreatureTemplate.Type's
    own values, then DLCSharedEnums, MoreSlugcatsEnums, WatcherEnums: the
    order ExtEnumInitializer registers them in), which StaticWorld.
    creatureTemplates is indexed by and the "every template" loops walk;
  * InitStaticWorldRelationships, ...MSC and ...Watcher in that order, with
    EstablishRelationship's recursive spread (a's descendants get a's row,
    b's descendants get b's column), the loops over every template (EggBug
    afraid of everything by InverseLerp(0, 8, bodySize), everyone ignoring
    the Overseer, afraid of the BigJelly and the StowawayBug), the TowerCrab
    row reset;
  * the Cyan lizard's Eats intensities cut to 0.3 at the end of
    InitStaticWorld.

It is a tiny C# subset interpreter, not a regex transcription: statements it
does not understand are skipped only when they cannot touch a template list,
an ancestor, a bodySize or a relationship, otherwise it stops with an error,
so a game update that changes the shape of the code is noticed.

The ported templates (the rows and columns, in `enum class tmpl` order) are
the TEMPLATES list below; keep it in sync with src/creature_types.hpp.

usage (plain python3, no third party modules):
    python3 -I tools/relationships.py            print the generated arrays
    python3 -I tools/relationships.py --check    diff them against src/relationships.cpp
    python3 -I tools/relationships.py --write    rewrite them in src/relationships.cpp
    python3 -I tools/relationships.py --first N  only the first N templates
                                                 (to check against an older table)
    --game DIR  decompiled Assembly-CSharp (default: game-code/Assembly-CSharp)
    --cpp FILE  relationships.cpp to check / write (default: src/relationships.cpp)
"""
import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (tmpl enumerator, C# CreatureTemplate.Type name); one inner list per line
# of the body size / names arrays
TEMPLATES = [
    [("slugcat", "Slugcat")],
    [("pink_lizard", "PinkLizard"), ("green_lizard", "GreenLizard"), ("blue_lizard", "BlueLizard"),
     ("yellow_lizard", "YellowLizard"), ("white_lizard", "WhiteLizard"), ("red_lizard", "RedLizard"),
     ("black_lizard", "BlackLizard"), ("salamander", "Salamander"),
     ("cyan_lizard", "CyanLizard"), ("spit_lizard", "SpitLizard"), ("eel_lizard", "EelLizard"),
     ("zoop_lizard", "ZoopLizard"), ("train_lizard", "TrainLizard"), ("blizzard_lizard", "BlizzardLizard"),
     ("basilisk_lizard", "BasiliskLizard"), ("indigo_lizard", "IndigoLizard"), ("peach_lizard", "PeachLizard")],
    [("centipede", "Centipede"), ("small_centipede", "SmallCentipede"), ("centiwing", "Centiwing"),
     ("red_centipede", "RedCentipede"), ("aqua_centi", "AquaCenti")],
    [("leech", "Leech"), ("sea_leech", "SeaLeech"), ("jungle_leech", "JungleLeech")],
    [("cicada_a", "CicadaA"), ("cicada_b", "CicadaB")],
    [("daddy_long_legs", "DaddyLongLegs"), ("brother_long_legs", "BrotherLongLegs"),
     ("terror_long_legs", "TerrorLongLegs"), ("hunter_daddy", "HunterDaddy")],
    [("drop_bug", "DropBug")],
    [("tube_worm", "TubeWorm")],
    [("big_needle_worm", "BigNeedleWorm"), ("small_needle_worm", "SmallNeedleWorm")],
    [("tentacle_plant", "TentaclePlant"), ("pole_mimic", "PoleMimic")],
    [("jet_fish", "JetFish")],
    [("scavenger", "Scavenger")],
    [("deer", "Deer")],
    [("miros_bird", "MirosBird")],
    [("barnacle", "Barnacle")],
    [("egg_bug", "EggBug"), ("fire_bug", "FireBug")],
    [("scavenger_elite", "ScavengerElite"), ("scavenger_disciple", "ScavengerDisciple")],
    [("garbage_worm", "GarbageWorm")],
    [("lantern_mouse", "LanternMouse")],
    [("loach", "Loach")],
    [("vulture", "Vulture"), ("king_vulture", "KingVulture"), ("miros_vulture", "MirosVulture")],
    [("moth_grub", "MothGrub")],
    [("overseer", "Overseer")],
    [("snail", "Snail")],
    [("sky_whale", "SkyWhale")],
    [("yeek", "Yeek")],
]

# ModManager flags: Downpour and the Watcher on, Remix and the rest off
MODS = {"MSC": True, "DLCShared": True, "Watcher": True}

REL_ABBREV = {
    "Ignores": "i", "Eats": "e", "Attacks": "a", "Afraid": "f", "Uncomfortable": "u",
    "AgressiveRival": "r", "StayOutOfWay": "s", "Antagonizes": "n", "PlaysWith": "p",
    "SocialDependent": "d", "Pack": "k",
}

ENUM_FILES = [  # in ExtEnumInitializer's registration order
    "CreatureTemplate.cs",
    "DLCSharedEnums.cs",
    "MoreSlugcats/MoreSlugcatsEnums.cs",
    "Watcher/WatcherEnums.cs",
]

TYPE_RE = r"(?:CreatureTemplate\.Type|(?:DLCSharedEnums|MoreSlugcatsEnums|WatcherEnums)\.CreatureTemplateType)\.(\w+)"
# a statement that could matter when it is skipped
RELEVANT = re.compile(r"bodySize|smallCreature|new CreatureTemplate\(|EstablishRelationship|relationships|"
                      r"BreedTemplate|StaticWorld\.Init|\.Add\(|\breturn\b|creatureTemplates")


class InterpError(Exception):
    pass


# ---------------------------------------------------------------- C# reading

def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            i = src.index("*/", i) + 2
        elif c == '"' or c == "'":
            j = i + 1
            while src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_close(s, i):
    """s[i] is an opening bracket; index of its closing one (strings skipped)"""
    pairs = {"(": ")", "{": "}", "[": "]"}
    stack = [pairs[s[i]]]
    j = i + 1
    while stack:
        c = s[j]
        if c in "\"'":
            k = j + 1
            while s[k] != c:
                k += 2 if s[k] == "\\" else 1
            j = k
        elif c in pairs:
            stack.append(pairs[c])
        elif c in ")}]":
            if c != stack.pop():
                raise InterpError("unbalanced brackets near: " + s[max(0, j - 60):j + 1])
        j += 1
    return j - 1


def split_top(s, sep=","):
    parts, depth, cur, i = [], 0, [], 0
    while i < len(s):
        c = s[i]
        if c in "\"'":
            k = i + 1
            while s[k] != c:
                k += 2 if s[k] == "\\" else 1
            cur.append(s[i:k + 1])
            i = k + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def find_functions(src):
    """name -> list of (param names, body) (overloads in file order)"""
    funcs = {}
    for m in re.finditer(r"(?:public|private|internal)\s+static\s+[\w<>\[\],. ]+?\s+(\w+)\s*\(", src):
        po = m.end() - 1
        pc = match_close(src, po)
        rest = src[pc + 1:]
        k = len(rest) - len(rest.lstrip())
        if not rest[k:].startswith("{"):
            continue
        bo = pc + 1 + k
        bc = match_close(src, bo)
        params = [p.split()[-1] for p in split_top(src[po + 1:pc]) if p]
        funcs.setdefault(m.group(1), []).append((params, src[bo + 1:bc]))
    return funcs


def skip_ws(s, i):
    while i < len(s) and s[i].isspace():
        i += 1
    return i


def parse_stmts(s):
    """a block body -> list of nodes:
    ('stmt', text) ('block', nodes) ('if', [(cond or None, nodes)...]) ('for', header, nodes)"""
    nodes, i = [], 0
    while True:
        i = skip_ws(s, i)
        if i >= len(s):
            return nodes
        node, i = parse_one(s, i)
        if node is not None:
            nodes.append(node)


def parse_one(s, i):
    i = skip_ws(s, i)
    if s[i] == "{":
        j = match_close(s, i)
        return ("block", parse_stmts(s[i + 1:j])), j + 1
    if s[i] == ";":
        return None, i + 1
    m = re.match(r"(if|for|foreach|while)\s*\(", s[i:])
    if m:
        po = i + m.end() - 1
        pc = match_close(s, po)
        head = s[po + 1:pc].strip()
        body, j = parse_one(s, pc + 1)
        body = body[1] if body and body[0] == "block" else [body] if body else []
        if m.group(1) != "if":
            return ("for", m.group(1), head, body), j
        branches = [(head, body)]
        while True:
            k = skip_ws(s, j)
            if not re.match(r"else\b", s[k:]):
                break
            k = skip_ws(s, k + 4)
            m2 = re.match(r"if\s*\(", s[k:])
            if m2:
                po = k + m2.end() - 1
                pc = match_close(s, po)
                cond = s[po + 1:pc].strip()
                body, j = parse_one(s, pc + 1)
            else:
                cond = None
                body, j = parse_one(s, k)
            body = body[1] if body and body[0] == "block" else [body] if body else []
            branches.append((cond, body))
        return ("if", branches), j
    if re.match(r"(switch|do|try)\b", s[i:]):
        raise InterpError("unsupported statement: " + s[i:i + 80])
    # a plain statement up to the ';' at bracket depth 0
    j = i
    while s[j] != ";":
        if s[j] in "([{":
            j = match_close(s, j)
        elif s[j] in "\"'":
            k = j + 1
            while s[k] != s[j]:
                k += 2 if s[k] == "\\" else 1
            j = k
        j += 1
    return ("stmt", " ".join(s[i:j].split())), j + 1


def node_text(node):
    if node[0] == "stmt":
        return node[1]
    if node[0] == "block":
        return " ".join(node_text(n) for n in node[1])
    if node[0] == "if":
        return " ".join((c or "") + " " + " ".join(node_text(n) for n in b) for c, b in node[1])
    return node[2] + " " + " ".join(node_text(n) for n in node[3])


# ---------------------------------------------------------------- the world

class Template:
    def __init__(self, world, tname, ancestor, default):
        self.type = tname
        self.ancestor = ancestor
        self.relationships = [default] * world.n_types
        self.body_size = ancestor.body_size if ancestor else 1.0
        self.small = ancestor.small if ancestor else False

    def __repr__(self):
        return "<%s>" % self.type


class RelArray:
    def __init__(self, t):
        self.t = t


class Unknown:
    def __init__(self, text):
        self.text = text

    def __repr__(self):
        return "<?%s>" % self.text


class Return(Exception):
    def __init__(self, value):
        self.value = value


class Break(Exception):
    pass


class World:
    def __init__(self, game):
        self.game = game
        self.types = []
        for f in ENUM_FILES:
            src = strip_comments((game / f).read_text(encoding="utf-8-sig"))
            if f == "CreatureTemplate.cs":
                found = re.findall(r'static readonly CreatureTemplate\.Type \w+ = new CreatureTemplate\.Type\("(\w+)", true\)', src)
            else:
                m = re.search(r"class CreatureTemplateType\b", src)
                cls = src[m.end():match_close(src, src.index("{", m.end()))]
                found = re.findall(r'new CreatureTemplate\.Type\("(\w+)", true\)', cls)
            for t in found:
                if t not in self.types:
                    self.types.append(t)
        self.index = {t: k for k, t in enumerate(self.types)}
        self.n_types = len(self.types)
        self.funcs = {}
        for f in ("StaticWorld.cs", "LizardBreeds.cs"):
            src = strip_comments((game / f).read_text(encoding="utf-8-sig"))
            cls = f.split(".")[0]
            for name, overloads in find_functions(src).items():
                self.funcs.setdefault(cls + "." + name, []).extend(overloads)
        self.creature_templates = None  # StaticWorld.creatureTemplates

    # ---- interpreter

    def call(self, fname, args):
        overloads = [o for o in self.funcs.get(fname, []) if len(o[0]) == len(args)]
        if len(overloads) != 1:
            raise InterpError("no single %s overload with %d arguments" % (fname, len(args)))
        params, body = overloads[0]
        env = dict(zip(params, args))
        try:
            self.run(parse_stmts(body), env)
        except Return as r:
            return r.value
        return None

    def run(self, nodes, env):
        for node in nodes:
            self.run_node(node, env)

    def run_node(self, node, env):
        kind = node[0]
        if kind == "block":
            self.run(node[1], env)
        elif kind == "if":
            for cond, body in node[1]:
                v = True if cond is None else self.cond(cond, env, node)
                if v:
                    self.run(body, env)
                    break
        elif kind == "for":
            self.run_for(node, env)
        else:
            self.stmt(node[1], env)

    def run_for(self, node, env):
        _, kw, head, body = node
        m = re.fullmatch(r"int (\w+) = 0; \1 < (.+?); \1\+\+", " ".join(head.split()))
        if kw != "for" or not m:
            if RELEVANT.search(node_text(node)):
                raise InterpError("unsupported loop: %s (%s)" % (kw, head))
            return
        if not RELEVANT.search(" ".join(node_text(n) for n in body)):
            return
        var, limit = m.group(1), self.expr(m.group(2), env)
        if not isinstance(limit, int):
            raise InterpError("loop bound %s is %r" % (m.group(2), limit))
        try:
            for k in range(limit):
                env[var] = k
                self.run(body, env)
        except Break:
            pass

    def cond(self, text, env, node):
        try:
            return bool(self.bool_expr(text, env))
        except InterpError:
            if RELEVANT.search(node_text(node)):
                raise
            return False

    def bool_expr(self, text, env):
        text = text.strip()
        parts = split_ops(text, "||")
        if len(parts) > 1:
            return any(self.bool_expr(p, env) for p in parts)
        parts = split_ops(text, "&&")
        if len(parts) > 1:
            return all(self.bool_expr(p, env) for p in parts)
        if text.startswith("!") and not text.startswith("!="):
            return not self.bool_expr(text[1:], env)
        if text.startswith("(") and match_close(text, 0) == len(text) - 1:
            return self.bool_expr(text[1:-1], env)
        m = re.fullmatch(r"ModManager\.(\w+)", text)
        if m:
            return MODS.get(m.group(1), False)
        for op in ("==", "!="):
            parts = split_ops(text, op)
            if len(parts) == 2:
                a, b = self.expr(parts[0], env), self.expr(parts[1], env)
                if isinstance(a, Unknown) or isinstance(b, Unknown):
                    raise InterpError("cannot compare " + text)
                return (a == b) if op == "==" else (a != b)
        v = self.expr(text, env)
        if isinstance(v, bool):
            return v
        raise InterpError("cannot evaluate condition " + text)

    def stmt(self, text, env):
        if text == "break":
            raise Break()
        if text == "return":
            raise Return(None)
        m = re.fullmatch(r"return (.+)", text)
        if m:
            raise Return(self.expr(m.group(1), env))
        # X.relationships[i] = REL / arr[i].intensity = arr[i].intensity * K
        m = re.fullmatch(r"(\w+)\.relationships\[(\w+)\] = (.+)", text)
        if m:
            t, k = self.expr(m.group(1), env), self.expr(m.group(2), env)
            t.relationships[k] = self.expr(m.group(3), env)
            return
        m = re.fullmatch(r"(\w+)\[(\w+)\]\.intensity = \1\[\2\]\.intensity \* ([\d.]+)f?", text)
        if m:
            arr, k = env[m.group(1)], self.expr(m.group(2), env)
            if not isinstance(arr, RelArray):
                raise InterpError(text)
            rt, iv = arr.t.relationships[k]
            arr.t.relationships[k] = (rt, iv * float(m.group(3)))
            return
        # declaration or assignment to a local
        m = re.fullmatch(r"(?:[\w.<>\[\]]+ )?(\w+) = (.+)", text)
        if m and "." not in text.split("=")[0].split()[-1]:
            env[m.group(1)] = self.expr(m.group(2), env)
            return
        # member assignment
        m = re.fullmatch(r"(.+)\.(\w+) = (.+)", text)
        if m and not re.search(r"\[[^\]]*$", m.group(1)):
            if m.group(2) in ("bodySize", "smallCreature"):
                t = self.expr(m.group(1), env)
                if not isinstance(t, Template):
                    raise InterpError("%s on %r" % (text, t))
                v = self.expr(m.group(3), env)
                if m.group(2) == "bodySize":
                    t.body_size = v
                else:
                    t.small = v
            elif m.group(2) == "ancestor" or "relationships" in m.group(1):
                raise InterpError("unsupported: " + text)
            return
        m = re.fullmatch(r"(\w+)\.Add\((.+)\)", text)
        if m:
            lst = env.get(m.group(1))
            if isinstance(lst, list):
                lst.append(self.expr(m.group(2), env))
            elif RELEVANT.search(m.group(2)) and "new CreatureTemplate(" in m.group(2):
                raise InterpError("Add to unknown list: " + text)
            return
        m = re.fullmatch(r"StaticWorld\.EstablishRelationship\((.+)\)", text)
        if m:
            a, b, rel = (self.expr(x, env) for x in split_top(m.group(1)))
            for v in (a, b, rel):
                if isinstance(v, Unknown):
                    raise InterpError("cannot evaluate %s in %s" % (v.text, text))
            self.establish(a, b, rel)
            return
        m = re.fullmatch(r"((?:StaticWorld|LizardBreeds)\.\w+)\((.*)\)", text)
        if m:
            if m.group(1) in self.funcs:
                self.call(m.group(1), [self.expr(x, env) for x in split_top(m.group(2))])
                return
        if RELEVANT.search(text) and not re.match(r"Custom\.Log", text):
            raise InterpError("unsupported statement: " + text)

    def expr(self, text, env):
        text = text.strip()
        if text == "null":
            return None
        if text in ("true", "false"):
            return text == "true"
        m = re.fullmatch(r"-?\d+(\.\d*)?f?", text)
        if m:
            return float(text.rstrip("f")) if (m.group(1) or text.endswith("f")) else int(text)
        m = re.fullmatch(TYPE_RE, text)
        if m:
            return ("type", m.group(1))
        m = re.fullmatch(r"CreatureTemplate\.Relationship\.Type\.(\w+)", text)
        if m:
            return ("rel", m.group(1))
        m = re.fullmatch(r"ModManager\.(\w+)", text)
        if m:
            return MODS.get(m.group(1), False)
        if re.fullmatch(r"\w+", text):
            if text in env:
                return env[text]
            raise InterpError("unknown name " + text)
        m = re.fullmatch(r"new CreatureTemplate\.Relationship\((.+)\)", text)
        if m:
            rt, iv = (self.expr(x, env) for x in split_top(m.group(1)))
            return (rt[1], float(iv))
        m = re.match(r"new CreatureTemplate\(", text)
        if m:
            pc = match_close(text, m.end() - 1)
            args = split_top(text[m.end():pc])
            if len(args) != 5:
                return Unknown(text)  # the copy constructor: not used by the init
            t = Template(self, self.expr(args[0], env)[1], self.expr(args[1], env), self.expr(args[4], env))
            init = text[pc + 1:].strip()
            if init:  # object initializer { a = b, ... }
                for a in split_top(init[1:-1]):
                    k, v = (x.strip() for x in a.split("=", 1))
                    if k == "bodySize":
                        t.body_size = self.expr(v, env)
                    elif k == "smallCreature":
                        t.small = self.expr(v, env)
            return t
        if re.fullmatch(r"new List<CreatureTemplate>\(\)", text):
            return []
        m = re.fullmatch(r"(\w+)\[\1\.Count - (\d+)\]", text)
        if m:
            return env[m.group(1)][-int(m.group(2))]
        m = re.fullmatch(r"(\w+)\.Count", text)
        if m and isinstance(env.get(m.group(1)), list):
            return len(env[m.group(1)])
        m = re.fullmatch(r"(\w+)\[(\w+)\]\.type", text)
        if m and isinstance(env.get(m.group(1)), list):
            return ("type", env[m.group(1)][self.expr(m.group(2), env)].type)
        if text == "StaticWorld.creatureTemplates.Length":
            return len(self.creature_templates)
        m = re.fullmatch(r"StaticWorld\.creatureTemplates\[(\w+)\]\.bodySize", text)
        if m:
            return self.creature_templates[self.expr(m.group(1), env)].body_size
        m = re.fullmatch(r"(\w+)\.relationships\.Length", text)
        if m:
            return len(self.expr(m.group(1), env).relationships)
        m = re.fullmatch(r"(\w+)\.relationships", text)
        if m:
            return RelArray(self.expr(m.group(1), env))
        m = re.fullmatch(r"(\w+)\.relationships\[(\w+)\]\.type", text)
        if m:
            return ("rel", self.expr(m.group(1), env).relationships[self.expr(m.group(2), env)][0])
        m = re.fullmatch(r"new CreatureTemplate\.Type\(ExtEnum<CreatureTemplate\.Type>\.values\.GetEntry\((\w+)\), false\)", text)
        if m:
            return ("type", self.types[self.expr(m.group(1), env)])
        m = re.fullmatch(r"Mathf\.InverseLerp\((.+)\)", text)
        if m:
            a, b, v = (self.expr(x, env) for x in split_top(m.group(1)))
            return min(1.0, max(0.0, (v - a) / (b - a)))
        m = re.fullmatch(r"StaticWorld\.GetCreatureTemplate\((.+)\)", text)
        if m:
            return self.get_template(self.expr(m.group(1), env))
        m = re.fullmatch(r"((?:StaticWorld|LizardBreeds)\.\w+)\((.*)\)", text)
        if m and m.group(1) in self.funcs:
            return self.call(m.group(1), [self.expr(x, env) for x in split_top(m.group(2))])
        return Unknown(text)

    # ---- StaticWorld

    def get_template(self, t):
        if t is None or t[1] not in self.index:
            return None
        return self.creature_templates[self.index[t[1]]]

    def establish(self, a, b, rel):
        ta, tb = self.get_template(a), self.get_template(b)
        if ta is None or tb is None:
            return
        ta.relationships[self.index[tb.type]] = rel
        for t3 in self.creature_templates:
            if t3.ancestor is ta:
                self.establish(("type", t3.type), ("type", tb.type), rel)
            if t3.ancestor is tb:
                self.establish(("type", ta.type), ("type", t3.type), rel)

    def init(self):
        # InitStaticWorld up to the creatureTemplates array, then the rest of
        # it with the array in place (its relationship calls need it)
        params, body = self.funcs["StaticWorld.InitStaticWorld"][0]
        nodes = parse_stmts(body)
        env = {}
        skipping = False
        for node in nodes:
            if node[0] == "stmt" and node[1].startswith("StaticWorld.creatureTemplates = "):
                lst = env["list"]
                self.creature_templates = [None] * max(len(lst), self.n_types)
                for t in lst:
                    self.creature_templates[self.index[t.type]] = t
                for k, t in enumerate(self.creature_templates):
                    if t is None:  # the StandardGroundCreature placeholders
                        self.creature_templates[k] = Template(self, "StandardGroundCreature", None, ("Ignores", 0.0))
                self.list = lst
                # what follows up to the relationship init is that array's
                # fill (done above), InitCustomTemplates (only logs) and the
                # pre baked pathing / quantified indices
                skipping = True
                continue
            if skipping:
                if not (node[0] == "stmt" and node[1].startswith("StaticWorld.InitStaticWorldRelationships(")):
                    continue
                skipping = False
            self.run_node(node, env)
        if self.creature_templates is None:
            raise InterpError("InitStaticWorld never made StaticWorld.creatureTemplates")

    def template(self, name):
        for t in self.list:
            if t.type == name:
                return t
        raise InterpError("no template " + name)


def split_ops(text, op):
    parts, depth, cur, i = [], 0, 0, 0
    out = []
    while i < len(text):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and text.startswith(op, i):
            out.append(text[cur:i].strip())
            i += len(op)
            cur = i
            continue
        i += 1
    out.append(text[cur:].strip())
    return out


# ---------------------------------------------------------------- output

def fmt(v):
    s = "%.6g" % v
    return (s + ".f") if "." not in s and "e" not in s else s + "f"


def generate(world, templates):
    flat = [t for group in templates for t in group]
    ts = [world.template(cs) for _, cs in flat]
    rows = []
    for t in ts:
        cells = []
        for o in ts:
            rt, iv = t.relationships[world.index[o.type]]
            if rt not in REL_ABBREV:
                raise InterpError("%s -> %s is %s, which the C++ rel enum has no value for" % (t.type, o.type, rt))
            cells.append("{%s,%s}" % (REL_ABBREV[rt], fmt(iv)))
        rows.append("        // %s\n        {%s}," % (t.type, ", ".join(cells)))
    table = "        constexpr relationship table[tmpl_count][tmpl_count] = {\n" + "\n".join(rows) + "\n        };"
    k = 0
    size_lines, name_lines = [], []
    for group in templates:
        g = ts[k:k + len(group)]
        k += len(group)
        size_lines.append("            " + " ".join(fmt(t.body_size) + "," for t in g))
        names = ['"%s",' % t.type for t in g]
        if len(names) > 9:  # the lizards: two lines, as before
            name_lines.append("            " + " ".join(names[:8]))
            names = names[8:]
        name_lines.append("            " + " ".join(names))
    sizes = "        constexpr float body_sizes[tmpl_count] = {\n" + "\n".join(size_lines) + "\n        };"
    names = "        constexpr char const* names[tmpl_count] = {\n" + "\n".join(name_lines) + "\n        };"
    small = [cpp for (cpp, _), t in zip(flat, ts) if t.small]
    return {"table": table, "body_sizes": sizes, "names": names}, small


SECTIONS = {
    "table": r"        constexpr relationship table\[tmpl_count\]\[tmpl_count\] = \{\n.*?\n        \};",
    "body_sizes": r"        constexpr float body_sizes\[tmpl_count\] = \{\n.*?\n        \};",
    "names": r"        constexpr char const\* names\[tmpl_count\] = \{\n.*?\n        \};",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", type=Path, default=ROOT / "game-code" / "Assembly-CSharp")
    ap.add_argument("--cpp", type=Path, default=ROOT / "src" / "relationships.cpp")
    ap.add_argument("--first", type=int, default=None)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    args = ap.parse_args()

    templates = TEMPLATES
    if args.first is not None:
        left, templates = args.first, []
        for group in TEMPLATES:
            if left <= 0:
                break
            templates.append(group[:left])
            left -= len(group)

    world = World(args.game)
    world.init()
    out, small = generate(world, templates)
    print("smallCreature: " + ", ".join(small), file=sys.stderr)

    if not args.check and not args.write:
        print("\n\n".join(out[k] for k in ("table", "body_sizes", "names")))
        return 0
    cpp = args.cpp.read_text()
    if args.check:
        bad = 0
        for k, pat in SECTIONS.items():
            m = re.search(pat, cpp, re.S)
            if not m:
                print("%s: no %s array found" % (args.cpp, k))
                bad += 1
                continue
            if m.group(0) != out[k]:
                bad += 1
                sys.stdout.writelines(difflib.unified_diff(
                    (m.group(0) + "\n").splitlines(True), (out[k] + "\n").splitlines(True), str(args.cpp) + ":" + k, "generated"))
        print("ok" if not bad else "%d array(s) differ" % bad)
        return 1 if bad else 0
    for k, pat in SECTIONS.items():
        cpp, n = re.subn(pat, lambda _m, k=k: out[k], cpp, count=1, flags=re.S)
        if n != 1:
            print("%s: no %s array found" % (args.cpp, k), file=sys.stderr)
            return 1
    args.cpp.write_text(cpp)
    return 0


if __name__ == "__main__":
    sys.exit(main())

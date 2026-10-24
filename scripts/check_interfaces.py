#!/usr/bin/env python3
"""Checks the three 'stringly-typed' seams the compiler cannot see. Fast (<1 s), no dependencies.

 1. format strings   Kotlin literals used with .format()/String.format(): every '%' must be a valid
                     java.util.Formatter specifier (v1.0.0 crashed at launch on "55%  (trigger ...").
 2. config keys      every key Kotlin sends to the native engine (ModelManager kv("..")) must be parsed by
                     EngineConfig::parse in engine.cpp, otherwise it is silently ignored.
 3. JNI signatures   NativeBridge.kt `external fun`s vs the RegisterNatives table and the C++ parameter lists.

Usage: check_interfaces.py [--root DIR]      exit status 1 when anything is wrong
"""
import argparse, pathlib, re, sys

FLAGS = "-#+ 0,(<"
CONVERSIONS = set("bBhHsScCdoxXeEfgGaAtTn%")
SPEC = re.compile(r"%(\d+\$)?([-#+ 0,(<]*)(\d+)?(\.\d+)?([tT])?([a-zA-Z%])?")
LITERAL = r'"(?:[^"\\\n]|\\.)*"'


def kotlin_files(root):
    return sorted((root / "app/src/main/java").rglob("*.kt"))


DATETIME = set("HIklMSLNpzZsQBbhAaCYyjmdeRTrDFc")
NUMERIC = set("deEfgGaAoxX")


def unescape_kotlin(body):
    """Kotlin literal text -> the runtime string the formatter will see."""
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n == "u" and i + 5 < len(body) + 0 and re.fullmatch(r"[0-9a-fA-F]{4}", body[i + 2:i + 6] or ""):
                out.append(chr(int(body[i + 2:i + 6], 16)))
                i += 6
                continue
            out.append({"n": "\n", "t": "\t", "r": "\r", "b": "\b"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_format_template(lit):
    """Returns a list of problems in one Kotlin string literal (including the quotes)."""
    raw = lit[1:-1]
    problems = []
    if re.search(r"(?<!\\)\$(\{|[A-Za-z_])", raw):
        problems.append("string interpolation inside a format template (the interpolated text is not escaped): pass it as an argument")
    body, i = unescape_kotlin(raw), 0
    while i < len(body):
        if body[i] != "%":
            i += 1
            continue
        m = SPEC.match(body, i)
        prefix, flags, conv = m.group(5), (m.group(2) or ""), m.group(6)
        near = body[i:i + 14]
        if conv is None or (not prefix and conv not in CONVERSIONS) or (prefix and conv not in DATETIME):
            problems.append(f"invalid format specifier near {near!r} (write %% for a literal percent sign)")
        elif prefix and conv not in DATETIME:
            problems.append(f"invalid date/time conversion near {near!r}")
        elif len(set(flags)) != len(flags):
            problems.append(f"duplicate flags {flags!r} near {near!r} -> DuplicateFormatFlagsException at runtime")
        elif conv == "n" and (flags or m.group(3) or m.group(4)):
            problems.append(f"%n takes no flags/width/precision near {near!r}")
        elif conv == "%" and set(flags) - {"-"}:
            problems.append(f"flags {flags!r} are not allowed on %% near {near!r} -> FormatFlagsConversionMismatchException")
        elif not prefix and conv not in NUMERIC and set(flags) & set("+ (,0"):
            problems.append(f"flags {flags!r} do not fit conversion %{conv} near {near!r} -> FormatFlagsConversionMismatchException")
        elif i > 0 and body[i - 1].isdigit() and flags == " " and conv in NUMERIC:
            problems.append(f"{body[i - 1]}% followed by a space looks like a literal percent sign near {near!r}: write %%")
        i = m.end() if m.end() > i else i + 1
    return problems


def check_formats(root):
    errors = []
    for f in kotlin_files(root):
        text = f.read_text(encoding="utf-8")
        found = []
        for m in re.finditer(r"(" + LITERAL + r")\s*\.format\(", text):
            found.append((m.group(1), m.start(1)))
        for m in re.finditer(r"String\.format\(\s*(?:[\w.]+\s*,\s*)?(" + LITERAL + r")", text):
            found.append((m.group(1), m.start(1)))
        for lit, pos in found:
            line = text.count("\n", 0, pos) + 1
            if "lint:format-ok" in text.splitlines()[line - 1]:
                continue
            for p in check_format_template(lit):
                errors.append(f"{f.relative_to(root)}:{line}: format string: {p}\n      {lit}")
    return errors


def check_config_keys(root):
    kt = (root / "app/src/main/java/com/maya/assistant/ModelManager.kt").read_text(encoding="utf-8")
    cpp = (root / "app/src/main/cpp/src/engine.cpp").read_text(encoding="utf-8")
    sent = set(re.findall(r'kv\("([\w.]+)"', kt))
    parsed = set(re.findall(r'\b(?:str|i32|f32|flag)\("([\w.]+)"', cpp)) | set(re.findall(r'k == "([\w.]+)"', cpp))
    return [f"config key {k!r} is sent by ModelManager.kt but never parsed by EngineConfig::parse (silently ignored)"
            for k in sorted(sent - parsed)]


JNI_FROM_KOTLIN = {"Long": "J", "Boolean": "Z", "Float": "F", "Int": "I", "Double": "D", "String": "Ljava/lang/String;",
                   "ShortArray": "[S", "ByteArray": "[B", "MayaListener": "Lcom/maya/assistant/MayaListener;", "Unit": "V"}
JNI_FROM_CPP = {"jlong": "J", "jboolean": "Z", "jfloat": "F", "jint": "I", "jdouble": "D", "jstring": "Ljava/lang/String;",
                "jshortArray": "[S", "jbyteArray": "[B", "void": "V"}


def check_jni(root):
    errors = []
    kt = (root / "app/src/main/java/com/maya/assistant/NativeBridge.kt").read_text(encoding="utf-8")
    cpp = (root / "app/src/main/cpp/src/maya_jni.cpp").read_text(encoding="utf-8")
    declared = {}
    for m in re.finditer(r"external fun (\w+)\(([^)]*)\)\s*(?::\s*(\w+))?", kt):
        params = [p.split(":")[1].strip() for p in m.group(2).split(",") if p.strip()]
        try:
            sig = "(" + "".join(JNI_FROM_KOTLIN[p] for p in params) + ")" + JNI_FROM_KOTLIN[m.group(3) or "Unit"]
        except KeyError as e:
            errors.append(f"NativeBridge.kt: no JNI mapping for Kotlin type {e} in {m.group(1)}")
            continue
        declared[m.group(1)] = sig
    registered = dict(re.findall(r'\{"(\w+)",\s*"([^"]+)",\s*reinterpret_cast', cpp))
    for name, sig in declared.items():
        if name not in registered:
            errors.append(f"JNI: external fun {name} is not registered in maya_jni.cpp (UnsatisfiedLinkError at first use)")
        elif registered[name] != sig:
            errors.append(f"JNI: {name} signature mismatch: Kotlin implies {sig}, native table says {registered[name]}")
    for name in registered:
        if name not in declared:
            errors.append(f"JNI: {name} is registered natively but has no external fun in NativeBridge.kt (RegisterNatives fails, the library will not load)")
    # the C++ definition must take exactly the types the descriptor promises (a mismatch is undefined behaviour)
    for name, sig in registered.items():
        m = re.search(r"^(\w+)\s+" + name + r"\(([^)]*)\)\s*\{", cpp, re.M)
        if not m:
            errors.append(f"JNI: could not find the C++ definition of {name}")
            continue
        ret = JNI_FROM_CPP.get(m.group(1))
        params = [p.strip().rsplit(" ", 1)[0].strip() for p in m.group(2).split(",")][2:]   # skip JNIEnv*, jobject
        mapped = []
        for p in params:
            mapped.append("Lcom/maya/assistant/MayaListener;" if p == "jobject" else JNI_FROM_CPP.get(p, "?" + p))
        cpp_sig = "(" + "".join(mapped) + ")" + (ret or "?" + m.group(1))
        if cpp_sig != sig:
            errors.append(f"JNI: C++ {name} takes/returns {cpp_sig} but is registered as {sig}")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    root = pathlib.Path(ap.parse_args().root)
    sections = [("format strings", check_formats), ("config keys", check_config_keys), ("JNI signatures", check_jni)]
    bad = 0
    for title, fn in sections:
        errs = fn(root)
        print(f"[{'FAIL' if errs else ' ok '}] {title}")
        for e in errs:
            print("   " + e)
        bad += len(errs)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

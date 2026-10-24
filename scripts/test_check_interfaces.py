#!/usr/bin/env python3
"""Self-test for check_interfaces.py: legitimate formats must pass, the known-bad ones must fail."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_interfaces import check_format_template as chk

GOOD = ['"%.4f"', '"%02x"', '"%d%%"', '"%s: %d"', '"%5.1f%%"', '"%n"', '"%1\\$s and %2\\$s"', '"100%%"', '"%-8s|"', '"%,d"', '"no specifiers at all"',
        '"Sensitivity %d%%  (trigger threshold %.3f, %s)"', '"%tY-%tm"']
BAD = ['"Sensitivity 55%  (trigger threshold %.3f)"',      # the v1.0.0 crash
       '"100%"',                                           # trailing percent
       '"50% done"',                                       # %<space>d is a valid flag+conversion... but 'd' eats an argument silently
       '"%  x"', '"% % x"', '"%q"', '"%.f%"', '"a ${b} %d"', '"a $b %d"', '"%1$s"']   # last: unescaped $s is a Kotlin template, not an argument index
failures = 0
for lit in GOOD:
    p = chk(lit)
    if p: failures += 1; print("FALSE POSITIVE:", lit, p)
for lit in BAD:
    p = chk(lit)
    if not p: failures += 1; print("MISSED:", lit)
print("check_interfaces self-test:", "ok" if not failures else f"{failures} problem(s)")
sys.exit(1 if failures else 0)

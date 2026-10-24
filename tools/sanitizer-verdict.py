#!/usr/bin/env python3
# Usage: sanitizer-verdict.py LOG
# Exits 1 when the log of a sanitizer run holds an error or a leak tinyiv can answer for.
# A leak allocated only from a library unloaded before exit has no frame to name, so it is
# listed and passed. The Vulkan driver is one, closed when SDL quits.
import re
import sys

text = open(sys.argv[1], errors="replace").read()
errors = re.findall(r"ERROR: AddressSanitizer: [^\n]*|runtime error: [^\n]*", text)
ours, unloaded = [], []

for block in re.split(r"\n(?=(?:Direct|Indirect) leak of )", text)[1:]:
    head = block.splitlines()[0]
    frames = re.findall(r"#\d+ 0x[0-9a-f]+ +(?:in (\S+) )?\(?([^)\n]*)", block)
    named = [f for f in frames[1:] if f[0] or "<unknown module>" not in f[1]]
    (ours if named else unloaded).append((head, named[:6]))

for error in errors:
    print(error)

for head, frames in ours:
    print(head)
    for function, where in frames:
        print(f"    {function or '?'} {where.split('/')[-1]}")

if unloaded:
    total = sum(int(re.search(r"of (\d+) byte", head).group(1)) for head, _ in unloaded)
    print(f"{len(unloaded)} leaks, {total} bytes, come from libraries unloaded before exit and are not counted")

sys.exit(1 if errors or ours else 0)

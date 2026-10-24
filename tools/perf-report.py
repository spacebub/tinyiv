#!/usr/bin/env python3
# Usage: perf-report.py OUT TREE STATUS
# Turns the perf recording tools/harness.sh made in OUT into OUT/report.md, with annotated
# listings of the hottest tinyiv functions and folded stacks for a flame graph.
import collections
import datetime
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
TREE = Path(sys.argv[2])
STATUS = sys.argv[3]
DATA = OUT / "perf.data"
BINARY = "tiv_harness"
ROW = re.compile(r"^\s*([\d.]+)%\s+(?:\S+\s+)?\[(.)\]\s+(.+?)\s*$")


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=False).stdout


def report(*args, limit=None):
    text = run("perf", "report", "-i", str(DATA), "--stdio", *args)
    lines = [line for line in text.splitlines() if line.strip() and not line.startswith("#")]

    return "\n".join(lines[:limit] if limit else lines)


def block(text):
    return f"```\n{text.rstrip()}\n```\n"


def header():
    text = run("perf", "report", "-i", str(DATA), "--stdio", "--sort", "comm", "--percent-limit", "100")

    return [line[2:] for line in text.splitlines() if line.startswith(("# Samples", "# Event count"))]


def machine():
    cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.processor())
    memory = next((int(line.split()[1]) // (1024 * 1024) for line in Path("/proc/meminfo").read_text().splitlines()
                   if line.startswith("MemTotal")), 0)
    root = TREE.parent
    commit = run("git", "-C", str(root), "rev-parse", "--short", "HEAD").strip()
    dirty = " with uncommitted changes" if run("git", "-C", str(root), "status", "--porcelain").strip() else ""
    cache = (TREE / "CMakeCache.txt").read_text() if (TREE / "CMakeCache.txt").exists() else ""
    compiler = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.*)$", cache, re.M)
    version = run(compiler.group(1), "--version").splitlines()[0] if compiler else "unknown"

    return [
        f"Recorded {datetime.datetime.now():%Y-%m-%d %H:%M} at commit {commit}{dirty}.",
        f"The CPU is {cpu}, with {os.cpu_count()} threads and {memory} GB of memory.",
        f"The build used {version}.",
        f"The harness exited with status {STATUS}.",
    ]


def harness_summary():
    path = OUT / "harness.txt"

    if not path.exists():
        return "The harness wrote nothing."

    # The app, libvips, libheif and the GPU driver print to the same output.
    noise = re.compile(r"^(tinyiv: |WARNING: |/|heif: |Vips|glib: |\[ perf record|\(process)|\s+\S.* waiting ")

    return "\n".join(line for line in path.read_text().splitlines() if not noise.match(line))


def hottest_own(limit):
    found = []

    for line in report("--no-children", "--dsos", BINARY, "--sort", "sym", "-g", "none").splitlines():
        match = ROW.match(line)

        if match:
            found.append(match.group(3))

        if len(found) == limit:
            break

    return found


# Frames of std::thread and std::function plumbing, which say nothing about where the time goes.
PLUMBING = re.compile(r"std::(__invoke|thread::_Invoker|thread::_State_impl|_Function_handler)|^\s*\|?\s*0x[0-9a-f]+$")


def callers(count):
    text = report("--no-children", "--dsos", BINARY, "--sort", "sym", "-g", "caller,0.5,callee,function,percent",
                  "--max-stack", "16")
    blocks, current = [], None

    for line in text.splitlines():
        if ROW.match(line):
            if len(blocks) == count:
                break

            current = [line]
            blocks.append(current)
        elif current is not None and not PLUMBING.search(line):
            current.append(line.rstrip())

    return ["\n".join(block[:40]) for block in blocks]


def per_sitting():
    marks = OUT / "marks.tsv"
    parts = []

    if not marks.exists():
        return parts

    for line in marks.read_text().splitlines():
        name, start, end = line.split("\t")
        table = report("--no-children", "--sort", "dso,sym", "-g", "none", "--time", f"{start},{end}",
                       "--percent-limit", "1", limit=20)
        parts.append(f"### {name}\n\n{block(table)}")

    return parts


def annotate(symbols):
    folder = OUT / "annotate"
    folder.mkdir(exist_ok=True)
    written = []

    for number, symbol in enumerate(symbols, 1):
        text = run("perf", "annotate", "-i", str(DATA), "--stdio", "--dsos", BINARY, "--symbol", symbol)
        path = folder / f"{number:02}.txt"
        path.write_text(f"{symbol}\n\n{text}")
        written.append((path.relative_to(OUT), symbol))

    return written


def fold():
    print("folding the stacks, which reads every sample")
    process = subprocess.Popen(["perf", "script", "-i", str(DATA), "-F", "comm,period,ip,sym", "--no-inline"],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    stacks = collections.Counter()
    comm, period, frames = None, 0, []

    def close():
        if comm is not None:
            stacks[";".join([comm] + frames[::-1])] += period

    for line in process.stdout:
        if not line.strip():
            continue

        if not line[0].isspace():
            close()
            parts = line.split()
            comm, period, frames = " ".join(parts[:-1]) or "?", int(parts[-1]) if parts[-1].isdigit() else 1, []
        else:
            parts = line.split(None, 1)
            frames.append(parts[1].strip().replace(";", ":") if len(parts) > 1 else "[unknown]")

    close()
    process.wait()

    path = OUT / "stacks.folded"

    with path.open("w") as out:
        for stack, count in stacks.most_common():
            out.write(f"{stack} {count}\n")

    for tool in ("inferno-flamegraph", "flamegraph.pl"):
        if shutil.which(tool):
            with (OUT / "flamegraph.svg").open("w") as svg, path.open() as folded:
                subprocess.run([tool], stdin=folded, stdout=svg, check=False)

            break


def main():
    own = hottest_own(8)
    sections = [
        "# Profile of tiv_harness\n",
        "\n".join(machine()) + "\n",
        block("\n".join(header())),
        "## Harness\n\nTimes come from the harness itself. A step is the time from the input until the app settled.\n",
        block(harness_summary()),
        "## Threads\n",
        block(report("--no-children", "--sort", "comm", "-g", "none", "--percent-limit", "0.5")),
        "## Libraries\n\nThe codecs, libvips and the GPU driver are shared objects. The tinyiv code and SDL are "
        f"inside {BINARY}.\n",
        block(report("--no-children", "--sort", "dso", "-g", "none", "--percent-limit", "0.3")),
        "## Hottest functions\n",
        block(report("--no-children", "--sort", "dso,sym", "-g", "none", "--percent-limit", "0.3", limit=60)),
        f"## Functions in {BINARY}, self time\n",
        block(report("--no-children", "--dsos", BINARY, "--sort", "sym", "-g", "none", "--percent-limit", "0.1",
                     limit=50)),
        f"## Functions in {BINARY}, with the time of what they call\n",
        block("\n".join(line for line in report("--children", "--dsos", BINARY, "--sort", "sym", "-g", "none",
                                                "--percent-limit", "1", limit=60).splitlines()
                        if not PLUMBING.search(line) and not re.search(r"\[\.\] 0x[0-9a-f]+$", line))),
        "## Callers of the hottest tinyiv functions\n\nEach function comes first, then the calls that led to it, "
        "inlined ones included.\n",
    ]

    sections += [block(chain) for chain in callers(6)]

    sections.append("## Each sitting\n")
    sections += per_sitting()

    if (OUT / "stat.txt").exists():
        sections += ["## Counters\n", block((OUT / "stat.txt").read_text())]

    listings = annotate(own[:5])
    fold()

    files = [
        "## Files\n",
        f"- `perf.data`, for `perf report -i {DATA}` and `perf annotate -i {DATA}`.",
        "- `stacks.folded`, for inferno-flamegraph, flamegraph.pl or speedscope.",
    ]

    if (OUT / "flamegraph.svg").exists():
        files.append("- `flamegraph.svg`.")

    files += [f"- `{path}`, the annotated `{symbol}`." for path, symbol in listings]
    sections.append("\n".join(files) + "\n")

    (OUT / "report.md").write_text("\n".join(sections))


main()

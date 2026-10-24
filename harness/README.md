# tiv_harness

`tiv_harness` runs the real tinyiv app without a window on screen. A script sends the keys, wheel
turns, clicks and drops a person would. After each input it waits until the app has settled, then
checks what the app shows against a corpus the harness writes itself. One run serves three
purposes. Under perf it is a profile of a whole session. Under the sanitizers it is a memory and
undefined behaviour check. Under clang coverage it shows which code no sitting reaches.

Linux and clang only. Built with `-DTIV_HARNESS=ON`.

## Running

```
tools/harness.sh perf
tools/harness.sh sanitize
tools/harness.sh coverage
```

Each mode configures and builds a separate tree, `build-perf`, `build-asan` or `build-cov`.
It writes results into a new folder under `reports/` in that tree, named by the date and time.
Arguments after `--` go to the harness, so `tools/harness.sh perf -- --sitting browse` profiles
one sitting.

Every run goes into a `systemd-run --user --scope` with a hard memory cap and no swap. A run that
goes past the cap is killed alone, and the desktop session stays up. The cap is half the system
memory unless `--memory` says otherwise, such as `--memory 12G`. The small corpus peaks near
3.5 GB while it is written and the full one near 3 GB. A whole run peaks near 2 GB on the small
corpus and 4 GB on the full one.

- `perf` builds Release with debug info and frame pointers, through `-DTIV_SYMBOLS=ON`. It runs the
  full corpus under `perf record` and writes `report.md`. The report shows time by thread, by
  library and by function, inclusive time in tinyiv, the callers of the hottest functions and the
  hottest functions of each sitting. It also writes annotated listings of the top functions and
  `stacks.folded` for a flame graph. `--dwarf` unwinds by DWARF when frame pointers fall short.
  `--stat` adds a second run that counts cycles, instructions and misses. perf records user call
  chains only, which `perf_event_paranoid` at 2 allows. libc on Arch carries no symbols, so its
  samples show as addresses.
- `sanitize` builds Debug with `-DTIV_SANITIZE=ON` and runs the small corpus. Any report from
  AddressSanitizer or UndefinedBehaviorSanitizer fails the run, and so does a leak with a frame
  that has a name. `tools/sanitizer-verdict.py` reads the log. A leak allocated only from a library
  unloaded before exit has no frame to name. Those leaks are listed and pass. The Vulkan driver is
  one, closed when SDL quits. Leaks inside other libraries that do have names go in
  `tools/lsan.supp`.
- `coverage` builds Debug with clang source coverage and runs the small corpus. It writes
  `coverage.txt` per file, `never-run.tsv` with every function in `src/` that no sitting entered,
  and a line by line view in `html/`.

The harness exits 1 when a check failed, and the script passes that on.

## By hand

```
cmake -S . -B build-harness -G Ninja -DCMAKE_BUILD_TYPE=Release -DTIV_HARNESS=ON
cmake --build build-harness --target tiv_harness
build-harness/bin/tiv_harness --list
build-harness/bin/tiv_harness --sitting formats --sitting broken
```

| Option              | Effect                                                                    |
|---------------------|---------------------------------------------------------------------------|
| `--sandbox DIR`     | Where everything goes, `harness-sandbox` in the working directory by default |
| `--size SIZE`       | `small` or `full` corpus, small by default                                |
| `--sitting NAME`    | Runs only this sitting. Give it again for more                            |
| `--repeat N`        | Runs the sittings N times over                                            |
| `--patience SEC`    | How long a step may stand still before it fails, 30 seconds by default    |
| `--marks FILE`      | Writes when each sitting started and ended, for `perf report --time`      |
| `--corpus`          | Writes and checks the corpus, then stops                                  |
| `--list`            | Lists the sittings                                                        |

## Sittings

Each sitting writes a `tinyiv.conf`, opens a folder of the corpus and works through it.

| Sitting     | What it does                                                                 |
|-------------|------------------------------------------------------------------------------|
| `first-run` | Starts with no config and no file, drops a file on the window, then sets every setting away from its default |
| `browse`    | Steps through photos, flicks the wheel both ways, sorts in every order, zooms, pans, turns, toggles the bars, the keybinds and fullscreen |
| `formats`   | Opens one file of each format the decoders tell apart, at fit and at actual size |
| `broken`    | Opens files that have to fail                                                |
| `animated`  | Plays, pauses and steps animations and drags along the play bar              |
| `vector`    | Zooms into SVG and PDF so they render again                                  |
| `oriented`  | Turns and flips every orientation, saves, turns back, saves and compares the bytes |
| `streaming` | Streams photos and huge images with the pyramids in memory, and presses S both ways |
| `persist`   | Builds huge images on disk, then opens them from disk and rebuilds one       |
| `small`     | Does the same with `cache_mode = small`                                      |
| `footprint` | Runs photos and huge images with `memory_mode = footprint`                   |
| `beside`    | Leaves `cache` empty, so pyramids go in `tinyiv-cache` beside the images     |

## How a step waits

The app keeps the main thread, as it does in tinyiv. The script runs on a second thread and
pushes SDL events. Every 5 ms it asks for `App::state()` through `SDL_RunOnMainThread`, which
runs between frames. A step ends once the state says nothing is left to decode, read, upload,
render again or draw, and the event queue is empty. Each look wakes the app loop, so a profile
shows a little `SDL_PumpEvents` on the main thread. A step fails when the image, the build
progress and the animation frame all stand still for longer than `--patience`.

## The sandbox

The harness sets `HOME`, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` inside the sandbox, so runs never
read or write the real config and caches. SDL uses the offscreen video driver unless
`SDL_VIDEO_DRIVER` says otherwise. Each sitting gets an empty folder under `work/`. The harness
empties a folder left by an earlier run only when the folder carries the `.tiv-harness` mark.
The corpus lives in `corpus/small` and `corpus/full` and is written once. A stamp file tells the
harness when the generator changed and the corpus has to be written again.

## What it does not reach

The sittings run about 85% of the lines and 92% of the functions in `src/`. The rest is mostly
out of reach without a hook in tinyiv:

- The offscreen display is SDR and 1024 by 768. HDR output never runs, so neither do the PQ
  encoders, the PQ pyramid halving or the gain map lift for an HDR display.
- An image counts as too large for memory only past half the system memory. The shrink paths for
  such images never run, and the harness reaches the tile caches through the settings instead.
- The rescan source, which reads a JPEG or TIFF again in footprint mode when no other source can,
  never runs. Every huge file in the corpus has a source of its own.
- The loader cache and the GPU tile budget never fill on the small corpus, so eviction runs only
  on the full one.
- Real wheel timing, the compositor and window manager events are out of reach too.

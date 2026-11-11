# Benchmarks

Google Benchmark over the decode, pyramid, upload and loader paths, driven the way the app
drives them. The suite is self contained. The first run writes a synthetic corpus of noise images
into `corpus/` under the working directory, which takes about a minute once. Nothing else is read.

```
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTIV_BENCHMARKS=ON
cmake --build build-bench --target tiv_bench
cd build-bench && ./bin/tiv_bench
```

- `Release` only. A `Debug` build measures `-Og`.
- The render benchmarks need a display for their hidden window and a Vulkan driver.
- Delete a corpus file to have it written again.

Useful flags:

```
./bin/tiv_bench --benchmark_filter='Decode_full|Pyramid_'
./bin/tiv_bench --benchmark_out=before.json --benchmark_out_format=json
python3 ../.download-cache/benchmark-v1.10.0/tools/compare.py benchmarks before.json after.json
```

| Name                  | Measures                                                        |
|-----------------------|-----------------------------------------------------------------|
| `Decode_preview`      | Time to the first pixels the app can show, per format           |
| `Decode_full`         | The whole image, up to the cap                                  |
| `Decode_forced`       | The streamed path that bounds memory for images over the cap    |
| `Pyramid_halve`       | The mipmap kernels, scalar to AVX2                              |
| `TileCache_build`     | Making a tile pyramid on disk or in memory, and what it stores  |
| `TileCache_read`      | Unpacking every full resolution tile, from disk or memory       |
| `TileCache_stores_agree` | Fails when the two stores give different tiles               |
| `TileCache_finest_exact` | Fails when a full resolution tile differs from libvips       |
| `Bmp_decode`          | The BMP row kernels, scalar to AVX2, whole and forced           |
| `JpegBands_exact`     | Fails when a restart band decodes differently from the whole file |
| `JpegBands_index`     | Finding the restart markers, a pass over the file               |
| `JpegBands_band`      | A tile row decoded from its restart marker, one thread and many |
| `Upload_static`       | Texture upload throughput                                       |
| `Tiles_visible_4k`    | The tiles one screen of a 128 MP image needs                    |
| `Tiles_pan_at_budget` | A 1:1 pan across a huge image with VRAM far below its size      |
| `Frame_first`         | Time until a fresh image shows whole and sharp at fit           |
| `Output_switch`       | An SDR image, an HDR one, then the SDR one again                |
| `Canvas_draw`         | One frame of draw calls                                         |
| `Loader_open`         | Cold start, request to first result                             |
| `Loader_step_cached`  | A step between two decoded images, which should take microseconds |
| `Loader_burst`        | A wheel flick through the folder and what the landing costs     |
| `Loader_land_busy`    | A step onto a file not decoded yet while every worker prefetches |
| `Loader_walk`         | A paced walk with prefetch: wait per step and where memory settles |
| `Startup_*`           | Folder listing, window and renderer creation                    |

`peak_rss_mb` beside a result is the memory that run took.

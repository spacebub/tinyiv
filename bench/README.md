# Benchmarks

Google Benchmark over the decode, pyramid, upload and loader paths, driven the way the app
drives them. Self contained: the first run writes a synthetic corpus of noise images into
`corpus/` under the working directory, about a minute once, and nothing else is read.

```
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTIV_BENCHMARKS=ON
cmake --build build-bench --target tiv_bench
cd build-bench && ./bin/tiv_bench
```

- `Release` only. A `Debug` build measures `-Og`.
- The render benchmarks need a display for their hidden window.
- Delete a corpus file to have it written again.

Useful flags:

```
./bin/tiv_bench --benchmark_filter='Decode_full|Pyramid_'
./bin/tiv_bench --benchmark_out=before.json --benchmark_out_format=json
python3 ../.download-cache/benchmark-v1.9.5/tools/compare.py benchmarks before.json after.json
```

| Name                  | Measures                                                        |
|-----------------------|-----------------------------------------------------------------|
| `Decode_preview`      | Time to the first pixels the app can show, per format           |
| `Decode_full`         | The whole image, up to the cap                                  |
| `Decode_forced`       | The streamed path that bounds memory for images over the cap    |
| `Pyramid_halve`       | The mipmap kernels, scalar to AVX2                              |
| `Bmp_decode`          | The BMP row kernels, scalar to AVX2, whole and forced           |
| `Upload_static`       | Texture upload throughput                                       |
| `Tiles_visible_4k`    | The tiles one screen of a 128 MP image needs                    |
| `Canvas_draw`         | One frame of draw calls                                         |
| `Loader_open`         | Cold start, request to first result                             |
| `Loader_step_cached`  | A step between two decoded images, should read in microseconds  |
| `Loader_burst`        | A wheel flick through the folder and what the landing costs     |
| `Loader_walk`         | A paced walk with prefetch: wait per step and where memory settles |
| `Startup_*`           | Folder listing, window and renderer creation                    |

`peak_rss_mb` beside a result is the memory that run took.

# Ollama API on the UGen300, natively on macOS

Stock Ollama can't drive a Hailo NPU. Hailo's own Ollama-compatible server, `hailo-ollama`,
can. [relaxcloud-cn/hailo-ollama-macos](https://github.com/relaxcloud-cn/hailo-ollama-macos)
ports it to Apple Silicon on top of the
[hailort-macos-usb](https://github.com/relaxcloud-cn/hailort-macos-usb) userspace USB runtime.
With `ugen300-loader` booting the stick, the whole chain runs on macOS with no VM:

```text
curl / Ollama client / OpenAI SDK
  └─ hailo-ollama (127.0.0.1:8000, macOS)
      └─ libhailort 5.3.0 (hailort-macos-usb), HRPC over libusb
          └─ UGen300 runtime (booted by ./ugen300-loader rfs-upload)
```

Tested 2026-10-16 on a MacBook (A18 Pro, 8 GB, macOS 27) with the stick on USB-C port 1.
Port 2 is USB 2 only and the stick never enumerates there.

## Build

The checkouts are siblings in `~/Documents/Claude`. `hailort-macos-usb` was already built,
with one local fix: `hailort/common/file_utils.hpp` needs `#include <fstream>` to compile
on Apple clang 21.

hailo-ollama-macos itself needed no source changes. Two things differ from its
`scripts/macos/build_hailo_ollama.sh`:

- The script looks for the runtime in a sibling `hailort-macos/`, but the repo is
  `hailort-macos-usb`. Set `HAILORT_ROOT=../hailort-macos-usb`. On this Mac,
  `~/Documents/Claude/hailort-macos` is an old 5.4.0 scratch clone with no USB build.
  Without the variable, the script stops with "HailoRT build not found".
- The script builds with every core. I ran its steps by hand with `--parallel 4`, since
  the Mac has 8 GB of RAM. Configuring took about 15 s, including fetching oatpp,
  eventpp and nlohmann-json. The build took about 80 s and used 172 MB of disk.

```bash
cd ~/Documents/Claude/hailo-ollama-macos
HI=../hailort-macos-usb/build-macos-usb/hailort/libhailort/src/cmake_install.cmake
S=$PWD/build-macos/hailort-prefix
cmake -DCMAKE_INSTALL_PREFIX=$S -DCOMPONENT=Unspecified -P $HI
cmake -DCMAKE_INSTALL_PREFIX=$S -DCOMPONENT=libhailort  -P $HI
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$S \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=@loader_path/../lib \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build-macos --target hailo-ollama --parallel 4
cmake --install build-macos --prefix build-macos/install
cp $S/lib/libhailort.5.3.0.dylib build-macos/install/lib/
ln -sfn libhailort.5.3.0.dylib build-macos/install/lib/libhailort.dylib
```

The binary finds `libhailort` through `@loader_path/../lib`.

HailoRT's own `llm_benchmark` example is a single file on top of the existing build:
`cmake --build build-macos-usb --target llm_benchmark --parallel 4`.

## Model

Each hailo-ollama manifest names its HEF by SHA-256 (`hef_h10h`). `/api/pull` downloads it
from `https://dev-public.hailo.ai/blob/sha256_<hash>`. That host is Hailo's public model
library and needs no login. I fetched the file with curl so the download could resume:

| Model | HEF size | SHA-256 |
|---|---:|---|
| Qwen3-1.7B-Instruct (A8W4, HAILO10H, HailoRT 5.3.0) | 2.88 GB | `cc9b9d1c92e35249b5a9b7bc31fbd652f03bba1232e99b9a8271845ad6f17821` |

```bash
curl -L -C - -o ../hailort-macos-usb/models/Qwen3-1.7B-Instruct.hef \
  https://dev-public.hailo.ai/blob/sha256_cc9b9d1c92e35249b5a9b7bc31fbd652f03bba1232e99b9a8271845ad6f17821
scripts/macos/link_local_model.sh qwen3 ../hailort-macos-usb/models/Qwen3-1.7B-Instruct.hef
```

The link script checks the hash and symlinks the file into `runtime/`, so nothing is
copied. The other manifests (`qwen2.5:1.5b`, `qwen2:1.5b`, `qwen2.5-coder:1.5b`,
`llama3.2:1b`, `deepseek_r1:1.5b`) work the same way.

## Run

```bash
~/Documents/Claude/ugen300-loader/ugen300-loader probe        # rfs-upload first if Mode: loader
cd ~/Documents/Claude/hailo-ollama-macos && scripts/macos/run_hailo_ollama.sh
```

The server listens on `127.0.0.1:8000` and has no auth. `/api/version` reports `0.5.1`.
Only one process can own the device at a time, so stop the server before running
`llm_benchmark` or `hailortcli`. If the stick is replugged, it comes back in loader mode.
Run `rfs-upload` again, then restart the server, because the old process keeps answering
HTTP but generation fails with `HAILO_DEVICE_NOT_CONNECTED`.

## Transcript

Unlike real Ollama, hailo-ollama needs `-H 'Content-Type: application/json'`. Without the
header, `curl -d` sends a form content type and the server returns HTTP 500 with
`No suitable mapper found to deserialize the request body`.

```text
$ H='Content-Type: application/json'
$ curl -s http://127.0.0.1:8000/api/generate -H "$H" -d '{"model":"qwen3"}'     # explicit load
{"model":"qwen3","created_at":"2026-10-16T22:31:11.276541000Z","response":"","done":true,"done_reason":"load"}
[HTTP 200, wall 8.13 s]

$ curl -s http://127.0.0.1:8000/api/generate -H "$H" -d '{"model":"qwen3","prompt":"Why is the sky blue? Answer in two sentences.","stream":false,"options":{"num_predict":128}}'
{"model":"qwen3","created_at":"2026-10-16T22:31:32.962834000Z","response":"The sky is blue because of Rayleigh scattering of sunlight by the molecules in the atmosphere. This scattering is more intense for shorter wavelengths of light, such as blue and violet, which is why the sky appears blue to our eyes.","done":true,"done_reason":"stop","total_duration":12938522333,"eval_count":47}
[HTTP 200, wall 12.95 s]

$ curl -s http://127.0.0.1:8000/api/chat -H "$H" -d '{"model":"qwen3","messages":[{"role":"user","content":"Name three uses for a Raspberry Pi. One line each."}],"stream":false,"options":{"num_predict":128}}'
{"model":"qwen3","created_at":"2026-10-16T22:31:39.359562000Z","message":{"role":"assistant","content":"1. Run a home server  \n2. Build a DIY robot  \n3. Set up a media center"},"done":true,"done_reason":"stop","total_duration":6341267209,"eval_count":22}
[HTTP 200, wall 6.36 s]
```

## Numbers

hailo-ollama fills in only `total_duration` and `eval_count`. It sends no `load_duration`
or `prompt_eval_*`, so TTFT and decode rate come from client-side stream timing with
[`scripts/ollama-bench.py`](../scripts/ollama-bench.py). All runs used greedy decoding.

| Metric | hailo-ollama over HTTP | `llm_benchmark` (direct) |
|---|---:|---:|
| Model load (2.88 GB HEF) | 8.13 s | 7.75 s, 7.85 s |
| TTFT, first request with the model not loaded | 8.58 s (load + prefill) | |
| TTFT, short prompt | 0.843–0.861 s (5 runs) | 0.819 s (2 runs) |
| Decode | **3.80 tok/s** (5 runs, 117–200 tokens) | 3.81–3.82 tok/s |

- The HTTP server costs about 25 ms of TTFT and nothing measurable in decode. The device
  sets the speed.
- Decode stays at 3.80 tok/s for both `/api/generate` and `/api/chat`. `eval_count`
  equals `num_predict` on capped runs, so one streamed chunk is one token.
- Prefill is about 650 characters/s, roughly 140–160 tok/s. TTFT was 0.86 s for a
  207-character prompt, 3.25 s for 1,362 characters and 7.30 s for 4,002 characters.
- In multi-turn chat the server keeps the KV cache when the new message list extends the
  previous one. A follow-up turn after a 1,360-character first turn had a TTFT of 0.83 s,
  against 3.26 s for the first turn.
- The 47-token answer in the transcript took 12.9 s and the 22-token answer took 6.3 s.

### Is it throttled?

Probably, by about 20%, but that isn't proven. On every device open, HailoRT logs:

```text
[device_hrpc_client.cpp:65] [create] USB-C source electrical current advertised: 1.5A.
The module may run in reduced-performance mode. For full performance, use a USB-C port
that advertises 3.0A.
```

- relaxcloud's M2 Ultra baseline was also on a 1.5 A port: 7.90 s load, 0.816 s TTFT and
  3.84 reads/s. That is the same as here, so the Mac's CPU doesn't matter.
- A third-party table ([codesota](https://www.codesota.com/embedded-ai/hailo-10h-llms))
  lists this HEF at 4.78 tok/s with a 620 ms TTFT. We get 80% of that decode rate and a
  TTFT 37% longer. Hailo itself doesn't publish this figure, and the table's methodology
  is unknown.
- To settle it, run the same `llm_benchmark` command through a powered USB-C hub or dock
  that advertises 3.0 A downstream, and check that the warning goes away.

For speed rather than Qwen3, the same table lists Qwen2-1.5B at about 9.5 tok/s on
Hailo-10H. The `qwen2:1.5b` and `qwen2.5:1.5b` manifests are already installed and only
need their HEFs (2.46 GB for qwen2.5).

# Ollama API on the UGen300, natively on macOS

Stock Ollama can't drive a Hailo NPU. Hailo's Ollama-compatible server, `hailo-ollama`, can,
and [relaxcloud-cn/hailo-ollama-macos](https://github.com/relaxcloud-cn/hailo-ollama-macos)
ports it to Apple Silicon. **Follow its README to build, register a model and run.** This page
only adds what that README doesn't cover. The main addition is that with `ugen300-loader`
the whole chain runs with no Linux VM:

```text
curl / Ollama client / OpenAI SDK
  └─ hailo-ollama (127.0.0.1:8000, macOS)
      └─ libhailort 5.3.0 (hailort-macos-usb), HRPC over libusb
          └─ UGen300 runtime, booted by `ugen300-loader rfs-upload` (replaces the loader VM)
```

Tested 2026-10-16 on a MacBook (A18 Pro, 8 GB, macOS 27). On that machine only one USB-C
port carries USB 3. The other is USB 2 only, and there the stick gets power but never
enumerates.

## Build notes

- `scripts/macos/build_hailo_ollama.sh` defaults `HAILORT_ROOT` to a sibling named
  `hailort-macos/`, but the runtime repo is `hailort-macos-usb`. Set
  `HAILORT_ROOT=../hailort-macos-usb`, or the script stops with "HailoRT build not found".
- hailort-macos-usb needs `#include <fstream>` in `hailort/common/file_utils.hpp` to build
  with Apple clang 21 (proposed upstream in relaxcloud-cn/hailort-macos-usb#1).
- On an 8 GB Mac, build with `--parallel 4` rather than every core. hailo-ollama itself
  needed no source changes.

## Getting a model without a login

Each hailo-ollama manifest names its HEF by SHA-256 (`hef_h10h`). `/api/pull` downloads it
from Hailo's public model library at `https://dev-public.hailo.ai/blob/sha256_<hash>`,
which needs no account. To make the 2–3 GB download resumable, fetch it with curl and
register it with the repo's link script:

```bash
curl -L -C - -o Qwen3-1.7B-Instruct.hef \
  https://dev-public.hailo.ai/blob/sha256_cc9b9d1c92e35249b5a9b7bc31fbd652f03bba1232e99b9a8271845ad6f17821
scripts/macos/link_local_model.sh qwen3 /path/to/Qwen3-1.7B-Instruct.hef
```

That is Qwen3-1.7B-Instruct (A8W4, HAILO10H, HailoRT 5.3.0), 2.88 GB. The other manifests
(`qwen2.5:1.5b`, `qwen2:1.5b`, `qwen2.5-coder:1.5b`, `llama3.2:1b`, `deepseek_r1:1.5b`) work
the same way.

Unlike real Ollama, hailo-ollama rejects requests without `Content-Type: application/json`.
It returns HTTP 500 with "No suitable mapper found to deserialize the request body", which
is what plain `curl -d` gets.

## Numbers

hailo-ollama reports only `total_duration` and `eval_count`, so TTFT and decode rate come
from timing the stream on the client with [`scripts/ollama-bench.py`](../scripts/ollama-bench.py).
These are Qwen3-1.7B results with greedy decoding:

| Metric | hailo-ollama over HTTP | HailoRT `llm_benchmark` (direct) |
|---|---:|---:|
| Model load (2.88 GB HEF) | 8.13 s | 7.75–7.85 s |
| TTFT, short prompt | 0.84–0.86 s (5 runs) | 0.82 s |
| Decode | **3.80 tok/s** (5 runs, 117–200 tokens) | 3.81–3.82 tok/s |

- relaxcloud's M2 Ultra baseline is 7.90 s load, 0.816 s TTFT and 3.84 reads/s. An 8 GB A18
  Pro gets the same, so **the stick sets the speed, not the Mac**. The HTTP server adds about
  25 ms to TTFT.
- Prefill runs at about 650 characters/s, roughly 140–160 tok/s. TTFT was 3.25 s for 1,362
  characters and 7.30 s for 4,002 characters.
- Multi-turn chat reuses the KV cache when the new message list extends the previous one.
  A follow-up turn had a TTFT of 0.83 s, against 3.26 s for the long first turn.

### Is it throttled?

Probably, by about 20%, but that isn't proven. Both test setups were on USB-C ports that
advertise 1.5 A, and HailoRT warns on every open: "The module may run in reduced-performance
mode. For full performance, use a USB-C port that advertises 3.0A." A third-party table
([codesota](https://www.codesota.com/embedded-ai/hailo-10h-llms)) lists this HEF at 4.78 tok/s
with a 620 ms TTFT. Its methodology is unknown, and Hailo doesn't publish the figure. A
`llm_benchmark` run through a powered hub or dock that advertises 3 A downstream would settle
it. If you have one, please open an issue with your numbers.

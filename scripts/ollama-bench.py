#!/usr/bin/env python3
"""Client-side timing for an Ollama-API server: TTFT and decode tokens/s from a stream.

Usage: ollama-bench.py <generate|chat> <prompt> [num_predict] [model] [base_url]

hailo-ollama only reports total_duration and eval_count, so TTFT has to be measured here.
Each streamed NDJSON chunk is one HailoRT completion read, which is one token in practice
(eval_count == num_predict when done_reason is "length").
"""
import json
import sys
import time
import urllib.request

mode = sys.argv[1]
prompt = sys.argv[2]
num_predict = int(sys.argv[3]) if len(sys.argv) > 3 else 128
model = sys.argv[4] if len(sys.argv) > 4 else "qwen3"
base = sys.argv[5] if len(sys.argv) > 5 else "http://127.0.0.1:8000"

body = {"model": model, "stream": True, "options": {"num_predict": num_predict, "temperature": 0}}
if mode == "generate":
    body["prompt"] = prompt
else:
    body["messages"] = [{"role": "user", "content": prompt}]

req = urllib.request.Request(f"{base}/api/{mode}", data=json.dumps(body).encode(),
                             headers={"Content-Type": "application/json"})
t0 = time.perf_counter()
first = None
last = None
chunks = 0
text = []
final = {}
with urllib.request.urlopen(req, timeout=600) as resp:
    for line in resp:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        piece = msg.get("response") if mode == "generate" else (msg.get("message") or {}).get("content")
        now = time.perf_counter()
        if msg.get("done"):
            final = msg
            last = last or now
            continue
        if piece is None:
            continue
        if first is None:
            first = now
        last = now
        chunks += 1
        text.append(piece)
t_end = time.perf_counter()

ttft = (first - t0) if first else float("nan")
decode = (last - first) if first and last else float("nan")
tps = (chunks - 1) / decode if decode and decode > 0 else float("nan")
print(f"TTFT_s={ttft:.3f} chunks={chunks} eval_count={final.get('eval_count')} "
      f"decode_s={decode:.3f} decode_tok_s={tps:.2f} wall_s={t_end - t0:.3f} "
      f"done_reason={final.get('done_reason')} server_total_s={(final.get('total_duration') or 0) / 1e9:.3f}")
print("TEXT:", "".join(text).replace("\n", "\\n")[:600])

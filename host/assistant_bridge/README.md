# PSE84 Voice Assistant — Host Bridge (Phase 5)

macOS-side Python daemon that speaks the L2CAP CoC framing defined in the
master plan and drives the Whisper → Ollama → TEXT_CHUNK pipeline.

Wire format (little-endian, bidirectional over a single SPSM):

```
| u8 type | u8 seq | u16 len | payload[len] |
```

Types: `0x01 AUDIO` (Opus), `0x10 CTRL_STATE`, `0x11 CTRL_START_LISTEN`,
`0x12 CTRL_STOP_LISTEN`, `0x20 TEXT_CHUNK`, `0x21 TEXT_END`.

## Install

Use the project's `zephyr-env` pyenv virtualenv — the Python on the system
path does not have PyObjC, which CoreBluetooth needs on macOS.

```bash
PY=~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3
$PY -m pip install -r host/assistant_bridge/requirements.txt
```

`opuslib` needs the native `libopus` shared library:

```bash
brew install opus   # macOS
```

`faster-whisper` pulls the CTranslate2 runtime; the first `--real` run
downloads the model (~150 MB for `base.en`).

## Run

### file-inject transport (no hardware)

Feeds a local 16 kHz mono WAV through the same pipeline the BLE transport
will use. Prints every outbound frame to stdout.

```bash
$PY -m assistant_bridge.bridge \
    --transport=file-inject \
    --input=host/assistant_bridge/tests/fixtures/hello.wav \
    --ollama-url=http://localhost:11434 \
    --dry-run-llm
```

`--dry-run-llm` returns a canned reply so you can verify framing without an
Ollama server. Drop the flag and point `--ollama-url` at a reachable server
to run the real LLM path:

```bash
export OLLAMA_URL=http://gpu-box.local:11434
$PY -m assistant_bridge.bridge \
    --transport=file-inject \
    --input=some_recording.wav \
    --ollama-model=glm-4.7 \
    --real
```

Sanity-check Ollama with curl first:

```bash
curl -s "$OLLAMA_URL/api/chat" \
    -d '{"model":"glm-4.7","messages":[{"role":"user","content":"hi"}],"stream":true}' \
    | head
```

### bleak transport (requires hardware)

Real L2CAP CoC client. Pass the peripheral's address:

```bash
$PY -m assistant_bridge.bridge \
    --transport=bleak \
    --bleak-address=XX:XX:XX:XX:XX:XX \
    --ollama-url=http://localhost:11434
```

The bleak transport's exact byte-stream handshake is not finalized in this
PoC — `bleak` exposes L2CAP CoC via CoreBluetooth on macOS, but the final
`openL2CAPChannel` wiring needs a live PSE84 kit to validate. The control
flow scaffold is in `bridge.py::_run_bleak`; once Phase 4 lands an
advertising device, wiring is a one-file change.

## Layout

```
host/assistant_bridge/
├── __init__.py
├── bridge.py          # CLI + transport glue
├── framing.py         # wire codec (encode/decode/StreamingFrameParser)
├── pipeline.py        # Opus → PCM → Whisper → Ollama → TEXT_CHUNK
├── requirements.txt
├── README.md
├── conftest.py        # pytest: puts host/ on sys.path
└── tests/
    ├── test_framing.py
    ├── test_pipeline.py
    └── fixtures/
        └── hello.wav  # 300 ms: 100 ms silence + 200 ms 1 kHz tone, 16 kHz mono
```

## Tests

```bash
cd host
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 -m pytest assistant_bridge/tests/ -v
# with coverage:
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 -m pytest assistant_bridge/tests/ \
    --cov=assistant_bridge.framing --cov=assistant_bridge.pipeline --cov-report=term-missing
```

Tests use mock Opus / Whisper / Ollama so they run offline with zero native
deps beyond `httpx` and `pytest`.

Current coverage: framing 100%, pipeline 90%. Uncovered pipeline lines are
the thin wrappers around real `opuslib` / `faster-whisper` — they're
integration paths that only execute with `--real` against hardware.

## Regenerating the fixture

```python
import math, struct, wave
sr = 16000
samples = [0]*int(0.1*sr) + [
    int(0.3*32767*math.sin(2*math.pi*1000*n/sr))
    for n in range(int(0.2*sr))
]
with wave.open("host/assistant_bridge/tests/fixtures/hello.wav","wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b"".join(struct.pack("<h", s) for s in samples))
```

## Outstanding hardware work

Things this PoC cannot finalize without a live PSE84 kit:

* **bleak CoC byte-for-byte validation** against the M33 Zephyr BT host
  (Phase 4). Specifically: PSM discovery vs fixed PSM, MTU negotiation,
  and whether macOS CoreBluetooth accepts the advertising payload.
* **Opus frame-boundary alignment** with the on-device encoder — we assume
  one Opus packet per AUDIO frame at 20 ms / 16 kHz, which matches the
  master plan, but the real device's encoder output size range needs to be
  measured.
* **End-to-end latency** (button release → first TEXT_CHUNK on display).
  Master plan target is ≤ 2 s; this can only be measured with the full
  loop wired.

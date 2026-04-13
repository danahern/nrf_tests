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

### uart transport (PSE84 over USB-UART — Phase 2 bridge)

End-to-end: **hold sw0, speak, LLM replies on Mac.**

This wires Phase 2's `PCM_BEGIN / <hex> / PCM_END` UART protocol into
the same pipeline. Raw PCM is fed directly to faster-whisper (no Opus on
this path), the transcript is sent to Ollama, and reply tokens stream to
stdout as they arrive.

Prereqs:

1. PSE84 `pse84_assistant` firmware flashed, USB-UART enumerated as
   `/dev/cu.usbmodem*` on macOS.
2. Reachable Ollama server, e.g. `http://192.168.1.129:11434` with the
   `glm-4.7` model pulled.
3. `pyserial` + `faster-whisper` installed (see `requirements.txt`).

Smoke test the Ollama endpoint first:

```bash
curl -s http://192.168.1.129:11434/api/tags | jq '.models[].name'
```

Run the bridge (the firmware autosends captures while `sw0` is held):

```bash
PY=~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3
export OLLAMA_URL=http://192.168.1.129:11434

$PY -m assistant_bridge.bridge \
    --transport=uart \
    --ollama-url=$OLLAMA_URL \
    --ollama-model=glm-4.7 \
    --real
```

Flags:

| flag | default | notes |
|------|---------|-------|
| `--uart-port` | auto (glob) | Explicit serial device; overrides glob. |
| `--uart-port-glob` | `/dev/cu.usbmodem*` | Glob when `--uart-port` is unset. |
| `--uart-baud` | `460800` | Must match the device firmware. |
| `--ollama-url` | `http://192.168.1.129:11434` | Or `$OLLAMA_URL`. |
| `--ollama-model` | `glm-4.7` | Any Ollama-pulled model. |
| `--system-prompt` | short voice-assistant preamble | Sent as the `system` role on every turn. |
| `--dry-run-llm` | off | Skip Ollama, return a canned reply (for wire-bring-up). |
| `--real` | off | Use real faster-whisper (vs. the fake transcriber). |

Expected output on each button-release capture:

```
uart transport listening on /dev/cu.usbmodem0001 @ 460800 baud (Ollama: http://192.168.1.129:11434 model=glm-4.7)
[uart] captured 48000 samples (3000 ms, sr=16000)
Paris is the capital of France.
[uart] transcript='what is the capital of france' reply_len=30
```

Troubleshooting:

* **`no serial device matched glob`** — the kit isn't enumerated, or the
  port name changed. Run `ls /dev/cu.usbmodem*` to confirm, then pass
  `--uart-port` explicitly. See `project_pse84_psram.md` for the typical
  port names.
* **Garbage hex / `expected X hex chars`** — baud mismatch. The firmware
  baud is pinned in `zephyr_workspace/pse84_assistant/boards/*.overlay`;
  the default here (460800) matches the `baud_fix` commit.
* **Whisper downloads model on first run** — expected; ~150 MB for
  `base.en`, cached under `~/.cache/huggingface`.

### mac-mic transport (macOS Core Audio, no device needed)

Fallback / demo path when on-device PDM is unavailable. Uses the Mac's
built-in microphone (or any `sounddevice` input device). SPACE toggles
recording on/off; 8 s max per turn; ESC or Ctrl-C exits.

```bash
$PY -m assistant_bridge.bridge \
    --transport=mac-mic \
    --ollama-url=http://192.168.1.129:11434 \
    --real
```

Options:

- `--mac-mic-device=<name|index>` — pick a specific input device. List with:
  ```bash
  $PY -c 'import sounddevice; print(sounddevice.query_devices())'
  ```
- `--mac-mic-max-seconds` — hard cap per utterance (default 8 s).
- `--mac-mic-sample-rate` — capture rate (default 16000 to match device).

Troubleshooting:

- First run prompts for microphone permission in macOS System Settings.
- "No module named 'sounddevice'" → `pip install -r requirements.txt`.
- No audio even after granting permission → verify input level in
  `sounddevice.query_devices()` shows a non-zero "in" count on the device
  you expect.

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

# EMO local brain

A fully local voice assistant for the EMO desk robot, running on one Windows desktop with a 16 GB GPU.
The robot streams its microphone to the PC. The PC turns speech into text, decides what to say and do,
and sends EMO's voice back to the robot's speaker.

```
robot mic ──TCP 7007──► speech-to-text ──► Gemma 4 (llama.cpp) ──► Pocket TTS + robot voice styles ──► robot speaker
                        Qwen3-ASR + VAD        │  tools
                                               ├─ robot: walk, turn, dances, animations, volume
                                               ├─ ask_claude: hard questions + web search (optional)
                                               ├─ Philips Hue lights
                                               ├─ Spotify (go-librespot, plays on the PC)
                                               └─ PC control (MCP server + agent on each PC)
```

## What is in here

| Path | What it does |
|---|---|
| `brain/emo_local.py` | The main server: robot audio in, reply out, tools, follow-up listening |
| `brain/stt.py` | Qwen3-ASR 1.7B/0.6B + Silero VAD, streaming partials, language ID limited to en/it/de |
| `brain/llm.py` | llama-server process + streaming chat with tool calls |
| `brain/speech_out.py` | Sentence-by-sentence TTS streaming to the robot |
| `brain/stream_auth.py` | Key check on port 7007 (HMAC challenge); `brain/emoa_auth_esp32.c` is the firmware side |
| `brain/robot_tools.py`, `emo_robot.py`, `emo_catalog.py` | Robot HTTP API and its movement/animation catalogue |
| `brain/claude_helper.py` | `ask_claude`: Claude with web search for questions Gemma should not guess |
| `brain/hue.py`, `hue_pair.*` | Philips Hue (CLIP v2), pairing script |
| `brain/spotify.py`, `spotify/setup_spotify.*` | Spotify through go-librespot's REST API |
| `brain/pc/` | PC control: `pc_mcp.py` (MCP server), `pc_agent.py` + tray app and installer for each PC |
| `brain/mcp_bridge.py` | Gives Gemma the tools of an MCP server |
| `brain/bench/` | Noise robustness benchmark for the speech-to-text, VRAM fit check |
| `tts_server.py`, `robot_styles.py`, `robot_fx.py` | TTS server (Pocket TTS / XTTS / Chatterbox) and 97 "small robot" voice styles |
| `pocket/` | Pocket TTS setup (CPU venv), voice samples |
| `docs/voice-clone-xtts.md` | Older notes: cloning and fine-tuning the robot's stock voice with XTTS |

## Requirements

- Windows 11, Python 3.12, a 16 GB GPU (developed on an RX 9070 XT with ROCm PyTorch; CUDA works the same)
- ~12 GB of disk for models (Gemma 4 12B Q4 GGUF, Qwen3-ASR)
- The robot running the replacement firmware that streams EMOA audio to port 7007

## Setup

```bat
:: 1. Python environment (see docs/voice-clone-xtts.md for the ROCm torch lines)
py -3.12 -m venv .venv
.venv\Scripts\pip install -r requirements-desktop.txt

:: 2. Models + llama.cpp (asks where to put the models, writes brain\paths.json)
brain\setup_brain.bat

:: 3. Pocket TTS (CPU environment for the voice)
pocket\setup_pocket.bat

:: 4. Optional extras
::    Claude helper: put an Anthropic API key in brain\anthropic_key.txt (or set ANTHROPIC_API_KEY)
::    Philips Hue:   brain\hue_pair.bat
::    Spotify:       brain\spotify\setup_spotify.bat   (Premium account)
::    PC control:    brain\pc\"Install EMO PC Agent.bat" on every PC to control

:: 5. Run
brain\start_emo.bat --style 28 --city <your city> --country <ISO code>
brain\stop_emo.bat
```

Useful options: `--style NN` (voice 01-97, also switchable by voice: "switch to voice 14"),
`--asr 0.6b`, `--tts-device cpu`, `--no-claude`, `--no-hue`, `--no-spotify`, `--no-pc`,
`--trust <ip>` / `--trust-lan` (who may stream without the key). `python brain\emo_local.py -h` lists all.

## Private files

These are created locally and are listed in `.gitignore`; never commit them:

| File | Contains |
|---|---|
| `brain/anthropic_key.txt` | Anthropic API key |
| `brain/hue_key.txt` | Hue bridge application keys |
| `brain/stream_key.txt` | Key the robot needs to stream to port 7007 |
| `brain/pc/pc_secret.txt` | Shared secret between the PC agents and the MCP server |
| `brain/spotify/config/state.json`, `spotify_app.txt` | Spotify login, Spotify developer app |
| `brain/utterances/` | Recordings of everything said to the robot |

## Third-party software and models (downloaded at setup, not included)

Gemma 4 (Gemma terms of use), Qwen3-ASR (Apache 2.0), Silero VAD (MIT), llama.cpp (MIT),
Pocket TTS by Kyutai (code MIT, weights CC-BY 4.0), go-librespot (GPL-3.0), MCP Python SDK (MIT).

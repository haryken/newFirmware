# EMO's own voice

The robot's stock assets contain about 115 spoken English lines in one consistent
young voice (`ttt_*`, `Fit_*`, `dice_pig_*`, `R_P_S_*`, `speak_tohome_*`,
`not_execute*`, `tictactoe_*`, `Spray_paint_*` …), roughly three minutes of clean
speech. `clips.tsv` lists them with transcripts (Whisper large-v3-turbo, hand
checked). That is enough to clone the voice zero-shot and to fine-tune a model on
it. The laptop (Snapdragon, no PyTorch for voice models) only does STT and the
assistant; the TTS runs here on the desktop and the laptop calls it over the LAN.

## 1. Desktop setup (Windows or Linux, RX 9070 XT = gfx1201)

```bat
py -3.12 -m venv .venv            (Linux: python3.12 -m venv .venv)
.venv\Scripts\activate            (Linux: source .venv/bin/activate)

python -m pip install --index-url https://stable.repo.amd.com/rocm/whl-next/ ^
    "torch[device-gfx1201]==2.14.0+rocm10.1.0" ^
    "torchvision[device-gfx1201]==0.29.0a0+rocm10.1.0" ^
    "torchaudio==2.11.0.3+rocm10.1.0"

pip install -r requirements-desktop.txt
python -c "import torch; print(torch.cuda.is_available(), torch.cuda.get_device_name(0))"
```

The torch lines are AMD's current ones for gfx1201 (ROCm 10.1, Python 3.10–3.14,
same command on both OSes); if they have moved on, take the ones from
[AMD's install page](https://rocm.docs.amd.com/projects/ai-ecosystem/en/latest/frameworks/pytorch/install.html?fam=radeon&os=windows&pytorch-ver=2.14.0&i=pip&w=compute&gpu=rx-9070-xt&gfx=gfx1201)
with the RX 9070 XT selected. ROCm torch reports the GPU as `cuda`, so every
script just uses `cuda`. Known caveat: bf16 training on Windows ROCm has been
reported to converge worse than Linux on this exact card
([ROCm/TheRock#8095](https://github.com/ROCm/TheRock/issues/8095)); the fine-tune
here runs in fp32, and Linux is the safer choice if you have it. An NVIDIA card
works the same with the normal CUDA torch.

Copy the `spiffs/spiffs/mp3` folder from this repo next to the scripts (or point
`--mp3` at it).

## 2. Dataset

```bat
python prepare_dataset.py --mp3 ..\..\spiffs\spiffs\mp3 --out dataset
```

Writes `dataset/wavs/*.wav` (22.05 kHz, trimmed, normalised), `metadata.csv`
(LJSpeech format), `reference.wav` (the eight best lines joined, ~25 s) and
`reference_6s.wav` (XTTS conditioning length). Listen to `reference.wav`: that is
the voice everything else copies.

## 3. Zero-shot clone (no training, 2 minutes)

```bat
python tts_server.py --engine xtts --reference dataset\reference_6s.wav
```

Downloads XTTS v2 (~1.8 GB) once, computes the speaker latents from the
reference and serves `POST /tts` on port 8777. Test from any machine:

```bat
curl -X POST http://<desktop-ip>:8777/tts -H "Content-Type: application/json" ^
     -d "{\"text\":\"Hi, I am EMO. Shall we play a game?\",\"lang\":\"en\"}" -o test.wav
```

XTTS speaks 17 languages, Italian included, in the cloned voice (the reference is
English; the accent carries over, which suits a robot). `--engine chatterbox`
(`pip install chatterbox-tts`) is a second zero-shot engine to compare.

## 4. Fine-tune (optional, ~20–40 min on the 9070 XT)

Shortcut on Windows: double-click `tune.bat`. It does step 1 (venv, ROCm torch, requirements),
checks the GPU, runs the fine-tune below and then `make_samples.py`, which renders the same five
sentences with stock XTTS and the fine-tuned model into `samples/` for A/B listening. Progress goes
to `tune.log`; finished setup steps are skipped on re-runs. On the 9070 XT the 12 epochs take
about 15 minutes (eval loss 5.08 -> 3.57 on the first run).

Two things differ from the stock coqui recipe and are handled in the scripts:
* `audio_compat.py` lets coqui-tts run on torch >= 2.9 without `torchcodec` (whose Windows wheels
  need FFmpeg DLLs and an exact torch ABI match): it swaps `torchaudio.load` for soundfile.
* Checkpoints hold model weights only (~2 GB). The trainer's default also stores the AdamW state
  (~5.6 GB each, three at once during a best-model save), which filled a 25 GB-free C: drive.
  Runs therefore can't be resumed mid-way; just start again.

```bat
python finetune_xtts.py --dataset dataset --out run --epochs 12
```

Fine-tunes XTTS v2's GPT on the 115 lines (batch 2, grad-accum 16, lr 5e-6). It
writes test sentences (English and Italian) to `run/xtts_emo-*/test_audios/`
every eval so you can hear the progress in TensorBoard
(`tensorboard --logdir run`). More epochs than ~20 start to overfit on three
minutes of data: the voice gets closer but Italian and long sentences degrade.
Serve the result:

```bat
python tts_server.py --engine xtts --xtts-dir run\xtts_emo-<date> --reference dataset\reference_6s.wav
```

## Child voice (on by default)

`tts_server.py` raises pitch and formants after synthesis (`voice_fx.py`, Praat's PSOLA "Change
gender" via praat-parselmouth) so EMO sounds like a small child: the fine-tuned model speaks at
~180 Hz, the `medium` preset moves it to ~300 Hz with formants +18 % and a livelier pitch range.
It costs ~0.1 s per reply. `install_fx.bat` installs it into an existing `.venv` and renders all
presets of the A/B sentences into `samples\child\`.

```bat
python tts_server.py ... --child light|medium|strong|off   (default medium)
python tts_server.py ... --child medium --pitch 320 --formant 1.2 --pitch-range 1.4
```

Per request: `{"text": "...", "lang": "it", "fx": {"preset": "strong"}}` or `"fx": {"pitch": 280}`.

## Robot pet voice (Pocket TTS, CPU)

A different direction from the cloned voice: Kyutai's Pocket TTS (100M params, CPU, built-in
voices, weights CC-BY-4.0) plus `robot_fx.py`, which turns a clean voice into a cute little
desktop-pet robot:
tiny body (pitch ~380 Hz, formants +25 %), intonation snapped toward semitones (stepped, synth-like
melody), a short metallic comb "tin shell" resonance, a little ring-mod buzz and bit-crush grit,
and a toy-speaker EQ. Base voice `azelma` (the brightest, cleanest built-in voice); Italian uses
the Italian model's only built-in voice, `giovanni`, which the effect reshapes the same way.

It lives in its own venv, `pocket\.venv` (plain CPU torch: the ROCm build's CPU path made Pocket
~270x slower). Synthesis + effect take ~0.25 s per second of speech on the 5600X.

```bat
pocket\setup_pocket.bat          one-time: venv + pocket-tts + praat-parselmouth, renders pocket\voices\
pocket\robot_samples.bat         samples\robot\<raw|soft|pet|bot>_<voice>_<n>_<lang>.wav
pocket\serve_pocket.bat          server on :8777 with the "pet" preset (same /tts API as XTTS)
pocket\serve_pocket.bat --robot bot --voices en=jane,it=giovanni
```

Presets `soft` / `pet` (default) / `bot`; every knob can be overridden per request:
`{"text": "...", "robot": {"preset": "pet", "pitch": 420, "quantize": 1.0, "tin_mix": 0.4}}`.
See the docstring of `robot_fx.py` for what each knob does.

### 20 alternative styles

`robot_styles.py` holds 20 more styles (base voice + effect chain): vocoders, one-note Speak & Spell,
sped-up "critter" tape, autotune, warble, octave layers, 8-bit, metal shell, glitch... Samples are
in `samples\robot2\NN_*.wav`. Serve one with `pocket\serve_pocket.bat --style NN`, or per request
`{"text": "...", "style": "NN"}`.

## 5. Point the laptop at it

```bat
python emo_server.py --tts-url http://<desktop-ip>:8777
```

Every reply is synthesised on the desktop in EMO's voice (24 kHz, resampled to
48 kHz for the robot). If the desktop is unreachable the server falls back to the
Windows voice and says so in the console. `--tts-url` is also honoured by the
MCP `say` tool.

## Licences

XTTS v2 weights are under the Coqui Public Model License (non-commercial);
Chatterbox is MIT. The reference lines are living.ai's assets from your own
robot.

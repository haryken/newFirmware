#!/usr/bin/env python3
"""Fine-tune XTTS v2 on EMO's voice (dataset/ from prepare_dataset.py).

Adapted from the coqui-tts recipe recipes/ljspeech/xtts_v2/train_gpt_xtts.py for a tiny
dataset (~115 clips, ~3 min): small batch, few epochs, low learning rate. XTTS is already a
strong zero-shot cloner; this pass nudges its GPT toward EMO's timbre and prosody so the
voice no longer drifts between sentences.

    python finetune_xtts.py --dataset dataset --out run --epochs 12
    -> run/<run_name>/best_model.pth (+ config.json, vocab.json, dvae / mel stats copied in)
       serve it with:  python tts_server.py --engine xtts --xtts-dir run/<run_name>

GPU: AMD RX 9070 XT (gfx1201) with the ROCm torch build, or any CUDA GPU; 16 GB VRAM is
plenty for batch 2. On ROCm, if a kernel fails try PYTORCH_ROCM_ARCH=gfx1201 and
HSA_OVERRIDE_GFX_VERSION=12.0.1 in the environment (see README.md).
"""
import argparse
import glob
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))

import audio_compat  # noqa: E402,F401  (torchcodec-free audio loading; must run before TTS imports, also in dataloader workers)


def slim_checkpoints():
    """Save model weights only. The trainer's default also stores the AdamW state, which makes
    every checkpoint ~5.6 GB (and a best-model save briefly needs three of them on disk); weights
    alone are ~2 GB and are all tts_server.py needs. The cost: a run can't be resumed mid-way."""
    import trainer.io as tio
    full = tio.save_model

    def save_model(config, model, output_path, **kw):
        kw.update(optimizer=None, scheduler=None, scaler=None)
        return full(config, model, output_path, **kw)

    tio.save_model = save_model


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dataset", default=os.path.join(HERE, "dataset"))
    p.add_argument("--out", default=os.path.join(HERE, "run"))
    p.add_argument("--run-name", default="xtts_emo")
    p.add_argument("--epochs", type=int, default=12)
    p.add_argument("--batch-size", type=int, default=2)
    p.add_argument("--grad-accum", type=int, default=16)
    p.add_argument("--lr", type=float, default=5e-6)
    p.add_argument("--language", default="en", help="language of the dataset transcripts")
    p.add_argument("--workers", type=int, default=4)
    args = p.parse_args()

    import torch
    from trainer import Trainer, TrainerArgs
    from TTS.config.shared_configs import BaseDatasetConfig
    from TTS.tts.datasets import load_tts_samples
    from TTS.tts.configs.xtts_config import XttsAudioConfig
    from TTS.tts.layers.xtts.trainer.gpt_trainer import GPTArgs, GPTTrainer, GPTTrainerConfig
    from TTS.utils.manage import ModelManager

    print(f"torch {torch.__version__}, device: {torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU (slow)'}")

    dataset = BaseDatasetConfig(formatter="ljspeech", dataset_name="emo", path=args.dataset,
                                meta_file_train="metadata.csv", language=args.language)

    # Base XTTS v2 files (downloaded once from coqui/XTTS-v2 on Hugging Face).
    base = os.path.join(args.out, "XTTS_v2.0_original_model_files")
    os.makedirs(base, exist_ok=True)
    hf = "https://huggingface.co/coqui/XTTS-v2/resolve/main/"
    files = {"dvae.pth": hf + "dvae.pth", "mel_stats.pth": hf + "mel_stats.pth",
             "vocab.json": hf + "vocab.json", "model.pth": hf + "model.pth",
             "config.json": hf + "config.json", "speakers_xtts.pth": hf + "speakers_xtts.pth"}
    for name, url in files.items():
        if not os.path.exists(os.path.join(base, name)):
            print("downloading", name)
            ModelManager._download_model_files([url], base, progress_bar=True)

    ref = os.path.join(args.dataset, "reference_6s.wav")
    model_args = GPTArgs(
        max_conditioning_length=132300,        # 6 s
        min_conditioning_length=66150,         # 3 s
        debug_loading_failures=False,
        max_wav_length=255995,                 # ~11.6 s
        max_text_length=200,
        mel_norm_file=os.path.join(base, "mel_stats.pth"),
        dvae_checkpoint=os.path.join(base, "dvae.pth"),
        xtts_checkpoint=os.path.join(base, "model.pth"),
        tokenizer_file=os.path.join(base, "vocab.json"),
        gpt_num_audio_tokens=1026,
        gpt_start_audio_token=1024,
        gpt_stop_audio_token=1025,
        gpt_use_masking_gt_prompt_approach=True,
        gpt_use_perceiver_resampler=True,
    )
    audio = XttsAudioConfig(sample_rate=22050, dvae_sample_rate=22050, output_sample_rate=24000)
    config = GPTTrainerConfig(
        output_path=args.out,
        model_args=model_args,
        run_name=args.run_name,
        project_name="emo_voice",
        run_description="XTTS v2 fine-tune on EMO's stock voice lines",
        datasets=[dataset],
        dashboard_logger="tensorboard",
        logger_uri=None,
        audio=audio,
        epochs=args.epochs,
        batch_size=args.batch_size,
        batch_group_size=48,
        eval_batch_size=args.batch_size,
        num_loader_workers=args.workers,
        eval_split_max_size=16,
        eval_split_size=0.08,
        print_step=10,
        plot_step=50,
        log_model_step=100,
        save_step=200,
        save_n_checkpoints=1,
        save_checkpoints=False,                # only best_model*.pth (see slim_checkpoints)
        save_on_interrupt=False,
        print_eval=True,
        optimizer="AdamW",
        optimizer_wd_only_on_weights=True,
        optimizer_params={"betas": [0.9, 0.96], "eps": 1e-8, "weight_decay": 1e-2},
        lr=args.lr,
        lr_scheduler="MultiStepLR",
        lr_scheduler_params={"milestones": [10000, 20000], "gamma": 0.5, "last_epoch": -1},
        test_sentences=[
            {"text": "Hi, I'm EMO. It's nice to finally talk to you.", "speaker_wav": ref, "language": "en"},
            {"text": "Ciao, sono EMO. Oggi ho voglia di ballare.", "speaker_wav": ref, "language": "it"},
        ],
    )

    slim_checkpoints()
    model = GPTTrainer(config)
    train_samples, eval_samples = load_tts_samples(config, eval_split=True,
                                                   eval_split_max_size=config.eval_split_max_size,
                                                   eval_split_size=config.eval_split_size)
    trainer = Trainer(TrainerArgs(restore_path=None, skip_train_epoch=False, start_with_eval=True,
                                  grad_accum_steps=args.grad_accum),
                      config, output_path=args.out, model=model,
                      train_samples=train_samples, eval_samples=eval_samples)
    trainer.fit()

    # Make the run folder self-contained for tts_server.py --xtts-dir
    runs = sorted(glob.glob(os.path.join(args.out, args.run_name + "*")), key=os.path.getmtime)
    if runs:
        run_dir = runs[-1]
        for name in ("vocab.json", "dvae.pth", "mel_stats.pth", "speakers_xtts.pth"):
            dst = os.path.join(run_dir, name)
            if not os.path.exists(dst):
                shutil.copy(os.path.join(base, name), dst)
        print(f"\nfine-tuned model in {run_dir}\n"
              f"serve: python tts_server.py --engine xtts --xtts-dir \"{run_dir}\" --reference \"{ref}\"")


if __name__ == "__main__":
    main()

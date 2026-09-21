#!/usr/bin/env python3
"""Batch-distill A1 NAM teacher models into Daisy-sized Nano-ReLU students."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def run(command: list[str], *, log: Path | None = None) -> None:
    printable = " ".join(command)
    print(f"+ {printable}", flush=True)
    if log is None:
        subprocess.run(command, check=True)
        return
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as stream:
        stream.write(f"\n$ {printable}\n")
        stream.flush()
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            stream.write(line)
            stream.flush()
            sys.stdout.write(line)
            sys.stdout.flush()
        if process.wait() != 0:
            raise subprocess.CalledProcessError(process.returncode, command)


def model_config() -> dict:
    # Exact 842-weight topology proven on the Hothouse/Daisy Seed.
    return {
        "net": {
            "name": "WaveNet",
            "config": {
                "layers_configs": [
                    {
                        "input_size": 1,
                        "condition_size": 1,
                        "channels": 4,
                        "head_size": 2,
                        "kernel_size": 3,
                        "dilations": [1, 2, 4, 8, 16, 32, 64],
                        "activation": "ReLU",
                        "gated": False,
                        "head_bias": False,
                    },
                    {
                        "input_size": 4,
                        "condition_size": 1,
                        "channels": 2,
                        "head_size": 1,
                        "kernel_size": 3,
                        "dilations": [128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512],
                        "activation": "ReLU",
                        "gated": False,
                        "head_bias": True,
                    },
                ],
                "head_scale": 0.02,
            },
        },
        "optimizer": {"lr": 0.004},
        "lr_scheduler": {"class": "ExponentialLR", "kwargs": {"gamma": 0.993}},
    }


def data_config(input_wav: Path, target_wav: Path) -> dict:
    return {
        "train": {"start_seconds": None, "stop_seconds": -9.0, "ny": 8192},
        "validation": {"start_seconds": -9.0, "stop_seconds": None, "ny": None},
        "common": {
            "x_path": str(input_wav.resolve()),
            "y_path": str(target_wav.resolve()),
            "delay": 0,
        },
    }


def learning_config(epochs: int, checkpoint: Path | None) -> dict:
    accelerator = "mps" if sys.platform == "darwin" else "auto"
    fit_kwargs = {} if checkpoint is None else {"ckpt_path": str(checkpoint)}
    return {
        "train_dataloader": {
            "batch_size": 16,
            "shuffle": True,
            "pin_memory": True,
            "drop_last": True,
            "num_workers": 0,
        },
        "val_dataloader": {},
        "trainer": {
            "accelerator": accelerator,
            "devices": 1,
            "max_epochs": epochs,
            "enable_progress_bar": True,
        },
        "trainer_fit_kwargs": fit_kwargs,
    }


def validate_teacher(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("architecture") != "WaveNet":
        raise ValueError(f"Not an A1 WaveNet model: {path}")
    if not isinstance(value.get("weights"), list):
        raise ValueError(f"Missing weights: {path}")
    return value


def ensure_pcm16(path: Path) -> None:
    codec = subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=codec_name",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        text=True,
    ).strip()
    if codec == "pcm_s16le":
        return
    converted = path.with_name(f"{path.stem}.pcm16.wav")
    run(
        [
            "ffmpeg",
            "-y",
            "-v",
            "error",
            "-i",
            str(path),
            "-c:a",
            "pcm_s16le",
            str(converted),
        ]
    )
    os.replace(converted, path)


def enrich_student(student: Path, teacher: dict, teacher_path: Path) -> None:
    value = json.loads(student.read_text(encoding="utf-8"))
    metadata = dict(teacher.get("metadata") or {})
    old_name = metadata.get("name") or teacher_path.stem
    metadata["name"] = f"{old_name} - Nano-ReLU distillation"
    metadata["modeled_by"] = metadata.get("modeled_by") or "tone3000"
    metadata["distillation"] = {
        "teacher_sha256": hashlib.sha256(teacher_path.read_bytes()).hexdigest(),
        "teacher_architecture": teacher.get("architecture"),
        "student_topology": "Hothouse Nano-ReLU 842",
    }
    value["metadata"] = metadata
    write_json(student, value)


def validate_student(path: Path, namb_path: Path) -> None:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("architecture") != "WaveNet":
        raise ValueError("Student is not WaveNet")
    layers = value.get("config", {}).get("layers", [])
    activations = [layer.get("activation") for layer in layers]
    normalized = [a.get("type") if isinstance(a, dict) else a for a in activations]
    if normalized != ["ReLU", "ReLU"]:
        raise ValueError(f"Student activations are {normalized}, not ReLU/ReLU")
    if len(value.get("weights", [])) != 842:
        raise ValueError(f"Student has {len(value.get('weights', []))} weights, expected 842")
    size = namb_path.stat().st_size
    if size > 4096:
        raise ValueError(f"NAMB is {size} bytes; expected <=4096")


def newest_checkpoint(job: Path) -> Path | None:
    checkpoints = list((job / "training").glob("**/*.ckpt"))
    return max(checkpoints, key=lambda p: p.stat().st_mtime) if checkpoints else None


def newest_export(job: Path) -> Path:
    exports = list((job / "training").glob("**/model.nam"))
    if not exports:
        raise FileNotFoundError("nam-full completed without exporting model.nam")
    return max(exports, key=lambda p: p.stat().st_mtime)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--teachers", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--input-wav", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--trainer", type=Path, required=True)
    parser.add_argument("--nam2namb", type=Path, required=True)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()

    teachers = sorted(args.teachers.glob("**/*.nam"))
    if args.limit is not None:
        teachers = teachers[: args.limit]
    if not teachers:
        raise RuntimeError(f"No .nam teachers found under {args.teachers}")

    args.workspace.mkdir(parents=True, exist_ok=True)
    summary_path = args.workspace / "summary.json"
    completed = failed = 0

    for index, teacher_path in enumerate(teachers, 1):
        relative = teacher_path.relative_to(args.teachers)
        job = args.workspace / "jobs" / relative.parent / relative.stem
        job.mkdir(parents=True, exist_ok=True)
        done = job / "DONE"
        failed_marker = job / "FAILED"
        if done.exists():
            print(f"[{index}/{len(teachers)}] SKIP {relative}", flush=True)
            completed += 1
            continue

        print(f"[{index}/{len(teachers)}] DISTILL {relative}", flush=True)
        started = time.time()
        try:
            teacher = validate_teacher(teacher_path)
            teacher_namb = job / "teacher.namb"
            target_wav = job / "target.wav"
            student = job / "student-nano-relu.nam"
            student_namb = job / "student-nano-relu.namb"

            if not teacher_namb.exists():
                run([str(args.nam2namb), str(teacher_path), str(teacher_namb)])
            if not target_wav.exists():
                run([str(args.renderer), str(teacher_namb), str(args.input_wav), str(target_wav)])
            ensure_pcm16(target_wav)

            config_dir = job / "config"
            data_path = config_dir / "data.json"
            model_path = config_dir / "model.json"
            learning_path = config_dir / "learning.json"
            write_json(data_path, data_config(args.input_wav, target_wav))
            write_json(model_path, model_config())
            write_json(learning_path, learning_config(args.epochs, newest_checkpoint(job)))
            (job / "training").mkdir(exist_ok=True)

            run(
                [
                    str(args.python),
                    str(args.trainer),
                    str(data_path),
                    str(model_path),
                    str(learning_path),
                    str(job / "training"),
                ],
                log=job / "train.log",
            )
            shutil.copy2(newest_export(job), student)
            enrich_student(student, teacher, teacher_path)
            run([str(args.nam2namb), str(student), str(student_namb)])
            validate_student(student, student_namb)
            failed_marker.unlink(missing_ok=True)
            write_json(
                job / "status.json",
                {
                    "status": "complete",
                    "teacher": str(relative),
                    "epochs": args.epochs,
                    "student_namb_bytes": student_namb.stat().st_size,
                    "elapsed_seconds": round(time.time() - started, 1),
                },
            )
            done.touch()
            completed += 1
        except Exception as exc:
            failed += 1
            failed_marker.write_text(f"{type(exc).__name__}: {exc}\n", encoding="utf-8")
            print(f"FAILED {relative}: {exc}", file=sys.stderr, flush=True)

        write_json(
            summary_path,
            {
                "total": len(teachers),
                "completed": completed,
                "failed": failed,
                "remaining": len(teachers) - completed - failed,
                "last_update_epoch": int(time.time()),
            },
        )

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

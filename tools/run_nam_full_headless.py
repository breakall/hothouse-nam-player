#!/usr/bin/env python3
"""Run NAM's full trainer without interactive plots or timestamped directories."""

import argparse
import json
from pathlib import Path

from nam.train.full import main


parser = argparse.ArgumentParser()
parser.add_argument("data_config", type=Path)
parser.add_argument("model_config", type=Path)
parser.add_argument("learning_config", type=Path)
parser.add_argument("outdir", type=Path)
args = parser.parse_args()

args.outdir.mkdir(parents=True, exist_ok=True)
with args.data_config.open(encoding="utf-8") as stream:
    data = json.load(stream)
with args.model_config.open(encoding="utf-8") as stream:
    model = json.load(stream)
with args.learning_config.open(encoding="utf-8") as stream:
    learning = json.load(stream)

main(data, model, learning, args.outdir, no_show=True, make_plots=False)

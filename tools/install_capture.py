#!/usr/bin/env python3
"""Install a compatible NAM capture over the pedal's normal USB connection."""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import pathlib
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty
import zlib

from embed_a2_model import select_a2_lite, validate_model


ROOT = pathlib.Path(__file__).resolve().parents[1]
CHUNK_SIZE = 128
A1_LIMIT = 64 * 1024
A1_WEIGHT_COUNT = 842


class ProtocolError(RuntimeError):
    pass


class PedalPort:
    def __init__(self, path: str):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attributes = termios.tcgetattr(self.fd)
        attributes[4] = termios.B115200
        attributes[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attributes)
        self.buffer = bytearray()

    def close(self) -> None:
        os.close(self.fd)

    def request(self, command: str, timeout: float = 4.0) -> list[str]:
        payload = (command + "\n").encode("ascii")
        position = 0
        while position < len(payload):
            _, writable, _ = select.select([], [self.fd], [], timeout)
            if not writable:
                raise ProtocolError("USB write timed out")
            position += os.write(self.fd, payload[position:])

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                raw = bytes(self.buffer[:newline]).strip()
                del self.buffer[: newline + 1]
                if not raw.startswith(b"HNAM "):
                    continue
                line = raw.decode("ascii", errors="replace")
                fields = line.split()
                if len(fields) >= 3 and fields[1] == "ERR":
                    raise ProtocolError("Pedal rejected the request: " + " ".join(fields[2:]))
                if len(fields) < 3 or fields[1] != "OK":
                    raise ProtocolError(f"Unexpected pedal response: {line}")
                return fields[2:]
            readable, _, _ = select.select([self.fd], [], [], max(0.0, deadline - time.monotonic()))
            if readable:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if chunk:
                    self.buffer.extend(chunk)
        raise ProtocolError(f"Pedal did not respond to {command.split()[1]}")


def candidate_ports() -> list[str]:
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    ]
    return sorted({path for pattern in patterns for path in glob.glob(pattern)})


def connect(path: str | None) -> tuple[PedalPort, list[str]]:
    candidates = [path] if path else candidate_ports()
    if not candidates:
        raise ProtocolError("No USB serial device found. Connect the powered pedal with a data-capable USB cable.")
    failures: list[str] = []
    for candidate in candidates:
        port: PedalPort | None = None
        try:
            port = PedalPort(candidate)
            time.sleep(0.15)
            response = port.request("HNAM INFO", timeout=2.0)
            if len(response) < 5 or response[0] != "INFO":
                raise ProtocolError("not a Hothouse NAM Player")
            return port, response
        except (OSError, ProtocolError) as error:
            failures.append(f"{candidate}: {error}")
            if port is not None:
                port.close()
    raise ProtocolError("Could not find a responding Hothouse NAM Player:\n  " + "\n  ".join(failures))


def capture_name(path: pathlib.Path, document: dict | None = None) -> str:
    metadata = document.get("metadata", {}) if isinstance(document, dict) else {}
    raw = metadata.get("name") if isinstance(metadata, dict) else None
    name = str(raw or path.stem)
    name = "".join(character if 0x20 <= ord(character) <= 0x7E else " " for character in name)
    return " ".join(name.split())[:63] or "Unnamed capture"


def decode_name_hex(value: str) -> str:
    try:
        return bytes.fromhex(value).decode("ascii")
    except (ValueError, UnicodeDecodeError) as error:
        raise ProtocolError("Pedal returned an invalid capture name") from error


def find_converter(explicit: pathlib.Path | None) -> pathlib.Path:
    choices = [
        explicit,
        ROOT / "build/nam-binary-loader/nam2namb",
        ROOT / "nam-pedal/nam-binary-loader/build-release/nam2namb",
    ]
    for choice in choices:
        if choice is not None and choice.is_file() and os.access(choice, os.X_OK):
            return choice
    raise ProtocolError(
        "The A1 firmware needs NAMB data and nam2namb is not built. "
        "Build it using the commands in README.md or pass --converter PATH."
    )


def validate_a1_nano_relu(document: dict) -> None:
    if document.get("architecture") != "WaveNet":
        raise ProtocolError("This is not an A1 WaveNet capture.")
    try:
        sample_rate = float(document.get("sample_rate", 0.0))
    except (TypeError, ValueError) as error:
        raise ProtocolError("A1 capture sample rate is invalid.") from error
    if sample_rate != 48000.0:
        raise ProtocolError("A1 captures must be trained at 48 kHz.")
    layers = document.get("config", {}).get("layers")
    if not isinstance(layers, list) or len(layers) != 2:
        raise ProtocolError("This A1 capture is not the supported two-stack Nano topology.")
    expected = [
        {
            "input_size": 1, "condition_size": 1, "head_size": 2,
            "channels": 4, "kernel_size": 3,
            "dilations": [1, 2, 4, 8, 16, 32, 64],
            "gated": False, "head_bias": False,
        },
        {
            "input_size": 4, "condition_size": 1, "head_size": 1,
            "channels": 2, "kernel_size": 3,
            "dilations": [128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512],
            "gated": False, "head_bias": True,
        },
    ]
    for index, (layer, required) in enumerate(zip(layers, expected)):
        if not isinstance(layer, dict) or any(layer.get(key) != value
                                              for key, value in required.items()):
            raise ProtocolError(
                f"A1 layer stack {index} does not match the device-safe Nano topology."
            )
        activation = layer.get("activation")
        activation_type = activation.get("type") if isinstance(activation, dict) else activation
        if activation_type != "ReLU":
            raise ProtocolError(
                "This is a Nano capture, but it uses Tanh. The current A1 real-time "
                "profile supports Nano-ReLU captures."
            )
    weights = document.get("weights")
    if not isinstance(weights, list) or len(weights) != A1_WEIGHT_COUNT:
        count = len(weights) if isinstance(weights, list) else 0
        raise ProtocolError(
            f"This A1 capture has {count:,} weights; the device-safe topology has "
            f"{A1_WEIGHT_COUNT:,}."
        )
    try:
        finite_weights = all(math.isfinite(float(weight)) for weight in weights)
    except (TypeError, ValueError):
        finite_weights = False
    if not finite_weights:
        raise ProtocolError("A1 capture weights must all be finite numbers.")


def prepare_a1(path: pathlib.Path, converter: pathlib.Path | None) -> tuple[str, bytes]:
    if path.suffix.lower() == ".namb":
        payload = path.read_bytes()
        name = capture_name(path)
    elif path.suffix.lower() == ".nam":
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ProtocolError(f"Invalid NAM file: {error}") from error
        if not isinstance(document, dict):
            raise ProtocolError("Invalid NAM file: the root must be an object.")
        validate_a1_nano_relu(document)
        with tempfile.TemporaryDirectory(prefix="hothouse-nam-") as directory:
            output = pathlib.Path(directory) / "capture.namb"
            result = subprocess.run(
                [str(find_converter(converter)), str(path), str(output)],
                text=True,
                capture_output=True,
            )
            if result.returncode != 0:
                raise ProtocolError("nam2namb failed: " + (result.stderr.strip() or result.stdout.strip()))
            payload = output.read_bytes()
        name = capture_name(path, document)
    else:
        raise ProtocolError("A1 firmware accepts .nam or .namb files.")
    # NAMB's 0x4e414d42 magic is stored little-endian on disk.
    if not payload.startswith(b"BMAN"):
        raise ProtocolError("The converted file is not valid NAMB data.")
    if len(payload) > A1_LIMIT:
        raise ProtocolError(f"The A1 binary is {len(payload):,} bytes; the runtime limit is {A1_LIMIT:,} bytes.")
    return name, payload


def prepare_a2(path: pathlib.Path) -> tuple[str, bytes]:
    if path.suffix.lower() != ".nam":
        raise ProtocolError("A2-Lite firmware accepts .nam files.")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise ValueError("NAM root is not an object")
        model, _ = select_a2_lite(document)
        weights = validate_model(model)
    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as error:
        raise ProtocolError(f"Incompatible A2-Lite capture: {error}") from error
    return capture_name(path, document), struct.pack(f"<{len(weights)}f", *weights)


def prepare_capture(backend: str, path: pathlib.Path,
                    converter: pathlib.Path | None) -> tuple[str, str, bytes]:
    if backend == "a1_a2":
        if path.suffix.lower() == ".namb":
            name, payload = prepare_a1(path, converter)
            return "a1_namb", name, payload
        if path.suffix.lower() != ".nam":
            raise ProtocolError("Combined firmware accepts .nam or .namb files.")
        try:
            name, payload = prepare_a2(path)
            return "a2_weights_f32", name, payload
        except ProtocolError as a2_error:
            try:
                name, payload = prepare_a1(path, converter)
                return "a1_namb", name, payload
            except ProtocolError as a1_error:
                raise ProtocolError(
                    "Capture is not compatible with this pedal. "
                    f"A2-Lite check: {a2_error}. A1 Nano-ReLU check: {a1_error}"
                ) from a1_error

    adapters = {
        "a1_nano_relu": (
            "a1_namb", lambda: prepare_a1(path, converter),
        ),
        "a2_lite": (
            "a2_weights_f32", lambda: prepare_a2(path),
        ),
    }
    adapter = adapters.get(backend)
    if adapter is None:
        raise ProtocolError(f"Unsupported firmware backend reported by pedal: {backend}")
    capture_format, prepare = adapter
    name, payload = prepare()
    return capture_format, name, payload


def install(port: PedalPort, backend: str, path: pathlib.Path,
            converter: pathlib.Path | None, slot: str) -> None:
    capture_format, name, payload = prepare_capture(backend, path, converter)

    checksum = zlib.crc32(payload) & 0xFFFFFFFF
    encoded_name = name.encode("ascii").hex()
    port.request(
        f"HNAM BEGIN {slot} {capture_format} {len(payload)} {checksum:08x} {encoded_name}",
        timeout=20.0,
    )
    try:
        next_report = 10
        for offset in range(0, len(payload), CHUNK_SIZE):
            chunk = payload[offset : offset + CHUNK_SIZE]
            response = port.request(f"HNAM DATA {offset} {chunk.hex()}", timeout=5.0)
            expected = offset + len(chunk)
            if len(response) < 2 or response[0] != "DATA" or int(response[1]) != expected:
                raise ProtocolError("Pedal acknowledged an unexpected transfer offset")
            percent = expected * 100 // len(payload)
            if percent >= next_report or expected == len(payload):
                print(f"  {percent:3d}%  {expected:,}/{len(payload):,} bytes")
                next_report += 10
        response = port.request("HNAM COMMIT", timeout=15.0)
        if not response or response[0] != "COMMIT":
            raise ProtocolError("Pedal did not confirm the capture commit")
    except Exception:
        try:
            port.request("HNAM CANCEL", timeout=2.0)
        except Exception:
            pass
        raise
    print(f"Installed in slot {slot}: {name} "
          f"({backend}, {len(payload):,} bytes, CRC {checksum:08x})")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Install a compatible NAM capture without entering DFU mode."
    )
    parser.add_argument("capture", nargs="?", type=pathlib.Path,
                        help="A .nam or .namb capture")
    parser.add_argument("--slot", choices=("A", "B", "C"), default="A",
                        help="toggle slot to replace (default: A / UP)")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--list", action="store_true",
                         help="list all capture slots")
    actions.add_argument("--delete-slot", choices=("A", "B", "C"),
                         help="erase a capture slot")
    parser.add_argument("--port", help="USB serial path; normally detected automatically")
    parser.add_argument("--converter", type=pathlib.Path, help="Path to nam2namb for A1 .nam input")
    args = parser.parse_args()
    if args.capture is None and not args.list and args.delete_slot is None:
        parser.error("capture is required unless --list or --delete-slot is used")
    if args.capture is not None and (args.list or args.delete_slot is not None):
        parser.error("capture cannot be combined with --list or --delete-slot")
    if args.capture is not None and not args.capture.is_file():
        parser.error(f"capture not found: {args.capture}")

    port, info = connect(args.port)
    try:
        backend, source = info[1], info[2]
        print(f"Connected: {port.path} ({backend}, current source: {source})")
        if args.list:
            summary = port.request("HNAM SLOTS")
            print(" ".join(summary))
            for slot in ("A", "B", "C"):
                detail = port.request(f"HNAM SLOT {slot}")
                if len(detail) < 3 or detail[0] != "SLOT" or detail[1] != slot:
                    raise ProtocolError(f"Pedal returned invalid metadata for slot {slot}")
                if detail[2] == "installed" and len(detail) >= 7:
                    print(f"  {slot}: {decode_name_hex(detail[6])} "
                          f"({detail[3]}, {int(detail[4]):,} bytes, CRC {detail[5]})")
                else:
                    print(f"  {slot}: {detail[2]}")
        elif args.delete_slot is not None:
            print(" ".join(port.request(f"HNAM DELETE {args.delete_slot}", timeout=20.0)))
        else:
            install(port, backend, args.capture.resolve(), args.converter,
                    args.slot)
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProtocolError) as error:
        print(f"Capture install failed: {error}", file=sys.stderr)
        raise SystemExit(2)

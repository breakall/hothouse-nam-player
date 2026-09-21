#!/usr/bin/env python3
"""Install a compatible NAM capture over the pedal's normal USB connection."""

from __future__ import annotations

import argparse
import glob
import json
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


def prepare_a1(path: pathlib.Path, converter: pathlib.Path | None) -> tuple[str, bytes]:
    if path.suffix.lower() == ".namb":
        payload = path.read_bytes()
        name = capture_name(path)
    elif path.suffix.lower() == ".nam":
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ProtocolError(f"Invalid NAM file: {error}") from error
        if not isinstance(document, dict) or document.get("architecture") != "WaveNet":
            raise ProtocolError("A1 firmware accepts WaveNet A1 Nano-ReLU captures only.")
        layers = document.get("config", {}).get("layers", [])
        if not layers or any(layer.get("activation") != "ReLU" for layer in layers):
            raise ProtocolError("A1 firmware requires a ReLU capture; tanh and other activations exceed its DSP budget.")
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


def install(port: PedalPort, backend: str, path: pathlib.Path,
            converter: pathlib.Path | None) -> None:
    if backend == "a2_lite":
        name, payload = prepare_a2(path)
        capture_format = "a2_weights_f32"
    elif backend == "a1_nano_relu":
        name, payload = prepare_a1(path, converter)
        capture_format = "a1_namb"
    else:
        raise ProtocolError(f"Unsupported firmware backend reported by pedal: {backend}")

    checksum = zlib.crc32(payload) & 0xFFFFFFFF
    encoded_name = name.encode("ascii").hex()
    port.request(
        f"HNAM BEGIN {capture_format} {len(payload)} {checksum:08x} {encoded_name}",
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
            raise ProtocolError("Pedal did not confirm capture activation")
    except Exception:
        try:
            port.request("HNAM CANCEL", timeout=2.0)
        except Exception:
            pass
        raise
    print(f"Installed and activated: {name} ({backend}, {len(payload):,} bytes, CRC {checksum:08x})")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Install a compatible NAM capture without entering DFU mode."
    )
    parser.add_argument("capture", type=pathlib.Path, help="A .nam or .namb capture")
    parser.add_argument("--port", help="USB serial path; normally detected automatically")
    parser.add_argument("--converter", type=pathlib.Path, help="Path to nam2namb for A1 .nam input")
    args = parser.parse_args()
    if not args.capture.is_file():
        parser.error(f"capture not found: {args.capture}")

    port, info = connect(args.port)
    try:
        backend, source = info[1], info[2]
        print(f"Connected: {port.path} ({backend}, current source: {source})")
        install(port, backend, args.capture.resolve(), args.converter)
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProtocolError) as error:
        print(f"Capture install failed: {error}", file=sys.stderr)
        raise SystemExit(2)

#!/usr/bin/env python3
"""Inspect or persist the Hothouse NAM player's UP/DOWN reverb assignments."""

from __future__ import annotations

import argparse
import sys

from install_capture import PedalPort, ProtocolError, connect


def describe(response: list[str]) -> str:
    if response[:2] == ["REVERB", "LIST"]:
        return "Available reverbs: " + ", ".join(response[2:])
    if response[:2] == ["REVERB", "INFO"]:
        return "Toggle mapping: " + " ".join(response[2:])
    if response[:2] == ["REVERB", "MAP"]:
        return "Saved toggle mapping: " + " ".join(response[2:])
    return "Unexpected response: " + " ".join(response)


def request(port: PedalPort, command: str) -> None:
    print(describe(port.request(command)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial path; normally detected automatically")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="list firmware-built reverb engines")
    commands.add_parser("info", help="show the persisted UP/DOWN mapping")
    map_parser = commands.add_parser("map", help="persist an engine in a toggle position")
    map_parser.add_argument("position", choices=("up", "down"))
    map_parser.add_argument("engine", choices=("reverbsc", "dattorro", "fdn16", "hybrid"))
    args = parser.parse_args()

    port, info = connect(args.port)
    try:
        print(f"Connected: {port.path} ({info[1]})")
        if args.command == "list":
            request(port, "HNAM REVERB LIST")
        elif args.command == "info":
            request(port, "HNAM REVERB INFO")
        else:
            request(port, f"HNAM REVERB MAP {args.position.upper()} {args.engine}")
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProtocolError) as error:
        print(f"Reverb configuration failed: {error}", file=sys.stderr)
        raise SystemExit(2)

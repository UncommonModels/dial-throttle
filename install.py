#!/usr/bin/env python3
"""Cross-platform installer for Dial Throttle firmware."""

from __future__ import annotations

import argparse
import glob
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

PROJECT_DIR = Path(__file__).resolve().parent
SKETCH_DIR = PROJECT_DIR / "DialThrottle"
CONFIG_FILE = SKETCH_DIR / "config.h"
LOCAL_CLI_DIR = PROJECT_DIR / ".arduino-cli"
CLI_DOWNLOAD_BASE = "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest"
PROFILE = "m5dial"


def say(message: str) -> None:
    print(f"==> {message}")


def warn(message: str) -> None:
    print(f"Warning: {message}")


def fail(message: str) -> None:
    print(f"Error: {message}")
    raise SystemExit(1)


def ask_yes_no(prompt: str, default_yes: bool, auto_yes: bool, auto_result: bool | None = None) -> bool:
    if auto_yes:
        return default_yes if auto_result is None else auto_result

    suffix = "[Y/n]" if default_yes else "[y/N]"
    while True:
        raw = input(f"{prompt} {suffix}: ").strip().lower()
        if not raw:
            return default_yes
        if raw in {"y", "yes"}:
            return True
        if raw in {"n", "no"}:
            return False
        print("Please enter y or n.")


def run_cmd(cmd: list[str], check: bool = True, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=PROJECT_DIR,
        check=check,
        text=True,
        capture_output=capture,
    )


def command_works(cmd: list[str]) -> bool:
    try:
        run_cmd(cmd + ["version"], check=True, capture=True)
        return True
    except Exception:
        return False


def resolve_cli_command() -> list[str] | None:
    exe = "arduino-cli.exe" if platform.system() == "Windows" else "arduino-cli"
    local = LOCAL_CLI_DIR / exe
    if local.exists() and command_works([str(local)]):
        return [str(local)]

    found = shutil.which("arduino-cli")
    if found and command_works([found]):
        return [found]

    return None


def cli_download_url() -> str:
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in {"arm64", "aarch64"}
    if system == "Windows":
        return f"{CLI_DOWNLOAD_BASE}_Windows_64bit.zip"
    if system == "Darwin":
        return f"{CLI_DOWNLOAD_BASE}_macOS_{'ARM64' if arm else '64bit'}.tar.gz"
    if system == "Linux":
        if arm:
            return f"{CLI_DOWNLOAD_BASE}_Linux_ARM64.tar.gz"
        if machine.startswith("arm"):
            return f"{CLI_DOWNLOAD_BASE}_Linux_ARMv7.tar.gz"
        return f"{CLI_DOWNLOAD_BASE}_Linux_64bit.tar.gz"
    fail(f"Unsupported OS for automatic arduino-cli install: {system}")
    return ""


def ensure_arduino_cli(auto_yes: bool) -> list[str]:
    cmd = resolve_cli_command()
    if cmd is not None:
        return cmd

    warn("arduino-cli is not installed.")
    if not ask_yes_no(f"Download arduino-cli into {LOCAL_CLI_DIR}?", default_yes=True, auto_yes=auto_yes):
        fail("arduino-cli is required. Install it (https://arduino.github.io/arduino-cli/) and run the installer again.")

    url = cli_download_url()
    say(f"Downloading {url} ...")
    LOCAL_CLI_DIR.mkdir(parents=True, exist_ok=True)
    archive = LOCAL_CLI_DIR / url.rsplit("/", 1)[-1]
    try:
        urllib.request.urlretrieve(url, archive)
        if archive.suffix == ".zip":
            with zipfile.ZipFile(archive) as zf:
                zf.extractall(LOCAL_CLI_DIR)
        else:
            with tarfile.open(archive) as tf:
                tf.extractall(LOCAL_CLI_DIR)
    except Exception as exc:
        fail(f"arduino-cli download failed ({exc}). Install it manually and retry.")
    finally:
        archive.unlink(missing_ok=True)

    exe = LOCAL_CLI_DIR / ("arduino-cli.exe" if platform.system() == "Windows" else "arduino-cli")
    if exe.exists() and platform.system() != "Windows":
        exe.chmod(0o755)

    cmd = resolve_cli_command()
    if cmd is None:
        fail("arduino-cli downloaded but could not be invoked.")
    return cmd


def parse_board_list_json(raw_json: str) -> list[dict[str, str]]:
    try:
        data: Any = json.loads(raw_json)
    except json.JSONDecodeError:
        return []

    # arduino-cli 1.x wraps the list in {"detected_ports": [...]}; 0.x returned the list directly.
    items = data.get("detected_ports", []) if isinstance(data, dict) else data
    ports: list[dict[str, str]] = []
    if not isinstance(items, list):
        return ports

    for item in items:
        if not isinstance(item, dict):
            continue
        info = item.get("port") or {}
        if not isinstance(info, dict) or info.get("protocol", "serial") != "serial":
            continue
        port = str(info.get("address") or "").strip()
        if not port:
            continue
        boards = [str(b.get("name")) for b in item.get("matching_boards") or [] if isinstance(b, dict)]
        description = ", ".join(boards) or str(info.get("protocol_label") or "").strip()
        ports.append({"port": port, "description": description, "usb": "1" if "USB" in str(info.get("protocol_label")) else ""})
    # List USB ports first, so --yes never picks a motherboard UART such as /dev/ttyS0.
    ports.sort(key=lambda p: not p["usb"])
    return ports


def detect_ports(cli_cmd: list[str]) -> list[dict[str, str]]:
    try:
        result = run_cmd(cli_cmd + ["board", "list", "--format", "json"], check=True, capture=True)
        ports = parse_board_list_json(result.stdout)
        if ports:
            return ports
    except Exception:
        pass

    # Fallback path-based probe for Unix-like systems.
    ports: list[dict[str, str]] = []
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usb*", "/dev/cu.SLAB*", "/dev/tty.usb*"):
        for path in sorted(glob.glob(pattern)):
            ports.append({"port": str(path), "description": ""})
    return ports


def choose_port_interactive(ports: list[dict[str, str]]) -> str:
    while True:
        print("\nDetected serial ports:")
        if not ports:
            print("  (none found)")
        else:
            for idx, item in enumerate(ports, start=1):
                desc = f" - {item['description']}" if item["description"] else ""
                print(f"  {idx}) {item['port']}{desc}")
        print("  m) Enter port manually")
        print("  r) Re-scan ports")

        choice = input("Select port: ").strip()
        if choice.lower() == "m":
            manual = input("Enter port (example: /dev/ttyACM0 or COM3): ").strip()
            if manual:
                return manual
            continue
        if choice.lower() == "r":
            return "__rescan__"
        if choice.isdigit():
            idx = int(choice)
            if 1 <= idx <= len(ports):
                return ports[idx - 1]["port"]
        print("Invalid selection.")


def select_port(cli_cmd: list[str], requested_port: str | None, auto_yes: bool) -> str:
    if requested_port:
        return requested_port

    ports = detect_ports(cli_cmd)

    if auto_yes:
        if not ports:
            fail("No serial port detected. Connect board or pass --port COM3 (Windows) or /dev/ttyACM0.")
        return ports[0]["port"]

    while True:
        selected = choose_port_interactive(ports)
        if selected == "__rescan__":
            ports = detect_ports(cli_cmd)
            continue
        return selected


def open_config_editor() -> None:
    editor = os.environ.get("EDITOR")
    system = platform.system()

    try:
        if editor:
            run_cmd(editor.split() + [str(CONFIG_FILE)], check=True)
            return

        if system == "Windows":
            run_cmd(["notepad", str(CONFIG_FILE)], check=True)
            return

        if system == "Darwin":
            run_cmd(["open", "-e", str(CONFIG_FILE)], check=True)
            return

        if shutil.which("nano"):
            run_cmd(["nano", str(CONFIG_FILE)], check=True)
            return
        if shutil.which("vi"):
            run_cmd(["vi", str(CONFIG_FILE)], check=True)
            return

        warn(f"No supported editor found. Edit this file manually: {CONFIG_FILE}")
    except subprocess.CalledProcessError:
        warn(f"Could not open editor. Edit this file manually: {CONFIG_FILE}")


def show_permissions_hint(port: str) -> None:
    if platform.system() != "Linux":
        return

    if not port.startswith("/dev/"):
        return

    if os.path.exists(port) and not os.access(port, os.R_OK | os.W_OK):
        warn(f"You may not have serial permission for {port}")
        print("Try:")
        print('  sudo usermod -aG dialout "$USER"')
        print("Then log out and back in.")


def determine_mode(flag_yes: bool, flag_no: bool, default_for_auto_yes: bool) -> str:
    if flag_yes:
        return "yes"
    if flag_no:
        return "no"
    return "auto_yes_default_true" if default_for_auto_yes else "auto_yes_default_false"


def should_enable(mode: str, auto_yes: bool, prompt: str, default_yes: bool) -> bool:
    if mode == "yes":
        return True
    if mode == "no":
        return False
    if auto_yes:
        return mode == "auto_yes_default_true"
    return ask_yes_no(prompt, default_yes=default_yes, auto_yes=False)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Dial Throttle cross-platform installer")
    parser.add_argument("-y", "--yes", action="store_true", help="Non-interactive mode")
    parser.add_argument("-p", "--port", help="Serial port (example: COM3 or /dev/ttyACM0)")

    monitor_group = parser.add_mutually_exclusive_group()
    monitor_group.add_argument("--monitor", action="store_true", help="Open serial monitor after upload")
    monitor_group.add_argument("--no-monitor", action="store_true", help="Do not open monitor after upload")

    edit_group = parser.add_mutually_exclusive_group()
    edit_group.add_argument("--edit-config", action="store_true", help="Open DialThrottle/config.h before flashing")
    edit_group.add_argument("--no-edit-config", action="store_true", help="Skip config editor")

    return parser.parse_args()


def main() -> None:
    args = parse_args()

    print("\nDial Throttle Installer (Linux/macOS/Windows)")
    print("This wizard builds and flashes firmware to a connected M5Stack Dial.")

    cli_cmd = ensure_arduino_cli(auto_yes=args.yes)

    edit_mode = determine_mode(args.edit_config, args.no_edit_config, default_for_auto_yes=False)
    if should_enable(edit_mode, args.yes, prompt=f"Open {CONFIG_FILE} now?", default_yes=False):
        open_config_editor()

    port = select_port(cli_cmd, requested_port=args.port, auto_yes=args.yes)
    show_permissions_hint(port)

    print(f"\nReady to flash using: {port}")
    if not ask_yes_no("Continue?", default_yes=True, auto_yes=args.yes):
        fail("Installer canceled by user.")

    # The first build downloads the ESP32 core and libraries pinned in sketch.yaml (several hundred MB).
    say("Building firmware (the first build downloads the ESP32 core and libraries)...")
    run_cmd(cli_cmd + ["lib", "update-index"], check=False)
    run_cmd(cli_cmd + ["compile", "--profile", PROFILE, str(SKETCH_DIR)], check=True)

    say(f"Uploading firmware to {port}...")
    run_cmd(cli_cmd + ["upload", "--profile", PROFILE, "-p", port, str(SKETCH_DIR)], check=True)

    monitor_cmd = cli_cmd + ["monitor", "-p", port, "--config", "baudrate=115200"]
    monitor_mode = determine_mode(args.monitor, args.no_monitor, default_for_auto_yes=False)
    if should_enable(monitor_mode, args.yes, prompt="Open serial monitor now?", default_yes=True):
        say("Opening serial monitor at 115200 baud (Ctrl+C to exit)...")
        run_cmd(monitor_cmd, check=True)
    else:
        print("Install complete.")
        print(f"Open monitor later with: {' '.join(monitor_cmd)}")

if __name__ == "__main__":
    main()

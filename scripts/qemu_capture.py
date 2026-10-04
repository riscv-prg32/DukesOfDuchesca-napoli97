#!/usr/bin/env python3
"""Run the cartridge in PRG32's QEMU firmware and record what it really does.

The cartridge is staged into a private copy of the firmware's flash image and
booted in Espressif QEMU. The tool then copies the firmware's 320x240 frame
buffer out of guest memory with the QEMU monitor's `pmemsave`, presses START
through the UART keyboard mapper, drives for a while (pointing the D-pad,
honking, pressing B), feeds the firmware's credit-paced UART audio and records it.

  release-artifacts/qemu/title.png, drive-*.png   real frames
  release-artifacts/qemu/audio.wav                the real mixer output
  release-artifacts/qemu/boot-log.txt             the firmware console

Usage (after `python3 -m prg32 qemu build` in the PRG32 checkout and ./build.sh):

    PRG32_REPO=/path/to/PRG32 python3 scripts/qemu_capture.py
"""
from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

import numpy as np
from PIL import Image

GAME = Path(__file__).resolve().parents[1]
RATE = 22050
CONSOLE_PORT, AUDIO_PORT, MONITOR_PORT = 5561, 4321, 5562
RIGHT, LEFT, UP, DOWN, A, B, START = "d", "a", "w", "s", "j", "k", " "
PANEL_W, PANEL_H = 320, 240


def connect(port: int, process: subprocess.Popen, timeout: float = 20.0) -> socket.socket:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited early with status {process.returncode}")
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"timed out connecting to QEMU port {port}")


def audio_worker(process, samples: bytearray, stopping: threading.Event) -> None:
    """Grant one 20 ms credit at a time, as the host audio player does."""
    sock = connect(AUDIO_PORT, process)
    sock.settimeout(None)
    chunk = 441 * 2
    try:
        sock.sendall(b"K")
        next_credit = time.monotonic() + 0.02
        while not stopping.is_set() and process.poll() is None:
            data = bytearray()
            while len(data) < chunk:
                part = sock.recv(chunk - len(data))
                if not part:
                    return
                data.extend(part)
            samples.extend(data)
            delay = next_credit - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            next_credit += 0.02
            sock.sendall(b"K")
    except OSError:
        return
    finally:
        sock.close()


def drain(sock: socket.socket, sink: bytearray, stopping: threading.Event) -> None:
    sock.settimeout(0.2)
    while not stopping.is_set():
        try:
            data = sock.recv(4096)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            return
        if not data:
            return
        sink.extend(data)


def framebuffer_address(elf: Path) -> int:
    tools = sorted(Path.home().glob(".espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin"))
    nm = shutil.which("riscv32-esp-elf-nm", path=os.pathsep.join([str(t) for t in tools] + [os.environ["PATH"]]))
    if not nm:
        raise SystemExit("riscv32-esp-elf-nm not found; source the ESP-IDF environment")
    for line in subprocess.run([nm, str(elf)], check=True, capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == "g_fb":
            return int(fields[0], 16)
    raise SystemExit(f"g_fb not found in {elf}")


def grab(monitor: socket.socket, address: int, path: Path) -> Image.Image | None:
    size = PANEL_W * PANEL_H * 2
    path.unlink(missing_ok=True)
    monitor.sendall(f"pmemsave {address:#x} {size} \"{path}\"\n".encode())
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size == size:
            raw = np.frombuffer(path.read_bytes(), dtype="<u2").reshape(PANEL_H, PANEL_W)[20:220]
            rgb = np.dstack(((raw >> 11) * 255 // 31, ((raw >> 5) & 63) * 255 // 63, (raw & 31) * 255 // 31))
            return Image.fromarray(rgb.astype(np.uint8), "RGB")
        time.sleep(0.004)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prg32-root", type=Path,
                        default=Path(os.environ.get("PRG32_REPO", GAME.parent / "PRG32")))
    parser.add_argument("--cartridge", type=Path, default=GAME / "build/dukesofduchesca-napoli97-base.prg32")
    parser.add_argument("--out", type=Path, default=GAME / "release-artifacts/qemu")
    parser.add_argument("--warmup", type=float, default=8.0, help="seconds from reset to the title screen")
    parser.add_argument("--drive", type=float, default=40.0, help="seconds of driving to record")
    parser.add_argument("--display", default="sdl", help="QEMU display back end; the firmware stalls with `none`")
    args = parser.parse_args()

    root = args.prg32_root.resolve()
    for required in (root / "build-qemu/qemu_flash.bin", root / "build-qemu/qemu_efuse.bin", args.cartridge):
        if not required.exists():
            raise SystemExit(f"missing {required}")
    qemu = shutil.which("qemu-system-riscv32", path=os.pathsep.join(
        [str(p) for p in sorted(Path.home().glob(".espressif/tools/qemu-riscv32/*/qemu/bin"))] + [os.environ["PATH"]]))
    if not qemu:
        raise SystemExit("qemu-system-riscv32 (Espressif build) not found")
    fb_address = framebuffer_address(root / "build-qemu/PRG32.elf")
    args.out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="dukes-qemu-") as temp_name:
        temp = Path(temp_name)
        flash, efuse = temp / "flash.bin", temp / "efuse.bin"
        shutil.copy2(root / "build-qemu/qemu_flash.bin", flash)
        shutil.copy2(root / "build-qemu/qemu_efuse.bin", efuse)
        subprocess.run([sys.executable, "-m", "prg32", "qemu", "upload", str(args.cartridge.resolve()),
                        "--flash", str(flash)], cwd=root, check=True)
        command = [
            qemu, "-M", "esp32c3", "-m", "4M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth", "-display", args.display,
            "-monitor", f"tcp:127.0.0.1:{MONITOR_PORT},server=on,wait=off",
            "-serial", f"tcp:127.0.0.1:{CONSOLE_PORT},server=on,wait=off,nodelay=on",
            "-serial", f"tcp:127.0.0.1:{AUDIO_PORT},server=on,wait=on,nodelay=on",
        ]
        samples, transcript, monitor_log = bytearray(), bytearray(), bytearray()
        stopping = threading.Event()
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        threads = [threading.Thread(target=audio_worker, args=(process, samples, stopping), daemon=True)]
        threads[0].start()
        console = connect(CONSOLE_PORT, process)
        monitor = connect(MONITOR_PORT, process)
        threads.append(threading.Thread(target=drain, args=(console, transcript, stopping), daemon=True))
        threads.append(threading.Thread(target=drain, args=(monitor, monitor_log, stopping), daemon=True))
        for thread in threads[1:]:
            thread.start()
        try:
            time.sleep(args.warmup)
            image = grab(monitor, fb_address, temp / "panel.bin")
            if image is not None:
                image.save(args.out / "title.png")
            console.sendall(START.encode())
            start = time.monotonic()
            shots, frames, changed = 0, 0, 0
            previous = None
            while (now := time.monotonic() - start) < args.drive:
                # Point the D-pad one way for a few seconds, then another; honk
                # now and then, and try B (it lights a rauto once a box is aboard).
                keys = (UP, RIGHT, DOWN, LEFT, UP, LEFT)[int(now / 4.0) % 6]
                phase = now % 6.0
                if phase < 0.1:
                    keys += A
                if 3.0 < phase < 3.1:
                    keys += B
                console.sendall(keys.encode())
                image = grab(monitor, fb_address, temp / "panel.bin")
                if image is not None:
                    frames += 1
                    data = image.tobytes()
                    if data != previous:
                        changed += 1
                    previous = data
                    if now > 2.0 and int(now / 6.0) >= shots:
                        image.save(args.out / f"drive-{shots:02d}.png")
                        shots += 1
                time.sleep(0.05)
            print(f"{frames} frames grabbed, {changed} of them different from the one before")
        finally:
            stopping.set()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        with wave.open(str(args.out / "audio.wav"), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)
            wav.writeframes(bytes(samples[: len(samples) // 2 * 2]))
        pcm = np.frombuffer(bytes(samples[: len(samples) // 2 * 2]), dtype="<i2")
        peak = int(np.abs(pcm).max()) if len(pcm) else 0
        print(f"audio: {len(pcm) / RATE:.1f} s, peak {peak} of 32767")
        log = transcript.decode("utf-8", "replace")
        keep = [line for line in log.splitlines() if "TRACKER EVENT" not in line]
        (args.out / "boot-log.txt").write_text("\n".join(keep) + "\n")
        print(f"console: {len(log.splitlines())} lines, {log.count('TRACKER EVENT')} tracker events")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

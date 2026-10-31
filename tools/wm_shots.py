#!/usr/bin/env python3
"""Pictures of the window manager's and the bar's features on the real desktop (`nx`), one
capture a scene, through the queue and with no card time (--cpu --warp): the pictures the wm-
cases of tools/test_all.py cannot look at (docs/system/nanox.md, "The window manager and the bar").

    python tools/wm_shots.py --image build/images/NAME --out DIR [SCENE ...]

Writes DIR/SCENE.png and DIR/SCENE.log. With no scene, all of them."""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The screen is 800 x 600 unless the image says otherwise; the bar is its last 28 rows.
A = "nxwintest plain a & sleep 2; "
B = A + "nxwintest plain b & sleep 2; "
CAPTION = "150,50"   # on the caption of a window nxwintest made, not on its buttons
SCENES = {
    # a frame held at the left edge, with where it would go under it
    "snap-preview": A + "nxkey hold:%s to:300,200 to:60,200 to:2,200 2000 & sleep 6; nxwintest list" % CAPTION,
    "snap-left": A + "nxkey drag:%s:2,200 2000; nxwintest list" % CAPTION,
    "quarters": B + "nxkey ctrl+alt+u 1500; nxwintest plain c & sleep 2; nxkey ctrl+alt+k 1500; nxwintest list",
    "maximised": A + "nxkey ctrl+alt+up 1500; nxwintest list",
    "shaded": B + "nxkey ctrl+alt+s 1500; nxwintest list",
    "ontop": B + "nxkey ctrl+alt+t 500 ctrl+tab 1500; nxwintest list",
    "switcher": B + "nxwintest plain c & sleep 2; nxkey down:ctrl tab 800 tab 1500 & sleep 3; nxwintest list",
    "menu": A + "nxkey ctrl+alt+space 2000; nxwintest list",
    "caption-menu": A + "nxkey r:%s 2000; nxwintest list" % CAPTION,
    "desktops": A + "nxkey ctrl+alt+2 1000; nxwintest plain two & sleep 2; nxkey ctrl+alt+shift+3 1000 ctrl+alt+1 1500; nxwintest list",
    "group": "nxwintest two a & sleep 2; nxwintest plain c & sleep 2; nxwintest list",
    "group-menu": "nxwintest two a & sleep 3; nxkey 90,588 2000; nxwintest list",
    "starting": "nxwintest slow 30 & sleep 2; nxwintest list",
    "not-responding": "nxwintest hang h & sleep 2; nxkey alt+f4 14000; nxwintest list",
    "still-running": "nxwintest linger l & sleep 2; nxkey alt+f4 22000; nxwintest list",
    "crash": "nxwintest crash c & sleep 8; nxwintest list",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("scene", nargs="*")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    harness = os.path.join("bin", "rvc_harness.exe")
    for name in args.scene or SCENES:
        snap = os.path.join(args.out, name + ".snap")
        # (the bar's words go to the console; nx comes back with the desktop up)
        send = "export NXBAR_PRINT=1 NXBAR_CLOSE_MS=${NXBAR_CLOSE_MS:-5000}; nx; sleep 4; %s; sleep 1; echo LX-''DONE\\n" % SCENES[name]
        cmd = ["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "gpu_queue.ps1"), harness, "--d3d11", "--rvc", "experiments\\rvc_opt",
               "--image", args.image, "--cpu", "--warp", "--no-desktop", "--expect", "/ # ", "--send", send,
               "--until", "LX-DONE", "--save-state", snap]
        run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=900)
        with open(os.path.join(args.out, name + ".log"), "w", encoding="utf-8") as log:
            log.write(run.stdout + run.stderr)
        shot = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "snapshot_screen.py"), snap, os.path.join(args.out, name + ".png")],
                              capture_output=True, text=True, errors="replace")
        print(name, "exit", run.returncode, shot.stdout.strip()[-80:], shot.stderr.strip()[-200:])
        if os.path.exists(snap):
            os.remove(snap)


if __name__ == "__main__":
    main()

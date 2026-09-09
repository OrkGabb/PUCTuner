"""Run a reviewed local shell script through ADB with the whole script under su.

Avoid Windows/adb/su nested quoting accidentally running only the first command as root.
No reboot operation is provided here.
"""
import argparse
import pathlib
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("script", type=pathlib.Path)
args = parser.parse_args()
script = args.script.read_text(encoding="utf-8").replace("\r\n", "\n")
result = subprocess.run(["adb", "shell", "su -c 'sh -s'"], input=script, text=True)
raise SystemExit(result.returncode)

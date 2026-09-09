"""Produce reproducible module archives, checking the native payload and ZIP CRCs."""
from pathlib import Path
import hashlib
import shutil
import struct
import zipfile

root = Path(__file__).resolve().parents[1]
native = root / "module/bin/m54-adaptive"
payload = native.read_bytes()
assert payload[:6] == b"\x7fELF\x02\x01", "Build the Android ARM64 engine first"
assert struct.unpack_from("<HH", payload, 16) == (3, 183), "Expected AArch64 PIE executable"
archive = root / "m54tuner-module.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
    for path in sorted((root / "module").rglob("*")):
        if path.is_file():
            name = path.relative_to(root / "module").as_posix()
            entry = zipfile.ZipInfo(name, date_time=(2026, 9, 8, 0, 0, 0))
            mode = 0o755 if path.suffix == ".sh" or name.startswith("bin/") else 0o644
            entry.external_attr = (0o100000 | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(entry, path.read_bytes())
with zipfile.ZipFile(archive) as check:
    assert check.testzip() is None
    assert "bin/m54-adaptive" in check.namelist()
apk = root / "app/build/outputs/apk/release/app-release.apk"
shutil.copy2(apk, root / "m54tuner-release.apk")
for file in (archive, root / "m54tuner-release.apk"):
    print(file.name, file.stat().st_size, hashlib.sha256(file.read_bytes()).hexdigest())

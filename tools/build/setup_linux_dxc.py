#!/usr/bin/env python3
"""Install the pinned official Linux x64 DXC build used by the Android pipeline."""
import hashlib
import pathlib
import shutil
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
URL = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/linux_dxc_2026_07_29.x86_x64.tar.gz"
SHA256 = "55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54"
PREFIX = "linux_dxc_2026_07_29.x86_x64"
archive = ROOT / "out/downloads/dxc-linux.tar.gz"
archive.parent.mkdir(parents=True, exist_ok=True)
if not archive.exists():
    urllib.request.urlretrieve(URL, archive)
with archive.open("rb") as stream:
    digest = hashlib.file_digest(stream, "sha256").hexdigest()
if digest != SHA256:
    raise SystemExit("DXC archive checksum mismatch; remove it and retry")
dest = ROOT / "tools/XenosRecomp/thirdparty/dxc-bin"
with tarfile.open(archive) as source:
    for name, relative in (("bin/dxc", "bin/x64/dxc-linux"),
                           ("lib/libdxcompiler.so", "lib/x64/libdxcompiler.so"),
                           ("lib/libdxil.so", "lib/x64/libdxil.so")):
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with source.extractfile(f"{PREFIX}/{name}") as stream, target.open("wb") as output:
            shutil.copyfileobj(stream, output)
        target.chmod(0o755)
print("Installed checksum-verified Linux DXC")

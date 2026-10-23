#!/usr/bin/env python3
"""Download pinned Qwen3/llama.cpp assets and build a Linux CPU server."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def sha256(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file,"sha256").hexdigest()

def download(spec, target):
    if target.exists() and sha256(target) == spec["sha256"]:
        print("Verified", target, flush=True)
        return
    temp = target.with_suffix(target.suffix + ".part")
    try:
        print("Downloading", spec["url"], flush=True)
        with urllib.request.urlopen(spec["url"], timeout=60) as response, temp.open("wb") as output:
            shutil.copyfileobj(response,output)
        if sha256(temp) != spec["sha256"]:
            raise RuntimeError("SHA256 mismatch: " + str(target))
        temp.replace(target)
    finally:
        temp.unlink(missing_ok=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake",default="cmake")
    parser.add_argument("--jobs",type=int,default=2)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error("--jobs must be between 1 and 64")
    lock = json.loads((ROOT / "configs/runtime.lock.json").read_text())
    model = ROOT / "models" / lock["model"]["filename"]
    model.parent.mkdir(exist_ok=True)
    download(lock["model"],model)
    cache = ROOT / "build-runtime-download"
    cache.mkdir(exist_ok=True)
    archive = cache / "llama.tar.gz"
    download(lock["llama_cpp"],archive)
    source = ROOT / "third_party/llama.cpp"
    marker = source / ".edge-source-revision"
    if source.exists() and (not marker.is_file() or marker.read_text().strip() != lock["llama_cpp"]["revision"]):
        raise RuntimeError("Existing llama.cpp source has no matching revision marker; move it aside before setup")
    if not source.exists():
        source.mkdir()
        with tarfile.open(archive) as package:
            # Only regular files/directories from this pinned archive. Never extract links.
            for member in package.getmembers():
                parts = Path(member.name).parts[1:]
                if not parts:
                    continue
                if any(part in ("..", "/") for part in parts) or not (member.isdir() or member.isfile()):
                    raise RuntimeError("Unsafe archive member")
                destination = source.joinpath(*parts)
                if member.isdir():
                    destination.mkdir(parents=True,exist_ok=True)
                else:
                    destination.parent.mkdir(parents=True,exist_ok=True)
                    with package.extractfile(member) as data, destination.open("wb") as output:
                        shutil.copyfileobj(data,output)
                    destination.chmod(member.mode & 0o777)
        marker.write_text(lock["llama_cpp"]["revision"]+"\n")
    build = ROOT / "build-llama"
    subprocess.run([args.cmake,"-S",str(source),"-B",str(build),"-DCMAKE_BUILD_TYPE=Release", "-DGGML_CUDA=OFF", "-DGGML_VULKAN=OFF", "-DGGML_NATIVE=ON",
                    "-DLLAMA_OPENSSL=OFF","-DLLAMA_BUILD_TESTS=OFF","-DLLAMA_BUILD_EXAMPLES=OFF","-DLLAMA_BUILD_TOOLS=ON","-DLLAMA_BUILD_SERVER=ON","-DLLAMA_BUILD_UI=OFF"],check=True)
    subprocess.run([args.cmake,"--build",str(build),"--target","llama-server","-j",str(args.jobs)],check=True)
    version = subprocess.run([str(build / "bin/llama-server"),"--version"],capture_output=True,text=True,check=True)
    manifest = {**lock,"server_version_output":(version.stdout+version.stderr).strip(),"server_binary_sha256":sha256(build / "bin/llama-server"),"model_size_bytes":model.stat().st_size}
    (ROOT / "models/runtime-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    print("Runtime installed. Start with: python3 scripts/start_server.py")

if __name__ == "__main__":
    main()

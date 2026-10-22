#!/usr/bin/env python3
"""Transfer a guarded, source-only Cargo-fetch image into an original MDK011 build.

This local cache route consumes a successful mdk-gate images receipt whose image
has fetched the exact current Cargo pins but has never compiled the peer. It
exports a stopped, network-disabled, explicitly owned CID, binds the filesystem
and image configuration, then builds the unchanged original driver/tests in a new
owned builder. No prepared peer, dependency patch, default builder or registry
publication. Derived context/cache provenance and negative receipts are retained.
"""
import argparse
import contextlib
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import uuid

# This is a finite source-cache bridge, not arbitrary image/recipe admission.
FETCH_RECIPE_SHA256 = "a7ad9a5bcb317963487a6cdb186a0ac26ade79b18b91477bd8f11d4db3d45afb"
BUILD_PREFIX_SHA256 = "3618b62542bbac73ed18593cf91d4568b34c0d671a3aa03747682f7443cbf943"


def sha(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


@contextlib.contextmanager
def export_container(call, image_id, run_id, result, save):
    """Reserve exact Docker identity before create; cancellation never skips cleanup.

    A failed/indeterminate create is retained as negative even if its name is
    absent on reconciliation. No global prune or unverified name deletion.
    """
    name = 'nostrc-mdk-cache-' + uuid.uuid4().hex
    label = 'org.nostrc.mdk.cache'
    token = uuid.uuid4().hex
    result['cache_export_reservation'] = {'name': name, 'label': label, 'token': token,
                                           'producer_run_id': run_id, 'image_id': image_id}
    result['cache_export_container_removed'] = False
    cancelled = []
    handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    def stop(signum, frame):
        cancelled.append(signum)
    for sig in handlers:
        signal.signal(sig, stop)
    try:
        save()
        raw = call(['create', '--network', 'none', '--name', name,
                    '--label', label + '=' + token, image_id]).decode().strip()
        if not re.fullmatch(r'[0-9a-f]{64}', raw):
            raise ValueError('cache export did not return an immutable CID')
        result['cache_export_cid'] = raw
        save()
        if cancelled:
            raise InterruptedError('cache export cancelled after ownership acquisition')
        yield raw
        if cancelled:
            raise InterruptedError('cache export cancelled during export')
    except BaseException as exc:
        result['cache_export_error'] = type(exc).__name__ + ': ' + str(exc)
        raise
    finally:
        try:
            observed = json.loads(call(['container', 'inspect', name]))[0]
            cid = observed['Id']
            if (not re.fullmatch(r'[0-9a-f]{64}', cid) or observed['Name'] != '/' + name or
                observed['Image'] != image_id or observed['Config'].get('Labels', {}).get(label) != token or
                observed.get('State', {}).get('Running') or observed.get('Mounts')):
                raise ValueError('cache export reserved identity changed; removal refused')
            if result.get('cache_export_cid', cid) != cid:
                raise ValueError('cache export returned and reserved CID differ')
            result['cache_export_cid'] = cid
            result['cache_export_owned_inspection'] = observed
            save()
            call(['rm', '-v', cid])
            try:
                call(['container', 'inspect', cid])
            except subprocess.CalledProcessError as absent:
                stderr = (absent.stderr or b'').decode('utf-8', 'replace')
                result['cache_export_final_inspection'] = {'exit_code': absent.returncode, 'stderr': stderr}
                if absent.returncode != 1 or ('no such container: ' + cid) not in stderr.lower():
                    raise ValueError('cache export CID absence unverified') from absent
            else:
                raise ValueError('cache export CID still exists')
            result['cache_export_container_removed'] = True
        except BaseException as exc:
            result['cache_export_cleanup_error'] = type(exc).__name__ + ': ' + str(exc)
            raise
        finally:
            result['cache_export_cancel_signals'] = cancelled
            try:
                save()
            finally:
                for sig, handler in handlers.items():
                    signal.signal(sig, handler)
        if cancelled and 'cache_export_error' not in result:
            raise InterruptedError('cache export cancelled during cleanup')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--fetch-receipt", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    source = args.source_root.resolve(strict=True)
    receipt_path = args.fetch_receipt.resolve(strict=True)
    receipt = json.loads(receipt_path.read_text())
    helper = source / "scripts/mdk-gate.py"
    if (receipt.get("status") != "docker_lifecycle_succeeded" or
        receipt.get("cleanup_complete") is not True or receipt.get("complete") is not False or
        receipt.get("helper", {}).get("sha256") != sha(helper)):
        raise ValueError("cache producer is not a successful current guarded image build")
    images = receipt.get("image_results", [])
    if len(images) != 1 or images[0].get("version") != "011" or not images[0].get("build_complete"):
        raise ValueError("cache producer must contain exactly one completed 011 fetch image")
    original = source / "tests/interop/mdk/driver-0.11"
    text = (original / "Dockerfile").read_text()
    body_start = text.index("COPY Cargo.toml Cargo.lock rust-toolchain.toml ./")
    if hashlib.sha256(text[:body_start].encode()).hexdigest() != BUILD_PREFIX_SHA256:
        raise ValueError("unreviewed original011 base/tool/setup recipe")
    producer = Path(images[0]["context"]).resolve(strict=True)
    pins = {name: sha(original / name) for name in ("Cargo.toml", "Cargo.lock", "rust-toolchain.toml")}
    if any(sha(producer / name) != value for name, value in pins.items()):
        raise ValueError("cache producer Cargo pins differ from current original011")
    spec = importlib.util.spec_from_file_location("mdk_gate", helper)
    gate = importlib.util.module_from_spec(spec); spec.loader.exec_module(gate)
    if gate.digest(producer) != images[0]["digest"]:
        raise ValueError("cache producer context changed after its build")
    if sha(producer / "Dockerfile") != FETCH_RECIPE_SHA256:
        raise ValueError("unreviewed Cargo fetch producer recipe")
    docker = receipt["docker"]
    env = dict(os.environ, DOCKER_HOST=receipt["docker_endpoint"])
    def call(argv, **kwargs):
        return subprocess.check_output([docker, *argv], env=env, timeout=300,
                                       stderr=subprocess.PIPE, start_new_session=True, **kwargs)
    if (sha(docker) != receipt["docker_executable"]["sha256"] or
        call(["info", "--format", "{{.ID}}"]).decode().strip() != receipt["daemon_id"]):
        raise ValueError("cache producer Docker executable/daemon identity changed")
    image_id = images[0]["image_id"]
    image = json.loads(call(["image", "inspect", image_id]))[0]
    expected_labels = {gate.RUN_LABEL: receipt["run_id"],
                       gate.CONTEXT_LABEL: images[0]["digest"], gate.VERSION_LABEL: "011"}
    if image["Id"] != image_id or image["Config"].get("Volumes") or any(
        image["Config"].get("Labels", {}).get(k) != v for k, v in expected_labels.items()):
        raise ValueError("cache image immutable identity/labels/volume contract changed")
    os.umask(0o077)
    out = args.evidence_dir.resolve(); out.mkdir(mode=0o700)
    context_root = out / "source"
    context = context_root / "tests/interop/mdk/driver-0.11"
    shutil.copytree(original, context)
    result = {"producer_receipt": {"path": str(receipt_path), "sha256": sha(receipt_path)},
              "producer_image": image_id, "producer_image_inspect": image,
              "helper_sha256": sha(helper), "cache_builder_sha256": sha(__file__), "source_pins": pins,
              "original_context_digest": gate.digest(original),
              "complete": False, "complete_gate_eligible": False,
              "cache_export_container_removed": False}
    def save():
        (out / "receipt.json").write_text(json.dumps(result, indent=2) + "\n")
    save()
    with export_container(call, image_id, receipt['run_id'], result, save) as cid:
        raw = out / "rootfs.tar"
        call(["export", "--output", str(raw), cid])
        result["uncompressed_cache_sha256"] = sha(raw)
        with tarfile.open(raw) as archive:
            names = archive.getnames()
            if any(n.lstrip("./").startswith(("src/target", "usr/local/bin/mdk011-interop-driver")) for n in names):
                raise ValueError("cache filesystem contains compiled peer outputs")
            for name, expected in pins.items():
                member = archive.extractfile("src/" + name)
                if member is None or hashlib.sha256(member.read()).hexdigest() != expected:
                    raise ValueError("exported cache workspace does not match original pins")
        cache = context / "cargo-source-cache.tar.gz"
        with raw.open("rb") as src, cache.open("wb") as dst:
            with gzip.GzipFile(filename="", mode="wb", fileobj=dst, compresslevel=1, mtime=0) as zipped:
                shutil.copyfileobj(src, zipped)
        result["cache_sha256"] = sha(cache); raw.unlink()
    body = text[body_start:]
    body = body.replace("RUN mkdir src", "RUN --network=none mkdir src").replace(
        "cargo fetch --locked --verbose", "cargo fetch --frozen --verbose")
    prefix = "FROM scratch AS build\nADD cargo-source-cache.tar.gz /\n"
    for entry in image["Config"].get("Env", []):
        key, value = entry.split("=", 1)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key) or "\n" in value:
            raise ValueError("unsupported cache image environment")
        prefix += "ENV " + key + "=" + json.dumps(value) + "\n"
    prefix += "RUN --network=none rm -rf /src && mkdir /src\nWORKDIR /src\n"
    (context / "Dockerfile").write_text(prefix + body)
    if any(sha(context / name) != value for name, value in pins.items()) or sha(context / "src/main.rs") != sha(original / "src/main.rs"):
        raise ValueError("cached build changed original driver or Cargo inputs")
    result["derived_context_digest"] = gate.digest(context)
    result["derived_dockerfile"] = (context / "Dockerfile").read_text(); save()
    with (out / "build.log").open("wb") as log:
        result["build_exit_code"] = subprocess.run([sys.executable, str(helper), "images",
            "--source-root", str(context_root), "--evidence-dir", str(out / "images"),
            "--version", "011", "--probe"], env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    result["inputs_unchanged"] = (gate.digest(context) == result["derived_context_digest"] and
                                  gate.digest(original) == result["original_context_digest"] and
                                  sha(helper) == result["helper_sha256"] and
                                  sha(__file__) == result["cache_builder_sha256"])
    save()
    print(json.dumps({"receipt": str(out / "receipt.json"),
                      "build_exit_code": result["build_exit_code"], "inputs_unchanged": result["inputs_unchanged"]}))
    return result["build_exit_code"] if result["inputs_unchanged"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as exc:
        print("mdk011-cache-build: " + str(exc), file=sys.stderr)
        sys.exit(1)

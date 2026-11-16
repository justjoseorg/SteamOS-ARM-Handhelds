#!/usr/bin/env python3
"""Plan fork updates from legacy archives or current Zaviya release images."""

import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def gh(repo, *args):
    return json.loads(subprocess.check_output(["gh", "release", *args, "-R", repo], text=True))


def checksums(repo, tag):
    return subprocess.check_output(
        ["gh", "release", "download", tag, "-R", repo, "-p", "SHA256SUMS", "-O", "-"],
        text=True)


def release_images(upstream, tag, release, supported):
    base = tag.removeprefix("v")
    modern = tuple(int(p) for p in base.split(".")) >= (1, 5)
    if modern:
        images = []
        attached = {asset["name"] for asset in release["assets"]}
        for line in checksums(upstream, tag).splitlines():
            fields = line.split()
            if len(fields) != 2 or not re.fullmatch(r"[a-fA-F0-9]{64}", fields[0]):
                continue
            name = fields[1].removeprefix("*")
            match = re.fullmatch(r"zaviya-(sm\d{4})-[0-9.]+\.img\.zst", name)
            if match and match[1] in supported:
                images.append({
                    "soc": match[1], "format": "zstd", "name": name, "sha256": fields[0].lower(),
                    "url": "" if name in attached else
                           f"https://downloads.zaviyaos.workers.dev/{base}/{name}",
                })
        if not images:
            raise ValueError(f"{tag} has no supported .img.zst image in SHA256SUMS")
        if len({image["soc"] for image in images}) != len(images):
            raise ValueError(f"{tag} lists multiple images for the same chip")
        return sorted(images, key=lambda image: image["soc"]), "zaviya-arm", "true"
    socs = sorted({m[1] for asset in release["assets"]
                   if (m := re.fullmatch(r".*-(sm\d{4})-.*\.7z\.001", asset["name"]))
                   and m[1] in supported})
    if not socs:
        raise ValueError(f"{tag} has no supported legacy .7z image")
    return [{"soc": soc, "format": "7z"} for soc in socs], "steamos-arm", "false"


def plan(upstream, repo, tag="", rebuild=False):
    if not tag:
        releases = gh(upstream, "list", "--exclude-drafts", "--exclude-pre-releases",
                      "-L", "1", "--json", "tagName")
        if not releases:
            raise ValueError("Upstream has no stable release")
        tag = releases[0]["tagName"]
    base = tag.removeprefix("v")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", base):
        raise ValueError(f"Upstream tag {tag!r} is not a plain version")
    release = gh(upstream, "view", tag, "--json", "assets")
    spec = importlib.util.spec_from_file_location(
        "update", ROOT / "external-and-mods/konkr-update/konkr-update.py")
    update = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(update)
    images, prefix, upstream_kernel = release_images(upstream, tag, release, update.SOC_MODELS)
    socs = [image["soc"] for image in images]
    releases = gh(repo, "list", "--exclude-drafts", "-L", "200", "--json", "tagName")
    pattern = rf"v{re.escape(base)}\.([0-9]+)"
    revisions = [int(m[1]) for release in releases
                 if (m := re.fullmatch(pattern, release["tagName"]))]
    have = max(revisions, default=0)
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if have and not rebuild:
        body = gh(repo, "view", f"v{base}.{have}", "--json", "body")["body"]
        if f"main@{head}" in body:
            return {"build": "false"}, f"v{base}.{have} already built from main@{head}."
    version = f"{base}.{have + 1}"
    return {
        "build": "true", "head": head, "tag": f"v{version}", "version": version,
        "upstream_tag": tag, "socs": json.dumps(socs),
        "images": json.dumps(images), "prefix": prefix, "upstream_kernel": upstream_kernel,
    }, f"Build v{version} from {tag}, main@{head}."


def main():
    outputs, message = plan(
        os.environ["UPSTREAM"], os.environ["GITHUB_REPOSITORY"],
        os.environ.get("UPSTREAM_TAG", ""), os.environ.get("REBUILD") == "true")
    print(message)
    with open(os.environ["GITHUB_OUTPUT"], "a") as out:
        for key, value in outputs.items():
            print(f"{key}={value}", file=out)
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as summary:
        print(message, file=summary)


if __name__ == "__main__":
    main()

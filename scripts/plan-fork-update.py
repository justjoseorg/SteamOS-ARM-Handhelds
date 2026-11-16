#!/usr/bin/env python3
"""Plan legacy .sau builds without treating fresh-install releases as updates."""

import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def gh(repo, *args):
    return json.loads(subprocess.check_output(["gh", "release", *args, "-R", repo], text=True))


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
    socs = sorted({m[1] for asset in release["assets"]
                   if (m := re.fullmatch(r".*-(sm\d{4})-.*\.7z\.001", asset["name"]))
                   and m[1] in update.SOC_MODELS})
    if not socs:
        return {"build": "false"}, (
            f"{tag} has no supported legacy .7z image; no .sau update will be published. "
            "Upstream v1.5 requires a fresh install and is not an OTA upgrade. "
            "The existing fork release is unchanged.")
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

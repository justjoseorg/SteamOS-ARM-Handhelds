#!/usr/bin/env python3
"""Point an image or freshly installed device at the correct fork update family."""

import json
from pathlib import Path
import re
import sys


def configure(root, repo):
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        raise ValueError("Expected owner/repository")
    metadata = json.loads((root / "usr/share/steamos-arm/update/installed.json").read_text())
    version = tuple(int(p) for p in metadata["version"].split("."))
    prefix = "zaviya-arm" if version >= (1, 5) else "steamos-arm"
    agent = root / "usr/lib/steamos-arm/update-agent"
    text = agent.read_text()
    text, count = re.subn(r'^REPO = .*$', f'REPO = "{repo}"', text, flags=re.MULTILINE)
    if count != 1:
        raise ValueError("Could not configure update-agent repository")
    # Keep service/progress paths unchanged; only change package matching.
    for key in ("NAME", "PART"):
        text, count = re.subn(
            rf'^{key} = .*$', lambda m: re.sub(r"(?:steamos|zaviya)-arm-", prefix + "-", m[0]),
            text, flags=re.MULTILINE)
        if count != 1:
            raise ValueError(f"Could not configure update-agent {key}")
    text = re.sub(r'"(?:steamos|zaviya)-arm-\*\.sau\*"', f'"{prefix}-*.sau*"', text)
    agent.write_text(text)
    return prefix


if __name__ == "__main__":
    print(configure(Path(sys.argv[1]), sys.argv[2]))

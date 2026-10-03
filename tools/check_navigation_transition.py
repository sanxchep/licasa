#!/usr/bin/env python3
"""Check that the production viewer holds its outgoing image during navigation."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    executable = Path(sys.argv[1]).resolve(strict=True)
    source = Path(sys.argv[2]).resolve(strict=True)
    target = Path(sys.argv[3]).resolve(strict=True)
    for outgoing, incoming in ((source, target), (target, source)):
        with tempfile.TemporaryDirectory(prefix="licasa-navigation-") as temporary:
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                       QSG_RENDER_LOOP="basic", XDG_CONFIG_HOME=temporary + "/config",
                       XDG_CACHE_HOME=temporary + "/cache")
            run = subprocess.run(
                [str(executable), "--viewer", "--navigation",
                 str(outgoing), str(incoming)],
                env=env, capture_output=True, text=True, timeout=20)
        if run.returncode or "Binding loop detected" in run.stderr:
            raise RuntimeError(f"Navigation transition failed: {run.stdout}\n{run.stderr}")
        result = json.loads(run.stdout)
        if result.get("error") or result.get("incoming_width") == result.get("outgoing_width"):
            raise RuntimeError(f"Navigation transition did not change image: {result}")
        print(f"Verified outgoing geometry across navigation: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

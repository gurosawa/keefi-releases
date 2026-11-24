"""Install the ZIP obtained by the real release client into a disposable profile."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import sys

if __name__ == "__main__":
    if not (os.environ.get("GITHUB_ACTIONS") == "true" and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
            and os.environ.get("RUNNER_OS") == "Windows"):
        raise SystemExit("The WER fixture requires a disposable GitHub-hosted Windows runner")
    work = Path(sys.argv[1])
    archive = work / "live.zip"
    receipt_path, = (work / "dist").glob("*-verification.json")
    receipt = json.loads(receipt_path.read_bytes())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != receipt["sha256"]:
        raise SystemExit("The signed public update differs from the tested candidate")
    scripts = Path(__file__).parent
    fixture = runpy.run_path(str(scripts / "verify-windows-update.py"))["verify"]
    clean_policy = runpy.run_path(str(scripts / "verify-windows-runner.py"))["without_local_dumps"]
    with clean_policy():
        # The candidate job already covered tamper and rollback with these exact bytes.
        # Exercise the signed public download from a single fresh, foreground-only 111 GUI.
        checked = fixture(work, archive, receipt, inject_failures=False)
    print(json.dumps(checked, ensure_ascii=True))

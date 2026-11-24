#!/usr/bin/env python3
"""Freeze a synthetic pre-rename storage baseline using the existing ed2317ae libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

EXPECTED_HEAD = "ed2317aecae5cb1c873c49102191f6746224f9f9"

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--tools-dir", type=Path, required=True)
    parser.add_argument("--attempt", type=int, default=1)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = here.parents[4]
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if head != EXPECTED_HEAD:
        raise SystemExit("Refusing a different baseline source HEAD")
    build = args.build_dir.resolve()
    suffix = "" if args.attempt == 1 else f"-{args.attempt:02d}"
    binary = args.tools_dir.resolve() / ("legacy-storage-baseline-ed2317ae" + suffix)
    data = here / ("data" + suffix)
    if data.exists() or binary.exists():
        raise SystemExit("Fresh fixture data and generator binary are required")
    ninja = (build / "build.ninja").read_text()
    def fields(target):
        match = re.search(r"^build " + re.escape(target) + r":.*\n((?:  .*\n)+)", ninja, re.M)
        if not match:
            raise SystemExit("Missing recorded build rule: " + target)
        return dict(re.findall(r"^  ([A-Z_]+) = (.*)$", match.group(1), re.M))
    compile_rule = fields("tests/finix/CMakeFiles/tst_finix_record_transfer.dir/tst_record_transfer.cpp.o")
    link_rule = fields("tests/finix/tst_finix_record_transfer")
    defines = [part for part in shlex.split(compile_rule["DEFINES"]) if not part.startswith("-DQT_TESTCASE_")]
    command = ["/usr/bin/clang++", *shlex.split(compile_rule["FLAGS"]), *defines,
               *shlex.split(compile_rule["INCLUDES"]), str(here / "generate-baseline.cpp"),
               *shlex.split(link_rule.get("LINK_PATH", "")), *shlex.split(link_rule["LINK_LIBRARIES"]),
               "-o", str(binary)]
    modules = ["Vault", "AssetsLedger", "SwapJournal", "PriceArchive", "RecordTransfer"]
    source_inputs = [source / "fincept-qt/src/finix" / (name + suffix) for name in modules for suffix in [".cpp", ".h"]]
    libraries = [build / name for name in ["libfinix_vault.a", "libfinix_trading.a", "libfinix_notifications.a"]]
    inputs = {str(p.relative_to(source)) if p.is_relative_to(source) else str(p): sha(p)
              for p in [*source_inputs, *libraries, here / "generate-baseline.cpp", here / "generate-baseline.py"]}
    compile_run = subprocess.run(command, cwd=build, text=True, capture_output=True)
    (here / ("compile" + suffix + ".log")).write_text(compile_run.stdout + compile_run.stderr)
    if compile_run.returncode:
        raise SystemExit("Synthetic generator compilation failed; see compile.log")
    run = subprocess.run([str(binary), str(data)], cwd=source, text=True, capture_output=True)
    (here / ("generation" + suffix + ".log")).write_text(run.stdout + run.stderr)
    if run.returncode:
        raise SystemExit("Synthetic fixture generation failed; see generation.log")
    for path in libraries:
        if sha(path) != inputs[str(path)]:
            raise SystemExit("A baseline library changed during generation")
    receipt = {"format": "legacy-storage-synthetic-baseline/1", "synthetic": True,
               "source_commit": head, "existing_library_build": True, "inputs_sha256": inputs,
               "attempt": args.attempt, "data_directory": data.name,
               "compile_command": command, "generator_binary_sha256": sha(binary),
               "expected_sha256": sha(data / "expected.json"),
               "exit_notice": "independent reproduction of the documented legacy codec",
               "personal_profile_accessed": False, "application_launched": False,
               "compile_exit": compile_run.returncode, "generation_exit": run.returncode}
    (here / "provenance.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n")
    print(run.stdout.strip())
    print("provenance.json written; source/libraries/expected hashes recorded")

if __name__ == "__main__":
    main()


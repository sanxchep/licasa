#!/usr/bin/env python3
"""Check actual viewer close/reopen behavior and release of large image buffers."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def measure(executable, sample, cycles):
    with tempfile.TemporaryDirectory(prefix="licasa-viewer-lifecycle-") as temporary:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   QSG_RENDER_LOOP="basic", XDG_CONFIG_HOME=temporary + "/config",
                   XDG_CACHE_HOME=temporary + "/cache")
        command = [str(executable), "--viewer", "--cycles", str(cycles)]
        if sample:
            sample_path = Path(sample)
            command.append(str(sample_path if sample_path.is_absolute() else ROOT / sample_path))
        run = subprocess.run(command, env=env, text=True, capture_output=True, timeout=75)
        if run.returncode:
            raise RuntimeError(f"Viewer failed for {sample}: {run.stdout}\n{run.stderr}")
        if "Binding loop detected" in run.stderr:
            raise RuntimeError(f"Viewer has a binding loop for {sample}: {run.stderr}")
        result = json.loads(run.stdout)
        if "error" in result or result.get("image_failed"):
            raise RuntimeError(f"Viewer failed for {sample}: {result}")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--heif", action="store_true")
    parser.add_argument("--heif-sequence", type=Path,
                        help="External HEIF/HEIC sequence fixture to include in lifecycle qualification")
    parser.add_argument("--jxl", action="store_true")
    parser.add_argument("--raw", action="store_true")
    parser.add_argument("--apng", action="store_true")
    parser.add_argument("--jobs", type=int, default=1,
                        help="Run independent sample lifecycle probes in parallel")
    parser.add_argument("--sanitized", action="store_true", help="Allocator quarantine makes RSS an unsuitable pass/fail metric")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    executable = args.executable.resolve(strict=True)
    empty = measure(executable, None, 1)
    baseline_process = empty["idle_after_close_process"]
    baseline_rss = baseline_process["rss_kib"]
    baseline_live = baseline_process.get("malloc_live_kib")
    memory_metric = "malloc_live_kib" if baseline_live is not None else "rss_kib"
    baseline = baseline_live if baseline_live is not None else baseline_rss
    samples = ["tests/test-assets/formats/core-matrix/jpeg/uhd-8k-7680x4320.jpg"]
    if args.heif:
        samples += ["tests/test-assets/modern/heif/grid-48mp.heic", "tests/test-assets/modern/heif/single-48mp.heic"]
    if args.heif_sequence:
        samples.append(str(args.heif_sequence.resolve(strict=True)))
    if args.jxl:
        samples += ["tests/test-assets/modern/jxl/large-48mp.jxl", "tests/test-assets/modern/jxl/animation-100-4k.jxl"]
    if args.raw:
        samples += ["tests/test-assets/modern/raw/large-48mp.dng"]
    if args.apng:
        samples += ["tests/test-assets/animated/apng/hd-transparent-1280x720.apng"]
    report = {
        "baseline": empty,
        "memory_checks_enabled": not args.sanitized,
        "memory_gate_metric": memory_metric,
        "rss_checks_enabled": not args.sanitized,  # compatibility with older reports
        "jobs": min(args.jobs, len(samples)) if samples else 1,
        "samples": {},
    }

    def verify_sample(sample):
        result = measure(executable, sample, 3)
        cycles = result["memory_cycles"]
        if len(cycles) != 3:
            raise RuntimeError(f"Incomplete close/reopen verification: {sample}")
        for cycle in cycles:
            retained = cycle["idle_process"].get("retained_gpu_image_bytes")
            if retained != 0:
                raise RuntimeError(
                    f"GPU edit buffers remain after close: {sample}: {retained} bytes")
        # The rasters are 126-183 MiB. Preserve the existing 32 MiB live-
        # allocation headroom, but do not infer ownership from total RSS on
        # glibc: free worker-arena top chunks may remain resident after
        # malloc_trim(). mallinfo2() reports those bytes as free rather than
        # live, while a retained full raster remains far above this bound.
        # Sanitizers use their own lifetime/leak checks because quarantine
        # changes both RSS and allocator accounting.
        if not args.sanitized:
            for cycle in cycles:
                idle = cycle["idle_process"]
                current = idle.get(memory_metric)
                if current is None:
                    current = idle["rss_kib"]
                if current > baseline + 32 * 1024:
                    raise RuntimeError(
                        f"Large image allocation remains live after close: {sample}: "
                        f"metric={memory_metric} baseline={baseline:.0f} KiB "
                        f"idle={current:.0f} KiB rss={idle['rss_kib']:.0f} KiB: {cycle}"
                    )
        return sample, result

    worker_count = min(args.jobs, len(samples)) if samples else 1
    if worker_count == 1:
        measured = map(verify_sample, samples)
        executor = None
    else:
        executor = ThreadPoolExecutor(max_workers=worker_count,
                                      thread_name_prefix="licasa-lifecycle")
        measured = executor.map(verify_sample, samples)

    try:
        for sample, result in measured:
            report["samples"][sample] = result
    finally:
        if executor is not None:
            executor.shutdown(wait=True, cancel_futures=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified three close/reopen cycles for {len(samples)} large images "
          f"using {worker_count} parallel lifecycle job(s).")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Verify RAW preview/promotion decisions and heartbeat in the real QML viewer."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--vendor-dir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    cases = [
        (ROOT / 'tests/test-assets/modern/raw/small.dng', 25, True, False, True),
        (ROOT / 'tests/test-assets/modern/raw/no-preview.dng', 25, False, False, True),
        (ROOT / 'tests/test-assets/modern/raw/large-48mp.dng', 25, True, False, False),
    ]
    if args.vendor_dir:
        cases.extend((args.vendor_dir / name, 100, True, False, name != 'samsung-s22.dng')
                     for name in ['apple-iphone-12-pro.dng', 'google-pixel-6-pro.dng',
                                  'google-pixel-8-pro-50mp.dng', 'samsung-s22.dng'])
    report = {}
    for sample, limit, preview_available, fast_intermediate, develop in cases:
        with tempfile.TemporaryDirectory(prefix='licasa-raw-behavior-') as tmp:
            env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software',
                       QSG_RENDER_LOOP='basic', XDG_CONFIG_HOME=tmp + '/config',
                       XDG_CACHE_HOME=tmp + '/cache', LICASA_TRACE_DECODE_CACHE='1')
            command = [str(args.executable.resolve()), '--viewer', '--raw-behavior', '--megapixels', str(limit), str(sample.resolve())]
            if sample.name == 'samsung-s22.dng':
                command.append('--raw-preview-only')
            run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=40)
            if run.returncode != 0:
                raise RuntimeError(
                    f"RAW diagnostics failed for {sample.name} with exit code {run.returncode}\n"
                    f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            try:
                result = json.loads(run.stdout)
            except json.JSONDecodeError as error:
                raise RuntimeError(
                    f"RAW diagnostics produced invalid JSON for {sample.name}\n"
                    f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}") from error
            report[sample.name] = result
            assert 'error' not in result, (sample, result, run.stderr)
            assert 'Binding loop detected' not in run.stderr, run.stderr
            assert result['initial_preview_available'] == preview_available, result
            assert result.get('raw_fast_intermediate_ready', False) == fast_intermediate, result
            if sample.name == 'no-preview.dng':
                assert result['first_raster_full_detail'], result
            assert result['preview_max_heartbeat_gap_ms'] < 100, result
            if sample.name == 'samsung-s22.dng':
                assert result['preview_only_verified'], result
            elif develop:
                assert result['automatic_full_detail_ready'], result
                assert result['automatic_full_detail_ms'] > 0, result
                assert result['explicit_full_detail_ready'], result
                assert result['full_edit_kept_detail'], result
                assert result['full_first_edit_ready_ms'] > 0, result
                assert result['full_repeat_edit_ready_ms'] > 0, result
                full_cache_lines = [line for line in run.stderr.splitlines()
                                    if 'Decoded base' in line and line.endswith(', full)')]
                assert any('Decoded base cached:' in line for line in full_cache_lines), run.stderr
                assert sum('Decoded base cache hit:' in line for line in full_cache_lines) >= 2, run.stderr
                assert result['full_max_heartbeat_gap_ms'] < 250, result
            elif limit == 25:
                assert result['full_detail_blocked_by_policy'], result
        print(f'RAW viewer behavior verified: {sample.name}', flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

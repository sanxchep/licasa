#!/usr/bin/env python3
"""Offline preparation must verify all inputs without a hidden network fallback."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from source_cache import fetch_archive
from build_codecs import prepare_archive_source, prepare_git_source, git_output, selected_names


class ArchiveDownloadTests(unittest.TestCase):
    payload = b"checksum-pinned archive bytes"

    def fetch(self, cache, *, url="https://example.invalid/source.tar.gz", **options):
        return fetch_archive(cache, url, hashlib.sha256(self.payload).hexdigest(), **options)

    def concurrent_downloads(self, distinct_names):
        barrier = threading.Barrier(2)
        payload = self.payload

        class Response(io.BytesIO):
            def __init__(self):
                super().__init__(payload)
                self.first_read = True

            def read(self, size=-1):
                if self.first_read:
                    self.first_read = False
                    barrier.wait(timeout=5)
                return super().read(size)

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            names = ["first.tar.gz", "second.tar.gz"] if distinct_names else ["source.tar.gz"] * 2
            with patch("source_cache.urlopen", side_effect=lambda *a, **k: Response()):
                with ThreadPoolExecutor(max_workers=2) as workers:
                    futures = [workers.submit(self.fetch, cache, name=name) for name in names]
                    paths = [future.result(timeout=10) for future in futures]
            self.assertEqual({p.name for p in cache.iterdir()}, set(names))
            for path in paths:
                self.assertEqual(path.read_bytes(), self.payload)

    def test_concurrent_downloads_of_same_archive(self):
        self.concurrent_downloads(distinct_names=False)

    def test_concurrent_downloads_of_distinct_archives(self):
        self.concurrent_downloads(distinct_names=True)

    def test_checksum_failure_is_not_published_and_can_be_retried(self):
        for algorithm in ["sha256", "sha512"]:
            with self.subTest(algorithm=algorithm), tempfile.TemporaryDirectory() as temporary:
                cache = Path(temporary)
                digest = hashlib.new(algorithm, self.payload).hexdigest()
                with patch("source_cache.urlopen", side_effect=[io.BytesIO(b"bad"), io.BytesIO(self.payload)]):
                    with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                        fetch_archive(cache, "https://example.invalid/source.tar.gz", digest, algorithm=algorithm)
                    self.assertEqual(list(cache.iterdir()), [])
                    result = fetch_archive(cache, "https://example.invalid/source.tar.gz", digest, algorithm=algorithm)
                self.assertEqual(result.read_bytes(), self.payload)
                self.assertEqual(list(cache.iterdir()), [result])

    def test_interrupted_download_leaves_no_cache_or_temporary(self):
        class Interrupted(io.BytesIO):
            def read(self, size=-1):
                if self.tell():
                    raise OSError("connection interrupted")
                return super().read(size)

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            with patch("source_cache.urlopen", return_value=Interrupted(self.payload)):
                with self.assertRaisesRegex(OSError, "connection interrupted"):
                    self.fetch(cache)
            self.assertEqual(list(cache.iterdir()), [])

    def test_download_bound_leaves_no_cache_or_temporary(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            with patch("source_cache.urlopen", return_value=io.BytesIO(self.payload)):
                with self.assertRaisesRegex(RuntimeError, "exceeded size bound"):
                    self.fetch(cache, limit=len(self.payload) - 1)
            self.assertEqual(list(cache.iterdir()), [])

    def test_cached_archive_still_obeys_bound_without_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive = cache / "source.tar.gz"
            archive.write_bytes(self.payload)
            with patch("source_cache.urlopen", side_effect=AssertionError("network attempted")):
                for offline in [False, True]:
                    with self.subTest(offline=offline), self.assertRaisesRegex(RuntimeError, "exceeded size bound"):
                        self.fetch(cache, offline=offline, limit=len(self.payload) - 1)
            self.assertEqual(archive.read_bytes(), self.payload)

    def test_bad_concurrent_download_cannot_corrupt_successful_writer(self):
        started = threading.Event()
        release = threading.Event()

        class DelayedBadResponse(io.BytesIO):
            def read(self, size=-1):
                started.set()
                if not release.wait(timeout=5):
                    raise TimeoutError("valid writer did not finish")
                return super().read(size)

        def response(url, **options):
            return DelayedBadResponse(b"bad") if url.endswith("/bad") else io.BytesIO(self.payload)

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            with patch("source_cache.urlopen", side_effect=response):
                with ThreadPoolExecutor(max_workers=1) as workers:
                    failed = workers.submit(self.fetch, cache, url="https://example.invalid/bad", name="source.tar.gz")
                    try:
                        self.assertTrue(started.wait(timeout=5))
                        result = self.fetch(cache, name="source.tar.gz")
                    finally:
                        release.set()
                    with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                        failed.result(timeout=10)
            self.assertEqual(result.read_bytes(), self.payload)
            self.assertEqual(list(cache.iterdir()), [result])

    @unittest.skipUnless(os.name == "posix", "POSIX archive permissions")
    def test_download_preserves_callers_archive_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            previous_mask = os.umask(0o027)
            try:
                with patch("source_cache.urlopen", return_value=io.BytesIO(self.payload)):
                    result = self.fetch(cache)
            finally:
                os.umask(previous_mask)
            self.assertEqual(result.stat().st_mode & 0o777, 0o640)


class OfflineSourceTests(unittest.TestCase):
    def test_missing_and_corrupt_archives_never_download(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            with patch("source_cache.urlopen", side_effect=AssertionError("network attempted")):
                with self.assertRaisesRegex(RuntimeError, "Offline source input missing"):
                    fetch_archive(cache, "https://example.invalid/source.tar.gz", "0" * 64,
                                  offline=True)
                (cache / "source.tar.gz").write_bytes(b"corrupted cache")
                with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                    fetch_archive(cache, "https://example.invalid/source.tar.gz", "0" * 64,
                                  offline=True)

    def test_verified_archive_prepares_a_fresh_tree_and_rechecks_cached_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            archive = base / "fixture-1.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                info = tarfile.TarInfo("fixture/README")
                info.size = 5
                bundle.addfile(info, io.BytesIO(b"input"))
            spec = {"version": "1", "url": "https://example.invalid/fixture.tar.gz",
                    "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
            source = base / "fresh-source"
            with patch("source_cache.urlopen", side_effect=AssertionError("network attempted")):
                prepare_archive_source("fixture", spec, base, source, offline=True)
                self.assertEqual((source / "README").read_text(), "input")
                archive.write_bytes(b"changed after first extraction")
                with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                    prepare_archive_source("fixture", spec, base, source, offline=True)

    def test_pinned_git_cache_supports_fresh_offline_work_and_rejects_dirty_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            origin = base / "origin"
            origin.mkdir()
            subprocess.run(["git", "init", "--quiet", str(origin)], check=True)
            (origin / "input.txt").write_text("pinned input")
            subprocess.run(["git", "-C", str(origin), "add", "input.txt"], check=True)
            subprocess.run(["git", "-C", str(origin), "-c", "user.name=Fixture",
                            "-c", "user.email=fixture@example.invalid", "commit", "--quiet",
                            "-m", "Local offline source fixture"], check=True)
            spec = {"url": str(origin), "commit": git_output(origin, "rev-parse", "HEAD"),
                    "tree": git_output(origin, "rev-parse", "HEAD^{tree}")}
            cached = base / "cache/git" / f"fixture-{spec['commit']}"
            cached.parent.mkdir(parents=True)
            # Seed an independently prepared source, as a prefetcher would.
            subprocess.run(["git", "clone", "--quiet", str(origin), str(cached)], check=True)
            fresh = base / "fresh"
            prepare_git_source("fixture", spec, fresh, offline=True, source_cache=base / "cache")
            self.assertEqual((fresh / "input.txt").read_text(), "pinned input")
            (cached / "input.txt").write_text("dirty cache")
            with self.assertRaisesRegex(RuntimeError, "dirty pinned source"):
                prepare_git_source("fixture", spec, base / "another-fresh", offline=True,
                                   source_cache=base / "cache")

    def test_selection_includes_transitive_locked_requirements(self):
        lock = {"reader": {"requires": ["decoder"]}, "decoder": {"requires": ["pixels"]},
                "pixels": {}, "unrelated": {}}
        self.assertEqual(selected_names(lock, ["reader"]), {"reader", "decoder", "pixels"})
        with self.assertRaises(ValueError):
            selected_names(lock, ["not-locked"])


if __name__ == "__main__":
    unittest.main()

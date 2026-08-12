# -*- coding: utf-8 -*-
"""
NFS/S3 export-import stress workloads.

Public surface:
  WORKLOADS       — dict[name → factory(client, stop, nfs_mount_path, fatal_error_event)]
  DEFAULT_WORKLOAD — str
  WorkloadRunner  — convenience runner class
"""
import logging
import os
import shutil
import sys
import tempfile
import threading
import time

import ydb

from .helpers import get_s3_config, get_export_source_path
from .storage import FsStorageBackend, S3StorageBackend
from .roundtrip import ExportImportRoundtripWorkload
from .full_roundtrip import WorkloadFullRoundtrip

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Workload factories
# Each factory has the same signature:
#   (client, stop, nfs_mount_path, fatal_error_event) -> WorkloadBase
# nfs_mount_path may be None for S3-only workloads.
# ---------------------------------------------------------------------------

_NFS_WORKLOAD_NAMES = {"single_table", "full_roundtrip"}
_S3_WORKLOAD_NAMES = {"single_table_s3", "full_roundtrip_s3"}


def _make_single_table(client, stop, nfs_mount_path, fatal_error_event):
    backend = FsStorageBackend(client.driver, nfs_mount_path, log_prefix="single_table")
    return ExportImportRoundtripWorkload(
        client, stop, backend, fatal_error_event,
        workload_name="single_table",
        import_dest_prefix="imported",
    )


def _make_single_table_s3(client, stop, nfs_mount_path, fatal_error_event):
    source = get_export_source_path("large_test_table")
    s3 = get_s3_config(default_source_prefix=source.split("/")[-1])
    backend = S3StorageBackend(client.driver, s3, log_prefix="single_table_s3")
    return ExportImportRoundtripWorkload(
        client, stop, backend, fatal_error_event,
        workload_name="single_table_s3",
        import_dest_prefix="imported_s3",
    )


def _make_full_roundtrip(client, stop, nfs_mount_path, fatal_error_event):
    backend = FsStorageBackend(client.driver, nfs_mount_path, log_prefix="nfs_full_roundtrip")
    return WorkloadFullRoundtrip(
        client, stop, backend, fatal_error_event,
        workload_name="nfs_full_roundtrip",
    )


def _make_full_roundtrip_s3(client, stop, nfs_mount_path, fatal_error_event):
    s3 = get_s3_config()
    backend = S3StorageBackend(client.driver, s3, log_prefix="s3_full_roundtrip")
    return WorkloadFullRoundtrip(
        client, stop, backend, fatal_error_event,
        workload_name="s3_full_roundtrip",
    )


WORKLOADS = {
    "single_table": _make_single_table,
    "single_table_s3": _make_single_table_s3,
    "full_roundtrip": _make_full_roundtrip,
    "full_roundtrip_s3": _make_full_roundtrip_s3,
}

DEFAULT_WORKLOAD = "full_roundtrip"


# ---------------------------------------------------------------------------
# WorkloadRunner
# ---------------------------------------------------------------------------

class WorkloadRunner:
    def __init__(self, client, duration, workload_names=None):
        self.client = client
        self.duration = duration
        self.workload_names = workload_names or [DEFAULT_WORKLOAD]
        self._temp_nfs_dir = None
        ydb.interceptor.monkey_patch_event_handler()

    def _needs_nfs(self):
        return any(n in _NFS_WORKLOAD_NAMES for n in self.workload_names)

    def _setup_nfs(self):
        nfs_mount_path = os.getenv("NFS_MOUNT_PATH")
        logger.info("[setup] NFS_MOUNT_PATH=%s", nfs_mount_path)

        if not nfs_mount_path:
            self._temp_nfs_dir = tempfile.mkdtemp(prefix="nfs_stress_")
            nfs_mount_path = self._temp_nfs_dir
            logger.info("[setup] NFS_MOUNT_PATH not set, created temp dir: %s", nfs_mount_path)
        else:
            os.makedirs(nfs_mount_path, exist_ok=True)

        logger.info("[setup] NFS mount directory ready: %s", nfs_mount_path)
        return nfs_mount_path

    def _cleanup_temp_nfs(self):
        if self._temp_nfs_dir and os.path.exists(self._temp_nfs_dir):
            try:
                shutil.rmtree(self._temp_nfs_dir, ignore_errors=True)
                logger.info("[cleanup] Removed temp NFS dir: %s", self._temp_nfs_dir)
            except Exception as e:
                logger.warning("[cleanup] Failed to remove temp NFS dir %s: %s", self._temp_nfs_dir, e)
            self._temp_nfs_dir = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self._cleanup_temp_nfs()

    def run(self):
        logger.info("[runner] Starting workload, duration=%ds", self.duration)
        stop = threading.Event()
        fatal_error = threading.Event()

        nfs_mount_path = self._setup_nfs() if self._needs_nfs() else None

        workloads = []
        for name in self.workload_names:
            if name not in WORKLOADS:
                raise ValueError(f"Unknown workload: {name!r}. Known: {list(WORKLOADS)}")
            factory = WORKLOADS[name]
            w = factory(self.client, stop, nfs_mount_path, fatal_error)
            workloads.append(w)
            logger.info("[runner] Registered workload: %s", name)

        for w in workloads:
            w.start()
            logger.info("[runner] Started workload thread: %s", w.name)

        started_at = time.time()
        while time.time() - started_at < self.duration:
            if fatal_error.is_set():
                logger.error("[runner] Fatal error detected, stopping workload")
                break

            elapsed = int(time.time() - started_at)
            for w in workloads:
                stat = w.get_stat()
                msg = f"[runner] Elapsed {elapsed}s | {w.name}: {stat}"
                logger.info(msg)
                print(msg, file=sys.stderr)
            time.sleep(10)

        logger.info("[runner] Sending stop signal")
        stop.set()

        for w in workloads:
            logger.info("[runner] Waiting for %s to finish (timeout=30s)", w.name)
            w.join()
            if w.is_alive():
                logger.warning("[runner] %s did not stop within 30s", w.name)
            else:
                logger.info("[runner] %s finished", w.name)

        if fatal_error.is_set():
            logger.error("[runner] Workload terminated due to fatal error")
            raise RuntimeError("Workload failed due to export/import error")

        logger.info("[runner] All workloads stopped successfully")

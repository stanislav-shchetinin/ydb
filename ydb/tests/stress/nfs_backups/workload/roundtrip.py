# -*- coding: utf-8 -*-
"""
ExportImportWorkloadBase  — shared stats / retry / wait logic.
ExportImportRoundtripWorkload — unified single-source export→import roundtrip.
"""
import logging
import threading
import time
import uuid

from ydb import issues as ydb_issues
from ydb.operation import OperationClient
from ydb.tests.stress.common.common import WorkloadBase

from .helpers import (
    get_encryption_config,
    get_export_source_path,
    ensure_source_exists,
    snapshot_table_row_counts,
    table_row_count,
    rel_to_database,
)

logger = logging.getLogger(__name__)

_TRANSIENT_ERRORS = (
    ydb_issues.ConnectionError,
    ydb_issues.Unavailable,
    ydb_issues.Overloaded,
    ydb_issues.Timeout,
    ydb_issues.Undetermined,
    ydb_issues.Aborted,
    ydb_issues.SessionBusy,
)


# ---------------------------------------------------------------------------
# Base class: stats / retry / wait_op / fatal
# ---------------------------------------------------------------------------

class ExportImportWorkloadBase(WorkloadBase):
    """Base for all export/import workloads.

    No storage-specific state here — just stats, retry helper, wait_op, and
    fatal-error signalling.
    """

    def __init__(self, client, stop, fatal_error_event, workload_name, extra_stats=None):
        super().__init__(client, "", workload_name, stop)
        self.lock = threading.Lock()
        self.fatal_error_event = fatal_error_event
        self.op_client = OperationClient(client.driver)
        self.encryption = get_encryption_config()

        self._stats = {
            "export_started": 0,
            "export_done": 0,
            "export_error": 0,
            "import_started": 0,
            "import_done": 0,
            "import_error": 0,
        }
        if extra_stats:
            self._stats.update(extra_stats)
        if self.encryption:
            self._stats["encrypted"] = 1
            logger.info(
                "[%s] Encryption enabled: algorithm=%s key_len=%d",
                workload_name, self.encryption["algorithm"], len(self.encryption["key"]),
            )

    @property
    def _log_prefix(self):
        return self.name

    def _should_stop(self):
        return self.is_stop_requested() or self.fatal_error_event.is_set()

    def get_stat(self):
        with self.lock:
            return ", ".join(f"{k}={v}" for k, v in self._stats.items())

    def _inc_stat(self, key):
        with self.lock:
            self._stats[key] += 1

    def _signal_fatal_error(self, message):
        logger.error("[%s][FATAL] %s", self._log_prefix, message)
        self.fatal_error_event.set()

    def _op_forget(self, op_id):
        try:
            self.op_client.forget(op_id)
        except Exception:
            pass

    def _wait_op(self, op_id, poll_fn):
        """Poll until terminal state. Returns status string or None if stopped."""
        while True:
            if self._should_stop():
                return None
            status = poll_fn(op_id)
            if status is not None:
                self._op_forget(op_id)
                return status
            time.sleep(30)

    def _retry(self, action, description):
        """Retry action indefinitely on transient errors, until stopped or success."""
        attempt = 0
        while not self._should_stop():
            try:
                return action()
            except _TRANSIENT_ERRORS as e:
                attempt += 1
                delay = min(2 ** attempt, 60)
                logger.warning(
                    "[%s] %s transient error (attempt %d, retry in %ds): %s: %s",
                    self._log_prefix, description, attempt, delay, type(e).__name__, e,
                )
                time.sleep(delay)
            except Exception as e:
                # Non-transient error — surface it immediately.
                raise
        return None


# ---------------------------------------------------------------------------
# Unified single-source roundtrip workload
# ---------------------------------------------------------------------------

class ExportImportRoundtripWorkload(ExportImportWorkloadBase):
    """
    Generic export→import roundtrip for a single source path (table or directory).

    Storage details are fully encapsulated in `backend` (FsStorageBackend or
    S3StorageBackend). All features — encryption, source_path, row-count
    verification, cleanup — work identically for both.
    """

    DEFAULT_SOURCE = "large_test_table"

    def __init__(self, client, stop, backend, fatal_error_event, workload_name,
                 default_source=None, import_dest_prefix=None):
        extra = {"row_count_ok": 0, "row_count_mismatch": 0}
        super().__init__(client, stop, fatal_error_event, workload_name, extra_stats=extra)
        self._backend = backend
        self.source_path = get_export_source_path(default_source or self.DEFAULT_SOURCE)
        self._import_dest_prefix = import_dest_prefix or "imported"
        self.source_row_counts = None

    # ------------------------------------------------------------------
    # Row count verification
    # ------------------------------------------------------------------

    def _verify_imported_row_counts(self, import_dest: str) -> bool:
        if not self.source_row_counts:
            self._signal_fatal_error("source_row_counts is empty — nothing to verify")
            return False

        mismatches = []
        for rel, expected in sorted(self.source_row_counts.items()):
            imported_path = f"{import_dest.rstrip('/')}/{rel}"
            try:
                actual = table_row_count(self.client, imported_path)
            except Exception as e:
                mismatches.append(f"{rel}: cannot COUNT imported `{imported_path}`: {e}")
                continue
            if actual != expected:
                mismatches.append(
                    f"{rel}: expected={expected} actual={actual} (imported `{imported_path}`)"
                )
            else:
                logger.info("[%s][verify] OK `%s` row_count=%d", self._log_prefix, rel, actual)

        if mismatches:
            self._inc_stat("row_count_mismatch")
            msg = "Row count mismatch after import:\n  " + "\n  ".join(mismatches)
            logger.error("[%s] %s", self._log_prefix, msg)
            self._signal_fatal_error(msg)
            return False

        self._inc_stat("row_count_ok")
        logger.info(
            "[%s][verify] All %d imported tables match source row counts",
            self._log_prefix, len(self.source_row_counts),
        )
        return True

    # ------------------------------------------------------------------
    # Main loop steps
    # ------------------------------------------------------------------

    def _do_export(self, run_id: str):
        """Start export and wait for it. Returns location or None on stop/error."""
        result = self._retry(
            lambda: self._backend.start_export(self.source_path, run_id, self.encryption),
            "Export start",
        )
        if result is None:
            return None
        if isinstance(result, Exception):
            self._inc_stat("export_error")
            self._signal_fatal_error(f"Export start failed for run_id={run_id}: {result}")
            return None

        op, location = result
        self._inc_stat("export_started")
        logger.info("[%s] Export started: op=%s location=%s", self._log_prefix, op.id, location)

        status = self._wait_op(op.id, self._backend.poll_export)
        if status is None:
            return None

        if status != "DONE":
            self._inc_stat("export_error")
            msg = f"Export FAILED: op={op.id} status={status} location={location}"
            logger.error("[%s] %s. NOT cleaning up for investigation.", self._log_prefix, msg)
            self._signal_fatal_error(msg)
            return None

        self._inc_stat("export_done")
        logger.info("[%s] Export DONE: op=%s", self._log_prefix, op.id[:16])
        return location

    def _do_import(self, location: str, run_id: str) -> str | None:
        """Start import, wait for it, verify row counts. Returns import_dest or None."""
        db_path = self.client.database.rstrip("/")
        import_dest = f"{db_path}/{self._import_dest_prefix}_{run_id}"

        result = self._retry(
            lambda: self._backend.start_import(location, import_dest, run_id, self.encryption),
            "Import start",
        )
        if result is None:
            return None
        if isinstance(result, Exception):
            self._inc_stat("import_error")
            self._signal_fatal_error(f"Import start failed for run_id={run_id}: {result}")
            return None

        op = result
        self._inc_stat("import_started")
        logger.info("[%s] Import started: op=%s dest=%s", self._log_prefix, op.id, import_dest)

        status = self._wait_op(op.id, self._backend.poll_import)
        if status is None:
            return None

        if status != "DONE":
            self._inc_stat("import_error")
            msg = f"Import FAILED: op={op.id} status={status}"
            logger.error("[%s] %s. NOT cleaning up for investigation.", self._log_prefix, msg)
            self._signal_fatal_error(msg)
            return None

        self._inc_stat("import_done")
        logger.info("[%s] Import DONE: op=%s", self._log_prefix, op.id[:16])
        return import_dest

    # ------------------------------------------------------------------
    # Entry point
    # ------------------------------------------------------------------

    def _main_loop(self):
        logger.info(
            "[%s] Starting roundtrip, source=%s encrypted=%s",
            self._log_prefix, self.source_path, bool(self.encryption),
        )

        try:
            self.source_path = ensure_source_exists(self.client, self.source_path)
            self.source_row_counts = snapshot_table_row_counts(self.client, self.source_path)
            logger.info(
                "[%s] Snapshotted row counts for %d source table(s)",
                self._log_prefix, len(self.source_row_counts),
            )
        except Exception as e:
            self._signal_fatal_error(f"Cannot prepare source / row counts: {e}")
            return

        iteration = 0
        while not self._should_stop():
            iteration += 1
            run_id = str(uuid.uuid1()).replace("-", "_")
            logger.info("[%s] === Iteration %d run_id=%s source=%s ===",
                        self._log_prefix, iteration, run_id[:16], self.source_path)

            location = self._do_export(run_id)
            if location is None:
                return

            import_dest = self._do_import(location, run_id)
            if import_dest is None:
                return

            if not self._verify_imported_row_counts(import_dest):
                # Keep data for investigation.
                return

            self._backend.cleanup_import_dest(import_dest, self.client)
            self._backend.cleanup_location(location)

        logger.info("[%s] Stopped after %d iterations", self._log_prefix, iteration)

    def get_workload_thread_funcs(self):
        return [self._main_loop]

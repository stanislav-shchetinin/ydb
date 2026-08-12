# -*- coding: utf-8 -*-
"""
StorageBackend abstraction and concrete FS / S3 implementations.

Each backend exposes a uniform interface for start/poll/cleanup of
export and import operations, hiding storage-specific details.
"""
import logging
import os
import shutil

from ydb import issues as ydb_issues
from ydb.operation import OperationClient
from ydb.export import ExportClient, ExportToS3Operation
from ydb.import_client import ImportClient, ImportFromS3Operation

try:
    from ydb.public.api.protos import ydb_export_pb2
    from ydb.public.api.protos import ydb_import_pb2
    from ydb.public.api.grpc import ydb_export_v1_pb2_grpc
    from ydb.public.api.grpc import ydb_import_v1_pb2_grpc
except ImportError:
    from contrib.ydb.public.api.protos import ydb_export_pb2
    from contrib.ydb.public.api.protos import ydb_import_pb2
    from contrib.ydb.public.api.grpc import ydb_export_v1_pb2_grpc
    from contrib.ydb.public.api.grpc import ydb_import_v1_pb2_grpc

from .helpers import (
    apply_encryption_settings,
    apply_compression_settings,
    apply_include_index_data,
    apply_index_population_mode,
    build_s3_export_prefix,
)
from . import _fs_client as _fs_mod

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

_EXPORT_PROGRESSES: dict = {}
_IMPORT_PROGRESSES: dict = {}


def _init_progresses():
    for key, value in ydb_export_pb2.ExportProgress.Progress.items():
        _EXPORT_PROGRESSES[value] = key[len("PROGRESS_"):]
    for key, value in ydb_import_pb2.ImportProgress.Progress.items():
        _IMPORT_PROGRESSES[value] = key[len("PROGRESS_"):]


_init_progresses()


# ---------------------------------------------------------------------------
# StorageBackend ABC
# ---------------------------------------------------------------------------

class StorageBackend:
    """
    Abstract interface for a storage backend.

    Concrete implementations must override all methods.

    Convention:
      - start_export / start_import return an operation object with .id attribute.
      - poll_export / poll_import return a status string ("DONE", "CANCELLED", …)
        when the operation has reached a terminal state, or None while still running.
        Transient errors are swallowed and None is returned so the caller retries.
      - cleanup_location removes backend-side export artifacts (S3 prefix / FS dir).
    """

    def start_export(self, source_path: str, run_id: str, encryption: dict | None,
                     compression: str | None = None, include_index_data: bool = False):
        """Start an export operation. Returns (op, location) where location identifies
        the storage destination (s3_prefix or fs base_path)."""
        raise NotImplementedError

    def poll_export(self, op_id: str):
        """Return terminal status or None if still running."""
        raise NotImplementedError

    def start_import(self, location: str, import_dest: str, run_id: str, encryption: dict | None,
                     include_index_data: bool = False):
        """Start an import operation from location into import_dest. Returns op."""
        raise NotImplementedError

    def poll_import(self, op_id: str):
        """Return terminal status or None if still running."""
        raise NotImplementedError

    def cleanup_location(self, location: str):
        """Remove backend-side export artifacts."""
        raise NotImplementedError

    def cleanup_import_dest(self, import_dest: str, client):
        """Remove imported DB objects. Default: remove_recursively + DROP TABLE fallback."""
        try:
            client.remove_recursively(import_dest)
            logger.info("[cleanup] Removed imported path: %s", import_dest)
            return
        except Exception as e:
            logger.warning("[cleanup] remove_recursively(%s) failed: %s", import_dest, e)
        try:
            client.query(f"DROP TABLE `{import_dest}`;", True)
            logger.info("[cleanup] Dropped imported table: %s", import_dest)
        except Exception as e:
            logger.warning("[cleanup] Failed to drop table %s: %s", import_dest, e)


# ---------------------------------------------------------------------------
# FS backend
# ---------------------------------------------------------------------------

class FsStorageBackend(StorageBackend):
    """
    Export/import using the filesystem (NFS mount or temp dir).

    Export always uses destination_prefix = base_path.
    Import uses destination_path = import_dest (SchemaMapping),
    which mirrors the S3 backend so directory exports + encryption work correctly.
    """

    def __init__(self, driver, nfs_mount_path: str, log_prefix: str = ""):
        self._driver = driver
        self._nfs_mount_path = nfs_mount_path
        self._log_prefix = log_prefix
        self._fs = _fs_mod.FsExportClient(driver)

    def start_export(self, source_path: str, run_id: str, encryption, compression=None,
                     include_index_data=False):
        base_path = os.path.join(self._nfs_mount_path, f"export_{run_id}")
        op = self._fs.export_to_fs(
            base_path=base_path,
            items=[(source_path, source_path)],
            description=f"stress_export_{run_id}",
            encryption=encryption,
            compression=compression,
            include_index_data=include_index_data,
        )
        return op, base_path

    def poll_export(self, op_id: str):
        try:
            op = self._fs.get_export_operation(op_id)
            logger.debug("[%s][fs][export] Poll op=%s ready=%s progress=%s",
                         self._log_prefix, op_id, op.ready, op.progress)
            if op.ready:
                return op.progress if op.progress != "UNSPECIFIED" else "DONE"
            return None
        except ydb_issues.NotFound:
            logger.debug("[%s][fs][export] Poll op=%s: NOT_FOUND (treating as DONE)",
                         self._log_prefix, op_id)
            return "DONE"
        except _TRANSIENT_ERRORS as e:
            logger.warning("[%s][fs][export] Poll op=%s transient error: %s", self._log_prefix, op_id, e)
            return None

    def start_import(self, location: str, import_dest: str, run_id: str, encryption,
                     include_index_data=False):
        op = self._fs.import_from_fs(
            base_path=location,
            destination_path=import_dest,
            description=f"stress_import_{run_id}",
            encryption=encryption,
            include_index_data=include_index_data,
        )
        return op

    def poll_import(self, op_id: str):
        try:
            op = self._fs.get_import_operation(op_id)
            logger.debug("[%s][fs][import] Poll op=%s ready=%s progress=%s",
                         self._log_prefix, op_id, op.ready, op.progress)
            if op.ready:
                return op.progress if op.progress != "UNSPECIFIED" else "DONE"
            return None
        except ydb_issues.NotFound:
            logger.debug("[%s][fs][import] Poll op=%s: NOT_FOUND (treating as DONE)",
                         self._log_prefix, op_id)
            return "DONE"
        except _TRANSIENT_ERRORS as e:
            logger.warning("[%s][fs][import] Poll op=%s transient error: %s", self._log_prefix, op_id, e)
            return None

    def cleanup_location(self, location: str):
        try:
            if os.path.exists(location):
                shutil.rmtree(location, ignore_errors=True)
                logger.info("[%s][fs][cleanup] Removed export dir: %s", self._log_prefix, location)
        except Exception as e:
            logger.warning("[%s][fs][cleanup] Failed to remove %s: %s", self._log_prefix, location, e)


# ---------------------------------------------------------------------------
# S3 backend
# ---------------------------------------------------------------------------

class S3StorageBackend(StorageBackend):
    """Export/import using S3-compatible object storage."""

    def __init__(self, driver, s3_config: dict, log_prefix: str = ""):
        self._driver = driver
        self._s3 = s3_config
        self._log_prefix = log_prefix
        self._export_client = ExportClient(driver)
        self._import_client = ImportClient(driver)
        endpoint = (s3_config.get("endpoint") or "").lower()
        self._scheme = 1 if endpoint.startswith("http://") else 2

    def start_export(self, source_path: str, run_id: str, encryption, compression=None,
                     include_index_data=False):
        s3_prefix = build_s3_export_prefix(run_id)
        request = ydb_export_pb2.ExportToS3Request(
            settings=ydb_export_pb2.ExportToS3Settings(
                endpoint=self._s3["endpoint"],
                bucket=self._s3["bucket"],
                access_key=self._s3["access_key"],
                secret_key=self._s3["secret_key"],
                scheme=self._scheme,
                number_of_retries=3,
                destination_prefix=s3_prefix,
            )
        )
        request.settings.items.add(source_path=source_path)
        apply_encryption_settings(request.settings, encryption)
        apply_compression_settings(request.settings, compression)
        apply_include_index_data(request.settings, include_index_data)
        op = self._driver(
            request,
            ydb_export_v1_pb2_grpc.ExportServiceStub,
            "ExportToS3",
            ExportToS3Operation,
            None,
            (self._driver,),
        )
        return op, s3_prefix

    def poll_export(self, op_id: str):
        try:
            op = self._export_client.get_export_to_s3_operation(op_id)
            progress = op.progress.name if op.progress is not None else "UNKNOWN"
            logger.debug("[%s][s3][export] Poll op=%s progress=%s", self._log_prefix, op_id, progress)
            if progress in ("DONE", "CANCELLED", "UNSPECIFIED"):
                return progress if progress != "UNSPECIFIED" else "DONE"
            return None
        except ydb_issues.NotFound:
            logger.debug("[%s][s3][export] Poll op=%s: NOT_FOUND (treating as DONE)", self._log_prefix, op_id)
            return "DONE"
        except _TRANSIENT_ERRORS as e:
            logger.warning("[%s][s3][export] Poll op=%s transient error: %s", self._log_prefix, op_id, e)
            return None

    def start_import(self, location: str, import_dest: str, run_id: str, encryption,
                     include_index_data=False):
        request = ydb_import_pb2.ImportFromS3Request(
            settings=ydb_import_pb2.ImportFromS3Settings(
                endpoint=self._s3["endpoint"],
                bucket=self._s3["bucket"],
                access_key=self._s3["access_key"],
                secret_key=self._s3["secret_key"],
                scheme=self._scheme,
                number_of_retries=3,
                source_prefix=location,
                destination_path=import_dest,
            )
        )
        apply_encryption_settings(request.settings, encryption)
        apply_index_population_mode(request.settings, include_index_data)
        op = self._driver(
            request,
            ydb_import_v1_pb2_grpc.ImportServiceStub,
            "ImportFromS3",
            ImportFromS3Operation,
            None,
            (self._driver,),
        )
        return op

    def poll_import(self, op_id: str):
        try:
            op = self._import_client.get_import_from_s3_operation(op_id)
            progress = op.progress.name if op.progress is not None else "UNKNOWN"
            logger.debug("[%s][s3][import] Poll op=%s progress=%s", self._log_prefix, op_id, progress)
            if progress in ("DONE", "CANCELLED", "UNSPECIFIED"):
                return progress if progress != "UNSPECIFIED" else "DONE"
            return None
        except ydb_issues.NotFound:
            logger.debug("[%s][s3][import] Poll op=%s: NOT_FOUND (treating as DONE)", self._log_prefix, op_id)
            return "DONE"
        except _TRANSIENT_ERRORS as e:
            logger.warning("[%s][s3][import] Poll op=%s transient error: %s", self._log_prefix, op_id, e)
            return None

    def cleanup_location(self, location: str):
        try:
            import boto3
            resource = boto3.resource(
                "s3",
                endpoint_url=self._s3["endpoint"],
                aws_access_key_id=self._s3["access_key"],
                aws_secret_access_key=self._s3["secret_key"],
                region_name="us-east-1",
            )
            bucket = resource.Bucket(self._s3["bucket"])
            deleted = 0
            for obj in bucket.objects.filter(Prefix=location):
                obj.delete()
                deleted += 1
            logger.info("[%s][s3][cleanup] Removed %d objects under s3://%s/%s",
                        self._log_prefix, deleted, self._s3["bucket"], location)
        except Exception as e:
            logger.warning("[%s][s3][cleanup] Failed to remove prefix %s: %s", self._log_prefix, location, e)

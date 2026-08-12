# -*- coding: utf-8 -*-
"""Shared helpers: encryption config, S3 config, DB path utilities, row-count helpers."""
import logging
import os
import time

from ydb import issues as ydb_issues
from ydb.import_client import ImportClient, ImportFromS3Settings

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Encryption
# ---------------------------------------------------------------------------

# Default 32-byte key for ChaCha20-Poly1305 / AES-256-GCM (same as unit tests).
_DEFAULT_ENCRYPTION_KEY = b"Very very secret export key!!!!!"
_KEY_LENGTHS = {
    "AES-128-GCM": 16,
    "AES-256-GCM": 32,
    "ChaCha20-Poly1305": 32,
}


def get_encryption_config():
    """Return encryption config dict if enabled via env, otherwise None."""
    enabled = os.getenv("EXPORT_ENCRYPTION_ENABLED", "").lower() in ("1", "true", "yes")
    if not enabled:
        return None

    algorithm = os.getenv("EXPORT_ENCRYPTION_ALGORITHM", "ChaCha20-Poly1305")
    expected_len = _KEY_LENGTHS.get(algorithm)
    if expected_len is None:
        raise RuntimeError(
            f"Unsupported EXPORT_ENCRYPTION_ALGORITHM={algorithm!r}. "
            f"Supported: {', '.join(_KEY_LENGTHS)}"
        )

    hex_key = os.getenv("YDB_ENCRYPTION_KEY")
    if hex_key:
        try:
            key = bytes.fromhex(hex_key)
        except ValueError as e:
            raise RuntimeError(f"Invalid YDB_ENCRYPTION_KEY hex: {e}") from e
    else:
        raw = os.getenv("EXPORT_ENCRYPTION_KEY")
        key = raw.encode("utf-8") if raw is not None else _DEFAULT_ENCRYPTION_KEY

    if len(key) != expected_len:
        raise RuntimeError(
            f"Encryption key length is {len(key)} bytes, but {algorithm} requires {expected_len}"
        )

    return {"algorithm": algorithm, "key": key}


def apply_encryption_settings(proto_settings, encryption):
    """Write encryption_settings into a proto request settings object."""
    if not encryption:
        return
    enc = proto_settings.encryption_settings
    enc.encryption_algorithm = encryption["algorithm"]
    enc.symmetric_key.key = encryption["key"]


# ---------------------------------------------------------------------------
# Compression (export-only; import detects codec from SchemaMapping)
# ---------------------------------------------------------------------------

def get_compression_config():
    """
    Return compression codec string if set via EXPORT_COMPRESSION / --compression.
    Supported values (per ydb_export.proto): 'zstd' or 'zstd-N' (e.g. 'zstd-3').
    Returns None when compression is disabled.
    """
    raw = os.getenv("EXPORT_COMPRESSION", "").strip()
    if not raw:
        return None
    # Accept 'zstd' or 'zstd-<level>'
    if raw == "zstd" or (raw.startswith("zstd-") and raw[5:].isdigit()):
        return raw
    raise RuntimeError(
        f"Unsupported EXPORT_COMPRESSION={raw!r}. "
        "Supported: 'zstd' or 'zstd-N' (e.g. 'zstd-3')."
    )


def apply_compression_settings(proto_settings, compression):
    """Write compression codec into a proto export settings object."""
    if not compression:
        return
    proto_settings.compression = compression


# ---------------------------------------------------------------------------
# S3 config
# ---------------------------------------------------------------------------


def get_s3_config(default_source_prefix=None):
    """Read S3 credentials from env vars (same ones used to preload large_test_table)."""
    s3_endpoint = os.getenv("S3_ENDPOINT")
    s3_bucket = os.getenv("S3_BUCKET")
    s3_access_key = os.getenv("S3_ACCESS_KEY_ID")
    s3_secret_key = os.getenv("S3_ACCESS_KEY_SECRET")
    s3_source_prefix = os.getenv("S3_SOURCE_PREFIX", default_source_prefix)

    if not all([s3_endpoint, s3_bucket, s3_access_key, s3_secret_key]):
        missing = [
            name for name, val in [
                ("S3_ENDPOINT", s3_endpoint),
                ("S3_BUCKET", s3_bucket),
                ("S3_ACCESS_KEY_ID", s3_access_key),
                ("S3_ACCESS_KEY_SECRET", s3_secret_key),
            ] if not val
        ]
        raise RuntimeError(f"Missing S3 env vars: {', '.join(missing)}")

    return {
        "endpoint": s3_endpoint,
        "bucket": s3_bucket,
        "access_key": s3_access_key,
        "secret_key": s3_secret_key,
        "source_prefix": s3_source_prefix,
    }


def build_s3_export_prefix(run_id: str) -> str:
    """Build S3 destination prefix: [S3_EXPORT_PREFIX/]stress_s3_export_{run_id}."""
    base = f"stress_s3_export_{run_id}"
    custom = os.getenv("S3_EXPORT_PREFIX", "").strip().strip("/")
    if custom:
        return f"{custom}/{base}"
    return base


# ---------------------------------------------------------------------------
# DB path utilities
# ---------------------------------------------------------------------------


def resolve_db_path(client, path):
    """Resolve path relative to database root if not already absolute."""
    path = path.strip()
    if path.startswith("/"):
        return path
    return f"{client.database.rstrip('/')}/{path}"


def rel_to_database(client, full_path):
    """Return path relative to database root (no leading slash)."""
    db = client.database.rstrip("/")
    full = full_path.rstrip("/")
    prefix = db + "/"
    if full.startswith(prefix):
        return full[len(prefix):]
    if full == db:
        return ""
    return full.lstrip("/")


# ---------------------------------------------------------------------------
# Table listing and row counts
# ---------------------------------------------------------------------------


def list_tables_recursive(client, path):
    """Return full DB paths of all row/column tables under path."""
    desc = client.describe(path)
    if desc is None:
        return []
    if desc.is_table() or desc.is_column_table():
        return [path]
    if not desc.is_directory():
        return []

    tables = []
    listing = client.driver.scheme_client.list_directory(path)
    for entry in listing.children:
        if entry.name.startswith("."):
            continue
        entry_path = f"{path.rstrip('/')}/{entry.name}"
        if entry.is_table() or entry.is_column_table():
            tables.append(entry_path)
        elif entry.is_directory():
            tables.extend(list_tables_recursive(client, entry_path))
    return tables


def table_row_count(client, table_path):
    result = client.query(f"SELECT COUNT(*) AS cnt FROM `{table_path}`;", False)
    if not result or not result[0].rows:
        raise RuntimeError(f"COUNT(*) returned empty result for `{table_path}`")
    row = result[0].rows[0]
    if hasattr(row, "cnt"):
        return int(row.cnt)
    try:
        return int(row["cnt"])
    except Exception:
        return int(row[0])


def snapshot_table_row_counts(client, source_path):
    """
    Return {db_relative_path: row_count} for all tables under source_path.
    Keys are relative to database root — same layout used under import destination_path.
    """
    tables = list_tables_recursive(client, source_path)
    if not tables:
        raise RuntimeError(f"No tables found under source path '{source_path}'")

    counts = {}
    for full in sorted(tables):
        rel = rel_to_database(client, full)
        cnt = table_row_count(client, full)
        counts[rel] = cnt
        logger.info("[setup] Source table `%s` row_count=%d", rel, cnt)
    return counts


# ---------------------------------------------------------------------------
# Source existence / preload
# ---------------------------------------------------------------------------


def ensure_source_exists(client, source_path):
    """
    Ensure DB source path exists and return its resolved absolute path.
    For the special case 'large_test_table' missing from DB, attempts S3 preload.
    """
    full_path = resolve_db_path(client, source_path)
    desc = client.describe(full_path)
    if desc is not None:
        kind = "directory" if desc.is_directory() else "object"
        logger.info("[setup] Source %s '%s' exists", kind, full_path)
        return full_path

    # Backward-compatible preload only for the default single-table name.
    if source_path.rstrip("/").endswith("large_test_table") or source_path == "large_test_table":
        table_name = source_path.split("/")[-1]
        ensure_table_exists(client, table_name)
        desc = client.describe(resolve_db_path(client, table_name))
        if desc is not None:
            return resolve_db_path(client, table_name)

    raise RuntimeError(
        f"Export source '{full_path}' does not exist. "
        "Create the table/directory first, or set EXPORT_SOURCE_PATH to an existing path."
    )


def ensure_table_exists(client, table_name):
    """Check if table exists; if not, import it from S3 using env vars."""
    try:
        client.query(
            f"SELECT COUNT(*) AS cnt FROM `{table_name}` LIMIT 1;",
            False,
        )
        logger.info("[setup] Table '%s' already exists", table_name)
        return
    except Exception:
        logger.info("[setup] Table '%s' not found, will try to import from S3", table_name)

    s3 = get_s3_config(default_source_prefix=table_name)
    if not s3["source_prefix"]:
        raise RuntimeError(
            f"Table '{table_name}' does not exist and cannot import from S3: "
            "S3_SOURCE_PREFIX is not set"
        )

    db_path = client.database.rstrip("/")
    dest_path = f"{db_path}/{table_name}"

    settings = (
        ImportFromS3Settings()
        .with_endpoint(s3["endpoint"])
        .with_bucket(s3["bucket"])
        .with_access_key(s3["access_key"])
        .with_secret_key(s3["secret_key"])
        .with_number_of_retries(3)
        .with_source_and_destination(s3["source_prefix"], dest_path)
    )

    logger.info(
        "[setup] Importing table from S3: endpoint=%s bucket=%s prefix=%s -> %s",
        s3["endpoint"], s3["bucket"], s3["source_prefix"], dest_path,
    )

    import_client = ImportClient(client.driver)
    result = import_client.import_from_s3(settings)
    op_id = result.id
    logger.info("[setup] S3 import started: op=%s progress=%s", op_id, result.progress.name)

    while True:
        op = import_client.get_import_from_s3_operation(op_id)
        progress = op.progress.name
        if progress == "DONE":
            logger.info("[setup] S3 import DONE: op=%s", op_id)
            break
        elif progress == "CANCELLED":
            raise RuntimeError(f"S3 import cancelled: op={op_id}")
        logger.debug("[setup] S3 import in progress: op=%s progress=%s", op_id, progress)
        time.sleep(5)

    try:
        client.query(f"SELECT COUNT(*) AS cnt FROM `{table_name}` LIMIT 1;", False)
        logger.info("[setup] Table '%s' imported successfully", table_name)
    except Exception as e:
        raise RuntimeError(f"Table '{table_name}' not accessible after S3 import: {e}")


# ---------------------------------------------------------------------------
# Source path from env
# ---------------------------------------------------------------------------


def get_export_source_path(default="large_test_table"):
    """DB path (table or directory) to export. Relative to --database or absolute."""
    return os.getenv("EXPORT_SOURCE_PATH", default).strip().rstrip("/") or default

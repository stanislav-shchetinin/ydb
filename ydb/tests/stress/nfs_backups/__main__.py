# -*- coding: utf-8 -*-
import argparse
import logging
import os
import sys

from ydb.tests.stress.nfs_backups.workload import WorkloadRunner, WORKLOADS, DEFAULT_WORKLOAD
from ydb.tests.stress.common.instrumented_client import InstrumentedYdbClient

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    stream=sys.stderr,
)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="NFS/S3 export/import stress workload",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--endpoint", default="localhost:2135", help="An endpoint to be used")
    parser.add_argument("--database", default="Root/test", help="A database to connect")
    parser.add_argument("--duration", default=10 ** 9, type=lambda x: int(x), help="A duration of workload in seconds.")
    parser.add_argument(
        "--nfs-path",
        default=None,
        help="Path to NFS mount directory for export/import operations. "
             "If not specified, uses NFS_MOUNT_PATH environment variable."
    )
    parser.add_argument(
        "--workload",
        nargs="+",
        default=[DEFAULT_WORKLOAD],
        choices=list(WORKLOADS.keys()),
        help=f"Workload(s) to run. Available: {', '.join(WORKLOADS.keys())}. "
             f"Default: {DEFAULT_WORKLOAD}",
    )
    parser.add_argument(
        "--encrypted",
        action="store_true",
        help="Enable encrypted export/import. Uses EXPORT_ENCRYPTION_KEY / "
             "YDB_ENCRYPTION_KEY or a built-in test key (ChaCha20-Poly1305).",
    )
    parser.add_argument(
        "--encryption-algorithm",
        default=None,
        choices=["AES-128-GCM", "AES-256-GCM", "ChaCha20-Poly1305"],
        help="Encryption algorithm (default: ChaCha20-Poly1305). "
             "Also via EXPORT_ENCRYPTION_ALGORITHM.",
    )
    parser.add_argument(
        "--encryption-key",
        default=None,
        help="Raw encryption key string. AES-128-GCM needs 16 bytes; "
             "AES-256-GCM and ChaCha20-Poly1305 need 32 bytes. "
             "Also via EXPORT_ENCRYPTION_KEY (or YDB_ENCRYPTION_KEY as hex).",
    )
    args = parser.parse_args()

    if args.nfs_path:
        os.environ["NFS_MOUNT_PATH"] = args.nfs_path
    if args.encrypted:
        os.environ["EXPORT_ENCRYPTION_ENABLED"] = "1"
    if args.encryption_algorithm:
        os.environ["EXPORT_ENCRYPTION_ALGORITHM"] = args.encryption_algorithm
    if args.encryption_key is not None:
        os.environ["EXPORT_ENCRYPTION_KEY"] = args.encryption_key

    logger = logging.getLogger("nfs_backups")
    logger.info("Connecting to %s database=%s", args.endpoint, args.database)

    client = InstrumentedYdbClient(args.endpoint, args.database, True)
    client.wait_connection()
    logger.info("Connected successfully")

    try:
        with WorkloadRunner(client, args.duration, args.workload) as runner:
            runner.run()
    except Exception:
        logger.exception("Workload failed")
        raise
    finally:
        logger.info("Closing client")
        client.close()
        logger.info("Done")

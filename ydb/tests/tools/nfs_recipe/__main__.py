#!/usr/bin/env python
# -*- coding: utf-8 -*-
import logging
import os
import shutil
import tempfile

from library.python.testing.recipe import declare_recipe, set_env
import yatest.common

TMPDIR_FILE = "nfs_recipe_dir.txt"
NFS_FAULT_LIB = os.getenv("NFS_FAULT_LIB") or "ydb/tests/tools/nfs_recipe/nfs_fault/libnfs_fault.so"
DEFAULT_FAULT_RATE = "0.05"
# libnfs_fault writes to stderr (fd=2) by default; inside yatest that
# means ydbd's logs land in testing_out_stuff/ydbd.out.N, client-process
# logs — in nfs_backups.err. Set to "0" to silence, "1" for rewrites/faults.
DEFAULT_FAULT_LOG = "1"


def start(argv):
    logging.debug("Starting NFS recipe")

    fault_rate = DEFAULT_FAULT_RATE
    if argv:
        fault_rate = argv[0]

    tmpdir = tempfile.mkdtemp(prefix="nfs_recipe_")
    with open(TMPDIR_FILE, "w") as f:
        f.write(tmpdir)

    lib_path = yatest.common.binary_path(NFS_FAULT_LIB)
    existing = os.environ.get("LD_PRELOAD", "")
    ld_preload = f"{lib_path}:{existing}" if existing else lib_path

    fault_log = os.environ.get("NFS_FAULT_LOG", DEFAULT_FAULT_LOG)

    set_env("NFS_MOUNT_PATH", tmpdir)
    set_env("NFS_FAULT_PATH", tmpdir)
    set_env("NFS_FAULT_RATE", fault_rate)
    set_env("NFS_FAULT_LOG", fault_log)
    set_env("LD_PRELOAD", ld_preload)

    logging.debug(
        "NFS recipe started: mount=%s, fault_rate=%s, log=%s, ld_preload=%s",
        tmpdir, fault_rate, fault_log, ld_preload,
    )


def stop(argv):
    logging.debug("Stopping NFS recipe")
    try:
        with open(TMPDIR_FILE, "r") as f:
            tmpdir = f.read().strip()
        if tmpdir and os.path.isdir(tmpdir):
            shutil.rmtree(tmpdir, ignore_errors=True)
            logging.debug("NFS recipe: removed %s", tmpdir)
    except Exception as e:
        logging.warning("NFS recipe stop: %s", e)


if __name__ == "__main__":
    declare_recipe(start, stop)

# -*- coding: utf-8 -*-
import os
import pytest
import yatest

from ydb.tests.library.stress.fixtures import StressFixture


_RECIPE_ENV_VARS = (
    "NFS_MOUNT_PATH",
    "NFS_FAULT_PATH",
    "NFS_FAULT_RATE",
    "NFS_FAULT_LOG",
    "NFS_FAULT_LOG_FD",
    "LD_PRELOAD",
)


def _env_without_recipe():
    env = os.environ.copy()
    for k in _RECIPE_ENV_VARS:
        env.pop(k, None)
    return env


class TestYdbNfsWorkload(StressFixture):
    @pytest.fixture(autouse=True, scope="function")
    def setup(self):
        yield from self.setup_cluster(
            extra_feature_flags={
                "enable_fs_backups": True,
                "enable_export_auto_dropping": True,
                "enable_changefeeds_export": True,
            }
        )

    def test(self):
        cmd = [
            yatest.common.binary_path(os.getenv("YDB_TEST_PATH")),
            "--endpoint", self.endpoint,
            "--database", self.database,
            "--duration", self.base_duration,
        ]
        yatest.common.execute(cmd, wait=True, env=_env_without_recipe())

    def test_with_nfs_faults(self):
        """Runs the `full_roundtrip` workload under the nfs_recipe, which
        LD_PRELOADs libnfs_fault.so — an interceptor that randomly returns
        NFS-specific errors (ESTALE, EIO, ENOSPC, EDQUOT, EWOULDBLOCK,
        ETIMEDOUT, ENOLCK, EACCES) for operations on files inside the NFS
        mount directory.

        The workload is expected to survive injected faults thanks to the
        retry loop in export_s3_uploader / import_s3 (retryable S3 errors)
        and the cooldown window in the fault injector. Only `full_roundtrip`
        is used: it exercises tables, topics and views, so every export
        artifact kind goes through the fault path.

        NFS_MOUNT_PATH, NFS_FAULT_RATE and LD_PRELOAD are exported by the
        recipe; the workload picks up NFS_MOUNT_PATH from the environment
        itself, and ydbd inherits LD_PRELOAD automatically.
        """
        cmd = [
            yatest.common.binary_path(os.getenv("YDB_TEST_PATH")),
            "--endpoint", self.endpoint,
            "--database", self.database,
            "--duration", 3600,
            "--workload", "full_roundtrip",
        ]
        yatest.common.execute(cmd, wait=True)

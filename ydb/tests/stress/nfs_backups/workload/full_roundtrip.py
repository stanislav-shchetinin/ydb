# -*- coding: utf-8 -*-
"""
WorkloadFullRoundtrip — creates a temporary multi-object schema (tables, topics,
views), exports it, imports it into a different DB prefix, and cleans up.

Export/import I/O is fully delegated to a StorageBackend so the same class
works with both FS and S3 backends.

Import cleanup uses backend.cleanup_import_dest instead of explicit DROP
statements, which makes path handling uniform regardless of how the backend
maps the export structure to the destination.
"""
import logging
import uuid

from .roundtrip import ExportImportWorkloadBase

logger = logging.getLogger(__name__)


class WorkloadFullRoundtrip(ExportImportWorkloadBase):
    NUM_TABLES = 5
    NUM_ROWS = 100
    NUM_TOPICS = 3
    NUM_VIEWS = 3

    def __init__(self, client, stop, backend, fatal_error_event, workload_name="nfs_full_roundtrip"):
        super().__init__(client, stop, fatal_error_event, workload_name,
                         extra_stats={"iterations": 0})
        self._backend = backend

    # ------------------------------------------------------------------
    # Schema helpers
    # ------------------------------------------------------------------

    def _create_schema(self, prefix, table_names, topic_names, view_names):
        for name in table_names:
            if self._should_stop():
                return False
            self.client.query(
                f"""
                    CREATE TABLE `{name}` (
                        id Uint32 NOT NULL,
                        payload Utf8,
                        INDEX idx_payload GLOBAL SYNC ON (payload),
                        PRIMARY KEY (id)
                    );
                """,
                True,
            )
        logger.info("[%s] Created %d tables under %s", self._log_prefix, len(table_names), prefix)

        for name in topic_names:
            if self._should_stop():
                return False
            self.client.query(f"CREATE TOPIC `{name}`;", True)
            self.client.query(f"ALTER TOPIC `{name}` ADD CONSUMER consumer_a;", True)
        logger.info("[%s] Created %d topics under %s", self._log_prefix, len(topic_names), prefix)

        for i, name in enumerate(view_names):
            if self._should_stop():
                return False
            src_table = table_names[i % len(table_names)]
            self.client.query(
                f"CREATE VIEW `{name}` WITH security_invoker = TRUE AS SELECT * FROM `{src_table}`;",
                True,
            )
        logger.info("[%s] Created %d views under %s", self._log_prefix, len(view_names), prefix)
        return True

    def _insert_rows(self, table_names):
        for table in table_names:
            if self._should_stop():
                return False
            for batch_start in range(0, self.NUM_ROWS, 10):
                if self._should_stop():
                    return False
                values = ", ".join(
                    f"({row_id}, 'row_{row_id}_in_{table.rsplit('/', 1)[-1]}')"
                    for row_id in range(batch_start, min(batch_start + 10, self.NUM_ROWS))
                )
                self.client.query(
                    f"INSERT INTO `{table}` (id, payload) VALUES {values};",
                    False,
                )
        logger.info("[%s] Inserted %d rows into %d tables", self._log_prefix, self.NUM_ROWS, len(table_names))
        return True

    def _drop_tables(self, names):
        for name in names:
            try:
                self.client.query(f"DROP TABLE `{name}`;", True)
            except Exception:
                pass

    def _drop_topics(self, names):
        for name in names:
            try:
                self.client.query(f"DROP TOPIC `{name}`;", True)
            except Exception:
                pass

    def _drop_views(self, names):
        for name in names:
            try:
                self.client.query(f"DROP VIEW `{name}`;", True)
            except Exception:
                pass

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def _main_loop(self):
        logger.info("[%s] Started, encrypted=%s compression=%s include_index_data=%s",
                    self._log_prefix, bool(self.encryption), self.compression,
                    self.include_index_data)

        while not self._should_stop():
            self._inc_stat("iterations")
            run_id = str(uuid.uuid1()).replace("-", "_")
            prefix = f"full_{run_id}"
            db_path = self.client.database.rstrip("/")

            table_names = [f"{prefix}/tbl{i}" for i in range(self.NUM_TABLES)]
            topic_names = [f"{prefix}/topic{i}" for i in range(self.NUM_TOPICS)]
            view_names = [f"{prefix}/view{i}" for i in range(self.NUM_VIEWS)]

            logger.info("[%s] === run_id=%s tables=%d topics=%d views=%d ===",
                        self._log_prefix, run_id[:16], len(table_names), len(topic_names), len(view_names))

            try:
                if not self._create_schema(prefix, table_names, topic_names, view_names):
                    break
                if not self._insert_rows(table_names):
                    break

                # Export the whole prefix directory via the backend.
                result = self._retry(
                    lambda: self._backend.start_export(
                        prefix, run_id, self.encryption, self.compression,
                        self.include_index_data,
                    ),
                    "Export start",
                )
                if result is None or isinstance(result, Exception):
                    if not self._should_stop():
                        self._inc_stat("export_error")
                        self._signal_fatal_error(
                            f"Export start failed after retries run_id={run_id}: {result}"
                        )
                    break
                op, location = result
                self._inc_stat("export_started")
                logger.info("[%s] Export started: op=%s location=%s", self._log_prefix, op.id, location)

                status = self._wait_op(op.id, self._backend.poll_export)
                if status is None:
                    break
                if status != "DONE":
                    self._inc_stat("export_error")
                    self._signal_fatal_error(f"Export failed status={status} op={op.id}")
                    break
                self._inc_stat("export_done")
                logger.info("[%s] Export DONE", self._log_prefix)

                # Import into a different prefix.
                # The backend maps the whole export to import_dest, so the imported
                # objects live under import_dest/<prefix>/... We clean the whole
                # import_dest at once via cleanup_import_dest.
                import_dest = f"{db_path}/imp_{run_id}"

                imp_result = self._retry(
                    lambda: self._backend.start_import(
                        location, import_dest, run_id, self.encryption,
                        self.include_index_data,
                    ),
                    "Import start",
                )
                if imp_result is None or isinstance(imp_result, Exception):
                    if not self._should_stop():
                        self._inc_stat("import_error")
                        self._signal_fatal_error(
                            f"Import start failed after retries run_id={run_id}: {imp_result}"
                        )
                    break
                self._inc_stat("import_started")
                logger.info("[%s] Import started: op=%s dest=%s",
                            self._log_prefix, imp_result.id, import_dest)

                imp_status = self._wait_op(imp_result.id, self._backend.poll_import)
                if imp_status is None:
                    break
                if imp_status != "DONE":
                    self._inc_stat("import_error")
                    self._signal_fatal_error(f"Import failed status={imp_status} op={imp_result.id}")
                    break
                self._inc_stat("import_done")
                logger.info("[%s] Import DONE", self._log_prefix)

                # Cleanup: remove import destination and backend export artifacts.
                self._backend.cleanup_import_dest(import_dest, self.client)
                self._backend.cleanup_location(location)

            except Exception as e:
                logger.error("[%s] Iteration failed: %s", self._log_prefix, e, exc_info=True)
                self._signal_fatal_error(f"Exception: {e}")
                break
            finally:
                # Always remove source schema created for this iteration.
                self._drop_views(view_names)
                self._drop_topics(topic_names)
                self._drop_tables(table_names)

        logger.info("[%s] Stopped", self._log_prefix)

    def get_workload_thread_funcs(self):
        return [self._main_loop]

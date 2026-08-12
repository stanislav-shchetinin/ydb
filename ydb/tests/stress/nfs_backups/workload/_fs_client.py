# -*- coding: utf-8 -*-
"""FsExportClient — thin gRPC wrapper for ExportToFs / ImportFromFs."""
import logging

from ydb import issues as ydb_issues

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

from .helpers import apply_encryption_settings, apply_compression_settings

logger = logging.getLogger(__name__)

_EXPORT_PROGRESSES: dict = {}
_IMPORT_PROGRESSES: dict = {}


def _init_progresses():
    for key, value in ydb_export_pb2.ExportProgress.Progress.items():
        _EXPORT_PROGRESSES[value] = key[len("PROGRESS_"):]
    for key, value in ydb_import_pb2.ImportProgress.Progress.items():
        _IMPORT_PROGRESSES[value] = key[len("PROGRESS_"):]


_init_progresses()


class ExportToFsOperation:
    def __init__(self, rpc_state, response, driver):
        ydb_issues._process_response(response.operation)
        self.id = response.operation.id
        self.ready = response.operation.ready
        self._driver = driver
        metadata = ydb_export_pb2.ExportToFsMetadata()
        response.operation.metadata.Unpack(metadata)
        self.progress = _EXPORT_PROGRESSES.get(metadata.progress, "UNKNOWN")
        self.items_progress = metadata.items_progress


class ImportFromFsOperation:
    def __init__(self, rpc_state, response, driver):
        ydb_issues._process_response(response.operation)
        self.id = response.operation.id
        self.ready = response.operation.ready
        self._driver = driver
        metadata = ydb_import_pb2.ImportFromFsMetadata()
        response.operation.metadata.Unpack(metadata)
        self.progress = _IMPORT_PROGRESSES.get(metadata.progress, "UNKNOWN")
        self.items_progress = metadata.items_progress


class FsExportClient:
    """Thin wrapper for ExportToFs / ImportFromFs gRPC calls."""

    def __init__(self, driver):
        self._driver = driver

    def export_to_fs(self, base_path, items, description="", number_of_retries=3,
                     encryption=None, compression=None):
        request = ydb_export_pb2.ExportToFsRequest(
            settings=ydb_export_pb2.ExportToFsSettings(
                base_path=base_path,
                number_of_retries=number_of_retries,
            )
        )
        if description:
            request.settings.description = description
        for src, dst in items:
            request.settings.items.add(source_path=src, destination_path=dst)
        apply_encryption_settings(request.settings, encryption)
        apply_compression_settings(request.settings, compression)
        return self._driver(
            request,
            ydb_export_v1_pb2_grpc.ExportServiceStub,
            "ExportToFs",
            ExportToFsOperation,
            None,
            (self._driver,),
        )

    def get_export_operation(self, operation_id):
        from ydb import _apis
        request = _apis.ydb_operation.GetOperationRequest(id=operation_id)
        return self._driver(
            request,
            _apis.OperationService.Stub,
            _apis.OperationService.GetOperation,
            ExportToFsOperation,
            None,
            (self._driver,),
        )

    def import_from_fs(self, base_path, items=None, description="", number_of_retries=3,
                       destination_path="", encryption=None):
        request = ydb_import_pb2.ImportFromFsRequest(
            settings=ydb_import_pb2.ImportFromFsSettings(
                base_path=base_path,
                number_of_retries=number_of_retries,
            )
        )
        if description:
            request.settings.description = description
        if destination_path:
            request.settings.destination_path = destination_path
        if items:
            for src, dst in items:
                request.settings.items.add(source_path=src, destination_path=dst)
        apply_encryption_settings(request.settings, encryption)
        return self._driver(
            request,
            ydb_import_v1_pb2_grpc.ImportServiceStub,
            "ImportFromFs",
            ImportFromFsOperation,
            None,
            (self._driver,),
        )

    def get_import_operation(self, operation_id):
        from ydb import _apis
        request = _apis.ydb_operation.GetOperationRequest(id=operation_id)
        return self._driver(
            request,
            _apis.OperationService.Stub,
            _apis.OperationService.GetOperation,
            ImportFromFsOperation,
            None,
            (self._driver,),
        )

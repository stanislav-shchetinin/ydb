PY3_LIBRARY()

PY_SRCS(
    __init__.py
    _fs_client.py
    full_roundtrip.py
    helpers.py
    roundtrip.py
    storage.py
)

PEERDIR(
    contrib/python/boto3
    ydb/tests/library
    ydb/tests/stress/common
    ydb/public/sdk/python
    ydb/public/sdk/python/enable_v3_new_behavior
    ydb/public/api/protos
    ydb/public/api/grpc
)

END()

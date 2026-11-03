UNITTEST_FOR(ydb/library/yql/providers/common/http_gateway)

FORK_SUBTESTS()

SRCS(
    yql_http_gateway_tls_ut.cpp
    yql_aws_signature_ut.cpp
    yql_dns_gateway_ut.cpp
    yql_http_default_retry_policy_ut.cpp
)

PEERDIR(
    library/cpp/testing/unittest
    ydb/core/security/certificate_check/test_utils
    ydb/library/actors/testlib
)

YQL_LAST_ABI_VERSION()

END()

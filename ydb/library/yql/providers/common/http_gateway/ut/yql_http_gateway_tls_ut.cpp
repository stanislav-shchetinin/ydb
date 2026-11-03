#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>
#include <ydb/library/actors/http/ut/tls_test_server.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>
#include <library/cpp/threading/future/future.h>

using namespace NYql;
using namespace NHttp::NTest;

namespace {

void CheckRequest(TTlsServer& server, const TString& caFile, TStringBuf host, CURLcode expected) {
    THttpGatewayConfig config;
    config.SetCaFile(caFile);
    config.SetRequestTimeoutSeconds(5);
    const auto gateway = IHTTPGateway::Make(&config);
    auto promise = NThreading::NewPromise<IHTTPGateway::TResult>();
    auto future = promise.GetFuture();
    IHTTPGateway::THeaders headers;
    headers.Fields.push_back("Authorization: Bearer test-secret");
    const auto previousRequests = server.Requests->load();
    gateway->Download(server.Url(host), headers, 0, 1024,
        [promise](IHTTPGateway::TResult&& result) mutable {
            promise.SetValue(std::move(result));
        });
    UNIT_ASSERT(future.Wait(TDuration::Seconds(10)));
    auto result = future.ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(result.CurlResponseCode, expected, result.Issues.ToOneLineString());
    if (expected == CURLE_OK) {
        UNIT_ASSERT_VALUES_EQUAL(result.Content.HttpResponseCode, 200);
        UNIT_ASSERT_VALUES_EQUAL(result.Content.Extract(), "ok");
        UNIT_ASSERT_VALUES_EQUAL(server.Requests->load(), previousRequests + 1);
    } else {
        // A TLS failure must not deliver the request containing Authorization.
        UNIT_ASSERT_VALUES_EQUAL(server.Requests->load(), previousRequests);
    }
}

}

Y_UNIT_TEST_SUITE(HttpGatewayTls) {
    Y_UNIT_TEST(TrustedDnsAndIpAddresses) {
        TTlsServer server;
        for (TStringBuf host : {"localhost", "127.0.0.1", "[::1]"}) {
            CheckRequest(server, server.Certificates.CaFile.Name(), host, CURLE_OK);
        }
    }

    Y_UNIT_TEST(RejectsUntrustedServer) {
        TTlsServer server;
        TTlsCertificates unrelated;
        CheckRequest(server, {}, "localhost", CURLE_PEER_FAILED_VERIFICATION);
        CheckRequest(server, unrelated.CaFile.Name(), "localhost", CURLE_PEER_FAILED_VERIFICATION);
    }

    Y_UNIT_TEST(RejectsWrongDnsAndIpAddresses) {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.CommonName = "wrong-host";
        props.AltNames = {"DNS:wrong-host", "IP:127.0.0.2", "IP:::2"};
        TTlsServer server(props);
        for (TStringBuf host : {"localhost", "127.0.0.1", "[::1]"}) {
            CheckRequest(server, server.Certificates.CaFile.Name(), host, CURLE_PEER_FAILED_VERIFICATION);
        }
    }

    Y_UNIT_TEST(RejectsInvalidCaFile) {
        TTlsServer server;
        TTempFileHandle invalidCa;
        invalidCa.Write("invalid CA", 10);
        CheckRequest(server, invalidCa.Name(), "localhost", CURLE_SSL_CACERT_BADFILE);
    }
}

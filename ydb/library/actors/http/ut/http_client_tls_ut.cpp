#include "tls_test_server.h"

using namespace NHttp;
using namespace NHttp::NTest;

namespace {

void CheckRequest(TTlsServer& server, TStringBuf host, bool http2, bool success) {
    auto& runtime = server.Runtime;
    const auto proxy = runtime.Register(CreateHttpProxy());
    const auto previousRequests = server.Requests->load();
    const auto edge = runtime.AllocateEdgeActor();
    auto request = THttpOutgoingRequest::CreateRequestGet(server.Url(host));
    request->Set("Authorization", "Bearer test-secret");
    auto outgoing = MakeHolder<TEvHttpProxy::TEvHttpOutgoingRequest>(request, TDuration::Seconds(5));
    outgoing->UseHttp2 = http2;
    runtime.Send(new NActors::IEventHandle(proxy, edge, outgoing.Release()), 0, true);

    TAutoPtr<NActors::IEventHandle> handle;
    const auto* response = runtime.GrabEdgeEvent<TEvHttpProxy::TEvHttpIncomingResponse>(handle, TDuration::Seconds(10));
    UNIT_ASSERT(response);
    if (success) {
        UNIT_ASSERT_C(response->Error.empty(), response->Error);
        UNIT_ASSERT(response->Response);
        UNIT_ASSERT_VALUES_EQUAL(response->Response->Status, "200");
        UNIT_ASSERT_VALUES_EQUAL(response->Response->Body, "ok");
        UNIT_ASSERT_VALUES_EQUAL(server.Requests->load(), previousRequests + 1);
    } else {
        UNIT_ASSERT_C(response->Error.Contains("TLS certificate verification failed"), response->Error);
        UNIT_ASSERT(!response->Response);
        // No HTTP request (including Authorization) reached the server.
        UNIT_ASSERT_VALUES_EQUAL(server.Requests->load(), previousRequests);
    }
}

}

Y_UNIT_TEST_SUITE(HttpClientTls) {
    Y_UNIT_TEST(TrustedDnsAndIpAddresses) {
        TTlsServer server;
        for (bool http2 : {false, true}) {
            for (TStringBuf host : {"localhost", "127.0.0.1", "[::1]"}) {
                CheckRequest(server, host, http2, true);
            }
        }
    }

    Y_UNIT_TEST(RejectsUntrustedCertificate) {
        TTlsServer server(NKikimr::NCertTestUtils::TProps::AsServer(), false);
        for (bool http2 : {false, true}) {
            CheckRequest(server, "localhost", http2, false);
        }
    }

    Y_UNIT_TEST(RejectsWrongDnsAndIpAddresses) {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.CommonName = "wrong-host";
        props.AltNames = {"DNS:wrong-host", "IP:127.0.0.2", "IP:::2"};
        TTlsServer server(props);
        for (bool http2 : {false, true}) {
            for (TStringBuf host : {"localhost", "127.0.0.1", "[::1]"}) {
                CheckRequest(server, host, http2, false);
            }
        }
    }

    Y_UNIT_TEST(RejectsExpiredCertificate) {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.SecondsValid = -3600;
        TTlsServer server(props);
        for (bool http2 : {false, true}) {
            CheckRequest(server, "localhost", http2, false);
        }
    }
}

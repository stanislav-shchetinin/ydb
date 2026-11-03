#include "tls_test_server.h"

using namespace NHttp;
using namespace NHttp::NTest;

namespace {

void CheckRequest(TTlsServer& server, const TActorId& proxy, TStringBuf host, bool http2, bool success,
        TStringBuf expectedError = "TLS certificate verification failed") {
    auto& runtime = server.Runtime;
    const auto previousRequests = server.Requests->load();
    const auto edge = runtime.AllocateEdgeActor();
    auto request = THttpOutgoingRequest::CreateRequestGet(server.Url(host));
    request->Set("Authorization", "Bearer test-secret");
    auto outgoing = MakeHolder<TEvHttpProxy::TEvHttpOutgoingRequest>(request, TDuration::Seconds(5));
    outgoing->UseHttp2 = http2;
    outgoing->AllowConnectionReuse = true;
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
        UNIT_ASSERT_C(response->Error.Contains(expectedError), response->Error);
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
                // A separate proxy ensures HTTP/2 is tested with a new TLS handshake.
                const auto client = server.Runtime.Register(CreateHttpProxy({}, {server.Certificates.CaFile.Name()}));
                CheckRequest(server, client, host, http2, true);
                CheckRequest(server, client, host, http2, true);
            }
        }
    }

    Y_UNIT_TEST(DefaultTrustRejectsPrivateCa) {
        TTlsServer server;
        for (bool http2 : {false, true}) {
            const auto proxy = server.Runtime.Register(CreateHttpProxy());
            CheckRequest(server, proxy, "localhost", http2, false);
        }
    }

    Y_UNIT_TEST(TrustStoresAreIsolatedBetweenProxies) {
        TTlsServer server;
        TTlsCertificates unrelated;
        for (bool http2 : {false, true}) {
            const auto trusted = server.Runtime.Register(CreateHttpProxy({}, {server.Certificates.CaFile.Name()}));
            CheckRequest(server, trusted, "localhost", http2, true);
            const auto untrusted = server.Runtime.Register(CreateHttpProxy({}, {unrelated.CaFile.Name()}));
            CheckRequest(server, untrusted, "localhost", http2, false);
        }
    }

    Y_UNIT_TEST(RejectsWrongDnsAndIpAddresses) {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.CommonName = "wrong-host";
        props.AltNames = {"DNS:wrong-host", "IP:127.0.0.2", "IP:::2"};
        TTlsServer server(props);
        for (bool http2 : {false, true}) {
            for (TStringBuf host : {"localhost", "127.0.0.1", "[::1]"}) {
                const auto proxy = server.Runtime.Register(CreateHttpProxy({}, {server.Certificates.CaFile.Name()}));
                CheckRequest(server, proxy, host, http2, false);
            }
        }
    }

    Y_UNIT_TEST(RejectsExpiredCertificate) {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.SecondsValid = -3600;
        TTlsServer server(props);
        for (bool http2 : {false, true}) {
            const auto proxy = server.Runtime.Register(CreateHttpProxy({}, {server.Certificates.CaFile.Name()}));
            CheckRequest(server, proxy, "localhost", http2, false);
        }
    }

    Y_UNIT_TEST(RejectsInvalidCaFile) {
        TTlsServer server;
        TTempFileHandle invalidCa;
        invalidCa.Write("invalid CA", 10);
        for (bool http2 : {false, true}) {
            for (const TString& caFile : {invalidCa.Name(), invalidCa.Name() + ".missing"}) {
                const auto proxy = server.Runtime.Register(CreateHttpProxy({}, {caFile}));
                CheckRequest(server, proxy, "localhost", http2, false, "Protocol error");
            }
        }
    }
}

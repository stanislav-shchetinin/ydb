#pragma once

#include <ydb/core/security/certificate_check/test_utils/test_cert_auth_utils.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/unittest/tests_data.h>
#include <util/system/env.h>
#include <util/system/tempfile.h>

#include <atomic>

namespace NHttp::NTest {

struct TTlsCertificates {
    NKikimr::NCertTestUtils::TCertAndKey Ca;
    NKikimr::NCertTestUtils::TCertAndKey Server;
    TTempFileHandle CaFile;
    const TMaybe<TString> PreviousCaFile = TryGetEnv("SSL_CERT_FILE");
    const TMaybe<TString> PreviousCaDir = TryGetEnv("SSL_CERT_DIR");

    explicit TTlsCertificates(const NKikimr::NCertTestUtils::TProps& serverProps = NKikimr::NCertTestUtils::TProps::AsServer(),
            bool trustServer = true)
        : Ca(NKikimr::NCertTestUtils::GenerateCA(NKikimr::NCertTestUtils::TProps::AsCA()))
        , Server(NKikimr::NCertTestUtils::GenerateSignedCert(Ca, serverProps))
    {
        CaFile.Write(Ca.Certificate.data(), Ca.Certificate.size());
        SetEnv("SSL_CERT_FILE", trustServer ? CaFile.Name() : TString());
        SetEnv("SSL_CERT_DIR", "");
    }

    ~TTlsCertificates() {
        RestoreEnvironment("SSL_CERT_FILE", PreviousCaFile);
        RestoreEnvironment("SSL_CERT_DIR", PreviousCaDir);
    }

    static void RestoreEnvironment(const TString& name, const TMaybe<TString>& value) {
        if (value) {
            SetEnv(name, *value);
        } else {
            UnsetEnv(name);
        }
    }

    TString ServerPem() const {
        return TString(Server.Certificate) + TString(Server.PrivateKey);
    }
};

struct TTlsServer {
    struct THandler : NActors::TActorBootstrapped<THandler> {
        std::shared_ptr<std::atomic<size_t>> Requests;

        explicit THandler(std::shared_ptr<std::atomic<size_t>> requests)
            : Requests(std::move(requests))
        {}

        void Bootstrap() {
            Become(&THandler::StateWork);
        }

        void Handle(TEvHttpProxy::TEvHttpIncomingRequest::TPtr& ev) {
            ++*Requests;
            Send(ev->Sender, new TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseOK("ok")));
        }

        STRICT_STFUNC(StateWork,
            hFunc(TEvHttpProxy::TEvHttpIncomingRequest, Handle);
        )
    };

    // Set up trust before starting runtime threads, and restore it after they stop.
    TTlsCertificates Certificates;
    TPortManager PortManager;
    TIpPort Port = PortManager.GetTcpPort();
    std::shared_ptr<std::atomic<size_t>> Requests = std::make_shared<std::atomic<size_t>>(0);
    NActors::TTestActorRuntimeBase Runtime{1, true};

    explicit TTlsServer(const NKikimr::NCertTestUtils::TProps& props = NKikimr::NCertTestUtils::TProps::AsServer(),
            bool trustServer = true)
        : Certificates(props, trustServer)
    {
        Runtime.Initialize();
        const auto proxy = Runtime.Register(CreateHttpProxy());
        const auto edge = Runtime.AllocateEdgeActor();
        auto add = MakeHolder<TEvHttpProxy::TEvAddListeningPort>(Port);
        add->Secure = true;
        add->AllowHttp2 = true;
        add->SslCertificatePem = Certificates.ServerPem();
        Runtime.Send(new NActors::IEventHandle(proxy, edge, add.Release()), 0, true);
        TAutoPtr<NActors::IEventHandle> handle;
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvHttpProxy::TEvConfirmListen>(handle, TDuration::Seconds(5)));
        const auto handler = Runtime.Register(new THandler(Requests));
        Runtime.Send(new NActors::IEventHandle(proxy, edge, new TEvHttpProxy::TEvRegisterHandler("/", handler)), 0, true);
    }

    TString Url(TStringBuf host = "localhost") const {
        return TStringBuilder() << "https://" << host << ':' << Port << "/test";
    }
};

}

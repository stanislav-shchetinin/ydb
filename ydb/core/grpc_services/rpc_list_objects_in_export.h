#pragma once

#include "rpc_deferrable.h"

#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/schemeshard/schemeshard_import.h>

namespace NKikimr::NGRpcService {

template <typename TTraits>
using TListObjectsInExportRequest = TGrpcRequestOperationCall<typename TTraits::TApiRequest, typename TTraits::TApiResponse>;

template <typename TTraits>
class TListObjectsInExportRPC
    : public TRpcOperationRequestActor<TListObjectsInExportRPC<TTraits>, TListObjectsInExportRequest<TTraits>>
{
    using TThis = TListObjectsInExportRPC<TTraits>;
    using TRequest = TListObjectsInExportRequest<TTraits>;
    using TBase = TRpcOperationRequestActor<TThis, TRequest>;
    using TSchemeShardRequestEvent = typename TTraits::TSchemeShardRequestEvent;
    using TSchemeShardResponseEvent = typename TTraits::TSchemeShardResponseEvent;

public:
    explicit TListObjectsInExportRPC(IRequestOpCtx* request)
        : TBase(request)
        , UserToken(CreateUserToken(request))
    {
    }

    STATEFN(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TSchemeShardResponseEvent, Handle);
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);

            hFunc(TEvTabletPipe::TEvClientConnected, Handle);
            hFunc(TEvTabletPipe::TEvClientDestroyed, Handle);
        default:
            return this->StateFuncBase(ev);
        }
    }

    void Bootstrap() {
        if (!this->Request_ || !this->Request_->GetDatabaseName()) {
            return this->Reply(Ydb::StatusIds::BAD_REQUEST, "Database name is not specified",
                NKikimrIssues::TIssuesIds::YDB_API_VALIDATION_ERROR, NActors::TActivationContext::AsActorContext());
        }

        TString error;
        if (!TTraits::ValidateRequest(*this->GetProtoRequest(), error)) {
            return this->Reply(Ydb::StatusIds::BAD_REQUEST, error,
                NKikimrIssues::TIssuesIds::YDB_API_VALIDATION_ERROR, NActors::TActivationContext::AsActorContext());
        }

        ResolveDatabase();
        this->Become(&TThis::StateFunc);
    }

    void ResolveDatabase() {
        YDB_LOG_DEBUG(TTraits::LogPrefix(),
            {"action", "Resolve database"},
            {"selfId", this->SelfId()},
            {"name", this->Request_->GetDatabaseName()});

        auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
        request->DatabaseName = *this->Request_->GetDatabaseName();

        auto& entry = request->ResultSet.emplace_back();
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        entry.Path = NKikimr::SplitPath(*this->Request_->GetDatabaseName());

        this->Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        const auto& request = ev->Get()->Request;

        YDB_LOG_DEBUG(TTraits::LogPrefix(),
            {"action", "Handle TEvTxProxySchemeCache::TEvNavigateKeySetResult"},
            {"selfId", this->SelfId()},
            {"request", (request ? request->ToString(*AppData()->TypeRegistry) : "nullptr")});

        if (request->ResultSet.empty()) {
            return this->Reply(Ydb::StatusIds::SCHEME_ERROR, "Scheme error",
                NKikimrIssues::TIssuesIds::GENERIC_RESOLVE_ERROR, NActors::TActivationContext::AsActorContext());
        }

        const auto& entry = request->ResultSet.front();
        if (request->ErrorCount > 0) {
            switch (entry.Status) {
            case NSchemeCache::TSchemeCacheNavigate::EStatus::Ok:
                break;
            case NSchemeCache::TSchemeCacheNavigate::EStatus::AccessDenied:
                return this->Reply(Ydb::StatusIds::UNAUTHORIZED, "Access denied",
                    NKikimrIssues::TIssuesIds::ACCESS_DENIED, NActors::TActivationContext::AsActorContext());
            case NSchemeCache::TSchemeCacheNavigate::EStatus::RootUnknown:
            case NSchemeCache::TSchemeCacheNavigate::EStatus::PathErrorUnknown:
                return this->Reply(Ydb::StatusIds::SCHEME_ERROR, "Unknown database",
                    NKikimrIssues::TIssuesIds::PATH_NOT_EXIST, NActors::TActivationContext::AsActorContext());
            case NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError:
            case NSchemeCache::TSchemeCacheNavigate::EStatus::RedirectLookupError:
                return this->Reply(Ydb::StatusIds::UNAVAILABLE, "Database lookup error",
                    NKikimrIssues::TIssuesIds::RESOLVE_LOOKUP_ERROR, NActors::TActivationContext::AsActorContext());
            default:
                return this->Reply(Ydb::StatusIds::SCHEME_ERROR, "Scheme error",
                    NKikimrIssues::TIssuesIds::GENERIC_RESOLVE_ERROR, NActors::TActivationContext::AsActorContext());
            }
        }

        if (!CheckDatabaseAccess(CanonizePath(entry.Path), entry.SecurityObject)) {
            return;
        }

        auto domainInfo = entry.DomainInfo;
        if (!domainInfo) {
            YDB_LOG_ERROR(TTraits::LogPrefix(),
                {"action", "Got empty domain info"},
                {"selfId", this->SelfId()});
            return this->Reply(Ydb::StatusIds::INTERNAL_ERROR, "Internal error",
                NKikimrIssues::TIssuesIds::GENERIC_RESOLVE_ERROR, NActors::TActivationContext::AsActorContext());
        }

        SchemeShardId = domainInfo->ExtractSchemeShard();
        SendRequestToSchemeShard();
    }

    bool CheckDatabaseAccess(const TString& path, TIntrusivePtr<TSecurityObject> securityObject) {
        const ui32 access = NACLib::DescribeSchema;

        if (!UserToken || !securityObject) {
            return true;
        }

        if (securityObject->CheckAccess(access, *UserToken)) {
            return true;
        }

        this->Reply(Ydb::StatusIds::UNAUTHORIZED,
            TStringBuilder() << "Access denied"
                << ": for# " << UserToken->GetUserSID()
                << ", path# " << path
                << ", access# " << NACLib::AccessRightsToString(access),
            NKikimrIssues::TIssuesIds::ACCESS_DENIED,
            NActors::TActivationContext::AsActorContext());
        return false;
    }

    void SendRequestToSchemeShard() {
        YDB_LOG_DEBUG(TTraits::LogPrefix(),
            {"action", "Send request"},
            {"selfId", this->SelfId()},
            {"schemeShardId", SchemeShardId});

        if (!PipeClient) {
            NTabletPipe::TClientConfig config;
            config.RetryPolicy = {.RetryLimitCount = 3};
            PipeClient = this->RegisterWithSameMailbox(
                NTabletPipe::CreateClient(this->SelfId(), SchemeShardId, config));
        }

        auto request = MakeHolder<TSchemeShardRequestEvent>();
        const auto& protoRequest = *this->GetProtoRequest();
        *request->Record.MutableOperationParams() = protoRequest.operation_params();
        TTraits::CopySettings(protoRequest, request->Record);
        request->Record.SetPageSize(protoRequest.page_size());
        request->Record.SetPageToken(protoRequest.page_token());

        NTabletPipe::SendData(this->SelfId(), PipeClient, std::move(request), 0, this->Span_.GetTraceId());
    }

    void Handle(typename TSchemeShardResponseEvent::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        YDB_LOG_DEBUG(TTraits::LogPrefix(),
            {"action", "Handle SchemeShard response"},
            {"selfId", this->SelfId()},
            {"record", record.ShortDebugString()});

        if (record.GetStatus() != Ydb::StatusIds::SUCCESS) {
            return this->Reply(record.GetStatus(), record.GetIssues(), NActors::TActivationContext::AsActorContext());
        }
        return this->ReplyWithResult(record.GetStatus(), record.GetIssues(), record.GetResult(),
            NActors::TActivationContext::AsActorContext());
    }

    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev) {
        if (ev->Get()->Status != NKikimrProto::OK) {
            DeliveryProblem();
        }
    }

    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr&) {
        DeliveryProblem();
    }

    void DeliveryProblem() {
        YDB_LOG_WARN(TTraits::LogPrefix(),
            {"action", "Delivery problem"},
            {"selfId", this->SelfId()});
        this->Reply(Ydb::StatusIds::UNAVAILABLE, "Delivery problem",
            NKikimrIssues::TIssuesIds::DEFAULT_ERROR, NActors::TActivationContext::AsActorContext());
    }

    void PassAway() override {
        NTabletPipe::CloseClient(this->SelfId(), PipeClient);
        TBase::PassAway();
    }

private:
    static THolder<const NACLib::TUserToken> CreateUserToken(IRequestOpCtx* request) {
        if (const auto& userToken = request->GetSerializedToken()) {
            return MakeHolder<NACLib::TUserToken>(userToken);
        }
        return {};
    }

private:
    ui64 SchemeShardId = 0;
    TActorId PipeClient;
    const THolder<const NACLib::TUserToken> UserToken;
};

template <typename TTraits>
void DoListObjectsInExportRequest(std::unique_ptr<IRequestOpCtx> request, const IFacilityProvider& facility) {
    facility.RegisterActor(new TListObjectsInExportRPC<TTraits>(request.release()));
}

} // namespace NKikimr::NGRpcService

#pragma once

#include <ydb/core/kqp/compute_actor/kqp_compute_events.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/federated_query/kqp_federated_query_helpers.h>
#include <ydb/core/scheme/scheme_tabledefs.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io_factory.h>

namespace NKikimr {

namespace NMiniKQL {

class TKqpScanComputeContext;

TComputationNodeFactory GetKqpActorComputeFactory(TKqpScanComputeContext* computeCtx, const std::optional<NKqp::TKqpFederatedQuerySetup>& federatedQuerySetup);

} // namespace NMiniKQL

namespace NKqp {

class TShardsScanningPolicy {
private:
    const NKikimrConfig::TTableServiceConfig::TShardsScanningPolicy ProtoConfig;
public:
    TShardsScanningPolicy(const NKikimrConfig::TTableServiceConfig::TShardsScanningPolicy& pbConfig)
        : ProtoConfig(pbConfig)
    {

    }

    bool IsParallelScanningAvailable() const {
        return ProtoConfig.GetParallelScanningAvailable();
    }

    bool GetShardSplitFactor() const {
        return ProtoConfig.GetShardSplitFactor();
    }

    void FillRequestScanFeatures(const NKikimrTxDataShard::TKqpTransaction::TScanTaskMeta& meta,
        ui32& maxInFlight, bool& isAggregationRequest) const;

};

IActor* CreateKqpComputeActor(const TActorId& executerId, ui64 txId, NYql::NDqProto::TDqTask* task,
    NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory,
    const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
    const NYql::NDq::TComputeRuntimeSettings& settings, const NYql::NDq::TComputeMemoryLimits& memoryLimits,
    NWilson::TTraceId traceId,
    TIntrusivePtr<NActors::TProtoArenaHolder> arena,
    const std::optional<TKqpFederatedQuerySetup>& federatedQuerySetup);

IActor* CreateKqpScanComputeActor(const TActorId& executerId, ui64 txId,
    NYql::NDqProto::TDqTask* task, NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory,
    const NKikimr::NMiniKQL::IFunctionRegistry* functionRegistry,
    const NYql::NDq::TComputeRuntimeSettings& settings, const NYql::NDq::TComputeMemoryLimits& memoryLimits, NWilson::TTraceId traceId,
    TIntrusivePtr<NActors::TProtoArenaHolder> arena);

IActor* CreateKqpScanFetcher(const NKikimrKqp::TKqpSnapshot& snapshot, std::vector<NActors::TActorId>&& computeActors,
    const NKikimrTxDataShard::TKqpTransaction::TScanTaskMeta& meta, const NYql::NDq::TComputeRuntimeSettings& settings,
    const ui64 txId, const TShardsScanningPolicy& shardsScanningPolicy, TIntrusivePtr<TKqpCounters> counters, NWilson::TTraceId traceId);

NYql::NDq::IDqAsyncIoFactory::TPtr CreateKqpAsyncIoFactory(
    TIntrusivePtr<TKqpCounters> counters, std::optional<TKqpFederatedQuerySetup> federatedQuerySetup);

} // namespace NKqp
} // namespace NKikimr
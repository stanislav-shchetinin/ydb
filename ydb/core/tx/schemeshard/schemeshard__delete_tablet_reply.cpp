#include "schemeshard_impl.h"

#include <ydb/core/tablet/tablet_exception.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>
#include <ydb/core/tx/schemeshard/schemeshard__tenant_shred_manager.h>
#include <ydb/library/actors/core/log.h>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::FLAT_TX_SCHEMESHARD

namespace NKikimr {
namespace NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TTxDeleteTabletReply : public TSchemeShard::TRwTxBase {
    NKikimrProto::EReplyStatus Status = NKikimrProto::EReplyStatus::UNKNOWN;
    const ui64 Cookie;
    TVector<std::pair<TShardIdx, TTabletId>> Deleted;
    TTabletId HiveId = InvalidTabletId;
    TTabletId ForwardToHiveId = InvalidTabletId;

    TTxDeleteTabletReply(TSelf* self, TEvHive::TEvDeleteTabletReply::TPtr& ev)
        : TRwTxBase(self)
        , Cookie(ev->Cookie)
    {
        const auto& record = ev->Get()->Record;
        Status = record.GetStatus();
        HiveId = TTabletId(record.GetOrigin());

        for (ui64 idx : record.GetShardLocalIdx()) {
            Deleted.emplace_back(TShardIdx(record.GetShardOwnerId(), idx), InvalidTabletId);
        }

        if (record.HasForwardRequest()) {
            ForwardToHiveId = TTabletId(record.GetForwardRequest().GetHiveTabletId());
        }
    }

    TTxType GetTxType() const override { return TXTYPE_FREE_TABLET_RESULT; }

    void DoExecute(TTransactionContext &txc, const TActorContext &ctx) override {
        if (Status != NKikimrProto::OK) {
            auto shardIdx = Deleted.front().first;
            if (Status == NKikimrProto::INVALID_OWNER) {
                YDB_LOG_WARN_CTX(ctx, "Got DeleteTabletReply with Forward response from Hive",
                    {"hiveId", HiveId},
                    {"forwardToHiveId", ForwardToHiveId},
                    {"shardIdx", shardIdx},
                );
                Y_ABORT_UNLESS(ForwardToHiveId);
                Self->ShardDeleter.RedirectDeleteRequest(HiveId, ForwardToHiveId, shardIdx, Self->ShardInfos, ctx);
                return;
            }
            // WTF could happen that hive failed to delete the freaking tablet?
            YDB_LOG_ALERT_CTX(ctx, "Got DeleteTabletReply from Hive",
                {"hiveId", HiveId},
                {"shardIdx", shardIdx},
                {"status", Status},
            );
            return;
        }

        for (auto& [shardIdx, tabletId] : Deleted) {
            ForgetShard(shardIdx, tabletId, txc, ctx);
        }
    }

    void ForgetShard(TShardIdx shardIdx, TTabletId& tabletId, TTransactionContext& txc, const TActorContext& ctx) {
        // "Forget" the deleted shard
        if (Self->ShardInfos.contains(shardIdx)) {
            auto tabletType = Self->ShardInfos[shardIdx].TabletType;
            switch (tabletType) {
            case ETabletType::DataShard:
                Self->TabletCounters->Simple()[COUNTER_TABLE_SHARD_INACTIVE_COUNT].Sub(1);
                break;
            case ETabletType::Coordinator:
                Self->TabletCounters->Simple()[COUNTER_SUB_DOMAIN_COORDINATOR_COUNT].Sub(1);
                break;
            case ETabletType::Mediator:
                Self->TabletCounters->Simple()[COUNTER_SUB_DOMAIN_MEDIATOR_COUNT].Sub(1);
                break;
            case ETabletType::SchemeShard:
                Self->TabletCounters->Simple()[COUNTER_SUB_DOMAIN_SCHEME_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::Hive:
                Self->TabletCounters->Simple()[COUNTER_SUB_DOMAIN_HIVE_COUNT].Sub(1);
                break;
            case ETabletType::BlockStoreVolume:
                Self->TabletCounters->Simple()[COUNTER_BLOCKSTORE_VOLUME_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::BlockStorePartition:
                Self->TabletCounters->Simple()[COUNTER_BLOCKSTORE_PARTITION_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::BlockStorePartition2:
                Self->TabletCounters->Simple()[COUNTER_BLOCKSTORE_PARTITION2_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::BlockStoreVolumeDirect:
                Self->TabletCounters->Simple()[COUNTER_BLOCKSTORE_VOLUME_DIRECT_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::BlockStorePartitionDirect:
                Self->TabletCounters->Simple()[COUNTER_BLOCKSTORE_PARTITION_DIRECT_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::FileStore:
                Self->TabletCounters->Simple()[COUNTER_FILESTORE_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::Kesus:
                Self->TabletCounters->Simple()[COUNTER_KESUS_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::KeyValue:
                Self->TabletCounters->Simple()[COUNTER_SOLOMON_PARTITIONS_COUNT].Sub(1);
                break;
            case ETabletType::PersQueue:
                Self->TabletCounters->Simple()[COUNTER_PQ_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::PersQueueReadBalancer:
                Self->TabletCounters->Simple()[COUNTER_PQ_RB_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::SysViewProcessor:
                Self->TabletCounters->Simple()[COUNTER_SYS_VIEW_PROCESSOR_COUNT].Sub(1);
                break;
            case ETabletType::ColumnShard:
                Self->TabletCounters->Simple()[COUNTER_COLUMN_SHARDS].Sub(1);
                break;
            case ETabletType::SequenceShard:
                Self->TabletCounters->Simple()[COUNTER_SEQUENCESHARD_COUNT].Sub(1);
                break;
            case ETabletType::ReplicationController:
                Self->TabletCounters->Simple()[COUNTER_REPLICATION_CONTROLLER_COUNT].Sub(1);
                break;
            case ETabletType::BlobDepot:
                Self->TabletCounters->Simple()[COUNTER_BLOB_DEPOT_COUNT].Sub(1);
                break;
            case ETabletType::StatisticsAggregator:
                Self->TabletCounters->Simple()[COUNTER_STATISTICS_AGGREGATOR_COUNT].Sub(1);
                break;
            case ETabletType::GraphShard:
                Self->TabletCounters->Simple()[COUNTER_GRAPHSHARD_COUNT].Sub(1);
                break;
            case ETabletType::BackupController:
                Self->TabletCounters->Simple()[COUNTER_BACKUP_CONTROLLER_TABLET_COUNT].Sub(1);
                break;
            case ETabletType::WasmCompileController:
                Self->TabletCounters->Simple()[COUNTER_WASM_COMPILE_CONTROLLER_COUNT].Sub(1);
                break;
            case ETabletType::TestShard:
                Self->TabletCounters->Simple()[COUNTER_TEST_SHARD_COUNT].Sub(1);
                break;
            case ETabletType::CdcAggregator:
                Self->TabletCounters->Simple()[COUNTER_CDC_AGGREGATOR_COUNT].Sub(1);
                break;
            default:
                Y_FAIL_S("Unknown TabletType"
                         << ", shardIdx " << shardIdx
                         << ", (ui32)TabletType" << (ui32)tabletType);
            };

            auto& shardInfo = Self->ShardInfos.at(shardIdx);

            auto pathId = shardInfo.PathId;
            auto it = Self->Tables.find(pathId);
            if (it != Self->Tables.end()) {
                it->second->PerShardPartitionConfig.erase(shardIdx);
            }

            NIceDb::TNiceDb db(txc.DB);
            Self->PersistShardDeleted(db, shardIdx, shardInfo.BindedChannels);

            Y_VERIFY_S(Self->PathsById.contains(pathId), "pathid: " << pathId);
            auto path = Self->PathsById.at(pathId);
            path->DecShardsInside();

            auto domain = Self->ResolveDomainInfo(path);
            domain->RemoveInternalShard(shardIdx, Self);
            switch (tabletType) {
            case ETabletType::SequenceShard:
                domain->RemoveSequenceShard(shardIdx);
                break;
            default:
                break;
            }

            tabletId = shardInfo.TabletID;
            Self->TabletIdToShardIdx[tabletId] = shardIdx;

            Self->ShardInfos.erase(shardIdx);

            Self->DecrementPathDbRefCount(pathId, "shard deleted");

            // This is for tests, so it's kinda ok to reply from execute
            auto itSubscribers = Self->ShardDeletionSubscribers.find(shardIdx);
            if (itSubscribers != Self->ShardDeletionSubscribers.end()) {
                for (const auto& subscriber : itSubscribers->second) {
                    ctx.Send(subscriber, new TEvPrivate::TEvNotifyShardDeleted(shardIdx));
                }
                Self->ShardDeletionSubscribers.erase(itSubscribers);
            }
        } else {
            NIceDb::TNiceDb db(txc.DB);
            Self->PersistUnknownShardDeleted(db, shardIdx);
        }

        Self->OnShardRemoved(shardIdx);
    }

    void DoComplete(const TActorContext &ctx) override {
        if (Status != NKikimrProto::OK) {
            return;
        }
        std::vector<TShardIdx> shredded;
        const bool shred = Self->EnableShred && Self->TenantShredManager->GetStatus() == EShredStatus::IN_PROGRESS;
        for (const auto& [shardIdx, tabletId] : Deleted) {
            if (!Cookie) {
                Self->ShardDeleter.ShardDeleted(shardIdx, ctx);
            }
            if (tabletId != InvalidTabletId) {
                Self->PipeClientCache->ForceClose(ctx, ui64(tabletId));
            }
            if (shred) {
                shredded.push_back(shardIdx);
            }
        }
        if (!shredded.empty()) {
            Self->Execute(Self->CreateTxCancelShredShards(shredded));
        }
        if (Cookie) {
            Self->ShardDeleter.BackupCommitted(Cookie, *Self, ctx);
        }
    }
};

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxDeleteTabletReply(TEvHive::TEvDeleteTabletReply::TPtr& ev) {
    return new TTxDeleteTabletReply(this, ev);
}

void TSchemeShard::Handle(TEvPrivate::TEvSubscribeToShardDeletion::TPtr& ev, const TActorContext& ctx) {
    auto shardIdx = ev->Get()->ShardIdx;
    if (ShardInfos.contains(shardIdx)) {
        ShardDeletionSubscribers[shardIdx].push_back(ev->Sender);
        return;
    }

    // This is for tests, so it's kinda ok to reply from handler
    ctx.Send(ev->Sender, new TEvPrivate::TEvNotifyShardDeleted(shardIdx));
}

}}

#undef YDB_LOG_THIS_FILE_COMPONENT

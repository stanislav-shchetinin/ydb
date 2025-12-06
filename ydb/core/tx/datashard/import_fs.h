#pragma once

#include "defs.h"

namespace NKikimrSchemeOp {
    class TRestoreTask;
}

namespace NKikimr {
namespace NDataShard {

class TTableInfo;

IActor* CreateFsDownloader(const TActorId& dataShard, ui64 txId, const NKikimrSchemeOp::TRestoreTask& task, const TTableInfo& info);

} // NDataShard
} // NKikimr


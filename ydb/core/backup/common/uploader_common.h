#pragma once

#include <ydb/core/protos/flat_scheme_op.pb.h>

namespace NKikimr::NBackup {

template <typename TSettings>
TSettings GetSettings(const NKikimrSchemeOp::TBackupTask& task);

} // NKikimr::NBackup

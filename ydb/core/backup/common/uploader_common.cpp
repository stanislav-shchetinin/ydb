#include "uploader_common.h"

#include <ydb/core/protos/s3_settings.pb.h>
#include <ydb/core/protos/fs_settings.pb.h>

namespace NKikimr::NBackup {

template <>
NKikimrSchemeOp::TS3Settings GetSettings(
    const NKikimrSchemeOp::TBackupTask& task)
{
    return task.GetS3Settings();
}

template <>
NKikimrSchemeOp::TFSSettings GetSettings(
    const NKikimrSchemeOp::TBackupTask& task)
{
    return task.GetFSSettings();
}

} // NKikimr::NBackup

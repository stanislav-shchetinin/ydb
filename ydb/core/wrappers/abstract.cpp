#include "abstract.h"
#include "fake_storage_config.h"
#include "fs_storage_config.h"
#include "s3_storage_config.h"

namespace NKikimr::NWrappers::NExternalStorage {

IExternalStorageOperator::TPtr IExternalStorageConfig::ConstructStorageOperator(bool verbose) const {
    return DoConstructStorageOperator(verbose);
}

template <>
IExternalStorageConfig::TPtr IExternalStorageConfig::Construct(const NKikimrSchemeOp::TS3Settings& settings) {
    if (settings.GetEndpoint() == "fake.fake") {
        Cerr << "Constructing TFakeExternalStorageConfig" << Endl;
        return std::make_shared<TFakeExternalStorageConfig>(settings.GetBucket(), settings.GetSecretKey());
    } else {
        Cerr << "Constructing TS3ExternalStorageConfig2" << Endl;
        return std::make_shared<TS3ExternalStorageConfig>(settings);
    }
}

template <>
IExternalStorageConfig::TPtr IExternalStorageConfig::Construct(const Ydb::Export::ExportToS3Settings& settings) {
    Cerr << "Constructing TS3ExternalStorageConfig1" << Endl;
    return std::make_shared<TS3ExternalStorageConfig>(settings);
}

template <>
IExternalStorageConfig::TPtr IExternalStorageConfig::Construct(const Ydb::Import::ImportFromS3Settings& settings) {
    return std::make_shared<TS3ExternalStorageConfig>(settings);
}

template <>
IExternalStorageConfig::TPtr IExternalStorageConfig::Construct(const NKikimrSchemeOp::TFSSettings& settings) {
    Cerr << "Constructing TFsExternalStorageConfig1" << Endl;
    return std::make_shared<TFsExternalStorageConfig>(settings);
}

template <>
IExternalStorageConfig::TPtr IExternalStorageConfig::Construct(const Ydb::Export::ExportToFsSettings& settings) {
    Cerr << "Constructing TFsExternalStorageConfig2" << Endl;
    return std::make_shared<TFsExternalStorageConfig>(settings);
}

template <>
IExternalStorageConfig::TPtr IExternalStorageConfig::Construct(const Ydb::Import::ImportFromFsSettings& settings) {
    return std::make_shared<TFsExternalStorageConfig>(settings);
}
}

# Анализ решения экспорта Kesus

Проверен [KESUS_EXPORT.md](KESUS_EXPORT.md) и снимок незакоммиченной реализации из ветки `export-kesus` поверх `ee2a87b43f3`, сделанный 29 октября 2026 года. Анализ выполнен в отдельном worktree; исходный workspace не изменялся. Ниже отделены обнаруженная проблема, подтверждённые свойства и ограничения. Тесты просмотрены по исходникам, но не запускались.

# Отложенное замечание к экспорту

Повторное использование IV при reboot принято для текущей реализации. Исправление отложено на отдельную задачу экспорта и не является условием реализации импорта.

При повторном запуске зашифрованного экспорта после reboot новый uploader может зашифровать изменившиеся ресурсы тем же ключом и IV. Это нарушает требование уникальности nonce для AES-GCM; различающихся file type для двух файлов недостаточно.

Цепочка видна в коде:

1. `UploadScheme` получает item IV из сохранённой `ExportMetadata`, а `Resume` создаёт uploader заново: [schemeshard_export__create.cpp](ydb/core/tx/schemeshard/schemeshard_export__create.cpp).
2. Новый `TKesusSchemeUploader` повторно читает живые описания SchemeShard и Kesus: [Bootstrap, HandleResources](ydb/core/tx/schemeshard/schemeshard_export_uploaders.cpp).
3. `MakeSchemeFileIV` снова вычисляет `Combine(itemIV, fileType, 0, 0)`. `AddFileWithChecksum` передаёт этот IV шифратору: [uploader](ydb/core/tx/schemeshard/schemeshard_export_uploaders.cpp).
4. `Combine`/`CombineForChunk` детерминированы, а `EncryptFullFile` не добавляет новую случайность: [encryption.cpp](ydb/core/backup/common/encryption.cpp).

Сценарий изменения набора между попытками явно предусмотрен тестом `KesusRebootReplacesResourcesWithEmptySet` в [ut_export.cpp](ydb/core/tx/schemeshard/ut_export/ut_export.cpp). В рамках будущего исправления потребуется проверка неизменности повторно шифруемого plaintext или смены IV при изменении данных.

При возвращении к этой задаче можно сохранять неизменные тексты экспортируемых файлов для всех повторов под данным IV либо начинать новую согласованную попытку с новым IV и обновлением metadata/schema mapping. Просто генерировать случайный IV одного файла нельзя: импорт проверяет IV, вычисленный из item IV. Проблема относится к изменяемым файлам item, включая конфигурацию и ACL, а не только к контейнеру ресурсов.

# Что подтверждено

* Новый uploader оправдан: SchemeShard хранит конфигурацию и TabletId, а набор ресурсов нужно читать из Kesus. Пустые фильтры и `Recursive=true` возвращают все ресурсы одной транзакцией, включая вложенные, через исходные `GetProps()`: [tx_quoter_resource_describe.cpp](ydb/core/kesus/tablet/tx_quoter_resource_describe.cpp).
* Единый публичный `Resources` подходит для восстановления и не привязывает бекап к внутренним tablet/resource ids. Нужные обратные преобразования уже есть: [kesus_description.cpp](ydb/core/ydb_convert/kesus_description.cpp).
* Лексикографическая сортировка достаточна для канонических путей; при добавлении Kesus действительно требует существующего родителя: [quoter_resource_tree.cpp](ydb/core/kesus/tablet/quoter_resource_tree.cpp).
* Обязательный файл с `resources: []\n` отделяет корректный пустой набор от отсутствующего файла и позволяет перезаписать прежний непустой набор без оставшихся файлов удалённых ресурсов. Он также избегает существующего Range GET с `contentLength - 1` для нулевого размера: [uploader](ydb/core/tx/schemeshard/schemeshard_export_uploaders.cpp), [getter](ydb/core/tx/schemeshard/schemeshard_import_getters.cpp).
* Имена файлов, публичный `CreateNodeRequest` с очищенным `config.path`, checksum по plaintext, суффикс `.enc` и разные файловые IV соответствуют предложенному формату. Metadata уже сообщает наличие ACL; список ресурсов в ней не нужен.
* Общий storage wrapper позволяет использовать один код для S3 и FS. В WIP предусмотрены FS `base_path`, отдельный timeout ожидания Kesus, закрытие pipe и ошибка вместо экспорта пустого набора при неуспешном Describe.

# Что важно для импорта

В исходном снимке наличие Kesus в `GetXxportProperties` ещё не означало поддержку импорта: `CheckAvailableInImport` пропускал этот тип, parser не читал `CreateNodeRequest`, а машина состояний не восстанавливала ресурсы. В текущем worktree реализованы расширение getter, создание узла и отдельная сохраняемая фаза восстановления по [KESUS_IMPORT.md](KESUS_IMPORT.md); сборка и тесты пока не запускались.

Две особенности tablet/RPC нельзя потерять при реализации:

* `TEvAddQuoterResource` при `ALREADY_EXISTS` сравнивает только DRR, игнорируя metering. Повтор требует Describe и полного семантического сравнения публичных свойств: [tx_quoter_resource_add.cpp](ydb/core/kesus/tablet/tx_quoter_resource_add.cpp).
* Публичное создание ресурса с metering требует администратора, а прямой pipe не выполняет эту проверку. Разрешение нужно перенести в серверный import и сохранить для reboot: [rpc_rate_limiter_api.cpp](ydb/core/grpc_services/rpc_rate_limiter_api.cpp), [rpc_operation_request_base.h](ydb/core/grpc_services/rpc_operation_request_base.h).

# Границы решения и проверки

Описание узла в SchemeShard и ресурсы Kesus читаются последовательно, поэтому общего атомарного снимка у них нет. Повторное чтение при reboot может дать другую версию набора. Это нужно явно обозначить как семантику экспорта; сортировка и один файл не дают согласованности между разными источниками.

Все ресурсы передаются одним actor event, преобразуются в один protobuf и сериализуются целиком. Для больших деревьев остаётся проверить память, размер события/файла и время обработки. По одному чтению кода нельзя объявить этот путь подходящим для неограниченного числа ресурсов.

Тесты WIP покрывают S3/FS, пустые ресурсы, шифрование, порядок, наследование, optional-поля, ACL/checksums, reboot с изменением набора, ошибку Describe и поздний timeout. Для законченного решения нужны export→import round trip, перезапуск импортера после Add и metering-конфликты. Проверка изменения IV относится к отложенной задаче экспорта. Семафоры, сессии, счётчики потребления и непубличные свойства остаются за пределами бекапа, как и заявлено в исходном документе.

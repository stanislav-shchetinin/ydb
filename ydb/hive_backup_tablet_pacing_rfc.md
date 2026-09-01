# RFC: ограничение фоновой нагрузки от backup-таблеток в Hive

**Статус:** Draft  
**Дата:** 2026-08-28  
**Компонент:** YDB Hive  
**Связанные подсистемы:** SchemeShard, DataShard, BlobStorage

## Краткое описание

При создании export/backup операций SchemeShard создаёт backup-копии таблиц с большим числом DataShard-таблеток. Массовый запуск этих таблеток создаёт кратковременный всплеск управляющего и системного трафика: одновременно выполняются tablet boot, чтение служебного состояния, регистрация на нодах, обмен по Interconnect и обработка событий в Hive.

Предлагается выделить запуск backup-таблеток в отдельный класс фоновой работы и ограничивать его в Hive с помощью:

1. отдельной логической очереди;
2. token bucket для ограничения среднего темпа запуска;
3. глобального и per-node лимитов одновременно стартующих backup-таблеток;
4. приоритета foreground-таблеток над backup-таблетками;
5. существующих общих лимитов Hive как жёсткой верхней границы суммарной нагрузки.

Вторая, независимая фаза решения ограничивает удаление backup-таблеток. Для удаления также используются две очереди, но foreground и backup операции разделяют одно общее окно `MaxDeleteTabletInProgress`. Backup-удаления могут занимать только ограниченную долю этого окна.

Решение намеренно не пытается регулировать чтение пользовательских данных, выгрузку в S3 и compaction borrowed parts. Эта работа выполняется в DataShard/BlobStorage и не может надёжно управляться через скорость boot в Hive.

## Мотивация

### Наблюдаемая проблема

Export большой базы может одновременно создать тысячи backup DataShard-таблеток. Текущая boot queue Hive ограничивает размер одного прохода и число стартующих таблеток на ноде, но не задаёт средний темп запуска. Если доступна ёмкость, последовательные проходы очереди выполняются практически без паузы.

В результате возникают:

- массовые tablet boot и init-чтения;
- всплеск событий в Hive и Local;
- дополнительный трафик по Interconnect;
- кратковременная конкуренция за CPU, memory и actor pools;
- рост latency пользовательских запросов и системных операций;
- конкуренция backup-таблеток с foreground-таблетками при восстановлении нод или кластера.

При завершении export возникает похожий всплеск удаления: большое количество backup-таблеток одновременно блокирует и удаляет tablet storage.

### Почему недостаточно уменьшить boot priority

Приоритет определяет порядок обработки только при наличии конкуренции в одной очереди. Если foreground-очередь пуста, тысячи backup-таблеток всё равно будут запущены максимально быстро. Для ограничения нагрузки нужен admission control, ограничивающий throughput и число операций in flight.

### Граница эффективности Hive

Export выполняется в несколько стадий:

1. SchemeShard создаёт backup-таблицы и их таблетки.
2. Hive запускает созданные таблетки.
3. CopyTable конфигурирует destination DataShard'ы и ждёт завершения общей consistent-copy операции.
4. SchemeShard запускает backup-транзакции на шардах, которые читают данные и выгружают их во внешнее хранилище.
5. После export backup-таблицы удаляются, а исходные DataShard'ы могут выполнять compaction возвращённых borrowed parts.

Hive непосредственно контролирует только запуск и Hive-часть удаления таблеток. Замедление boot сдвигает начало стадии 4, но не уменьшает её concurrency: после завершения общего `CopyTables` backup-транзакции могут быть отправлены всем таблицам и их шардам одновременно.

Поэтому данный RFC решает две локальные задачи:

- сглаживание boot/control-plane spike;
- сглаживание Hive/BlobStorage control-plane spike при удалении.

Он не является полным решением ограничения ресурсоёмкости export.

## Цели

- Ограничить средний темп запуска backup-таблеток.
- Ограничить глобальное и per-node число одновременно стартующих backup-таблеток.
- Не позволять backup-работе задерживать запуск foreground-таблеток при наличии конкуренции.
- Сохранить существующие общие лимиты Hive как hard limits.
- Не допустить spin loop и чрезмерной частоты транзакций Hive.
- Обеспечить корректное поведение при restart Hive и динамическом изменении конфигурации.
- Добавить наблюдаемость, достаточную для настройки и безопасного rollout.
- Ограничить долю backup-удалений, не увеличивая общий deletion concurrency.

## Не цели

- Ограничение числа одновременно выполняющихся backup-транзакций в DataShard.
- Ограничение чтения исходных таблиц, PDisk/VDisk I/O или S3 bandwidth.
- Управление compaction borrowed parts.
- Пейсинг создания таблеток и запросов выбора storage groups.
- Изменение поведения tablet balancer.
- Гарантия завершения backup при постоянно исчерпанной foreground capacity.
- Изменения SchemeShard или DataShard в рамках этого RFC.

## Термины

- **Foreground tablet** — таблетка, не помеченная `IsBackup`.
- **Backup tablet** — лидер таблетки, для которого persisted-признак `IsBackup` установлен SchemeShard.
- **Starting** — volatile-состояние таблетки от принятия решения о запуске до подтверждения успешного старта или выхода из этого состояния.
- **Rate limit** — ограничение среднего числа новых запусков или удалений в секунду.
- **In-flight limit** — ограничение числа уже начатых, но не завершённых операций.
- **Hard limit** — ограничение, которое не снимается из-за возраста записи или нагрузки на очередь.

## Предлагаемое решение

### 1. Классификация backup-таблеток

Источником истины является существующий persisted-признак `IsBackup`, переданный SchemeShard при создании таблетки.

Для follower используется признак лидера. Это сохраняет единое решение для tablet family, хотя текущий export обычно создаёт backup-таблетки без followers.

Классификация применяется в точке добавления таблетки в boot queue и в точке постановки удаления. Она не должна зависеть от tablet type, поскольку backup и foreground таблетки имеют одинаковый тип `DataShard`.

### 2. Две логические boot-очереди

Hive поддерживает два класса ожидающих запусков:

- main boot queue для foreground и системных таблеток;
- backup boot queue для `IsBackup`.

В одном проходе сначала обрабатывается main queue. После неё Hive может обработать backup queue, если осталась общая capacity.

Если после прохода main ready queue всё ещё непуста, backup-проход пропускается, а main queue перепланируется. Таким образом, ограничение `MaxBootBatchSize` не превращается в возможность запустить background работу впереди уже ожидающей foreground работы.

Backup-запуски:

- не расходуют `MaxBootBatchSize`, выделенный main queue;
- учитываются собственным ограничением числа просмотренных записей за проход;
- всегда подчиняются существующему общему per-node `MaxTabletsScheduled`;
- не запускаются, если main queue остановилась из-за исчерпания общей start capacity.

Обработка обоих классов может выполняться внутри одного `TTxProcessBootQueue`. Это уменьшает число actor events при одновременной работе очередей, но не является требованием корректности.

### 3. Token bucket для backup boot

Для ограничения среднего темпа используется token bucket:

```text
tokens = min(burst, tokens + rate * elapsed)
rateBudget = floor(tokens)
```

Один токен списывается при реальной попытке выбрать ноду и запустить существующую, ещё не запущенную таблетку. Устаревшая запись очереди токен не расходует.

Начальный бюджет после старта Hive должен быть ограничен, чтобы restart Hive не создавал полный burst. Рекомендуемое начальное значение — не больше одного токена.

Token bucket должен корректно обрабатывать floating-point rounding. При вычислении следующего пробуждения используется минимальная задержка, исключающая zero-delay или microsecond spin.

#### Частота пробуждений

Token bucket не должен порождать отдельную транзакцию на каждый токен при высокой configured rate. Hive использует ограниченную частоту timer tick, например не чаще одного раза в 100–500 мс, и за один проход расходует накопившийся бюджет.

Точное значение tick задаётся константой первой реализации. Выносить его в конфигурацию без эксплуатационной необходимости не требуется.

### 4. Ограничения числа стартующих таблеток

Backup boot разрешён только при одновременном выполнении условий:

```text
backupStartingGlobal < MaxBackupTabletsStarting
backupStartingOnCandidateNode < MaxBackupTabletsStartingPerNode
allStartingOnCandidateNode < MaxTabletsScheduled
rateBudget > 0
```

Итоговый бюджет прохода:

```text
backupBudget = min(
    rateBudget,
    MaxBackupTabletsStarting - backupStartingGlobal,
    batchScanBudget
)
```

Per-node limit проверяется при выборе candidate node. Нода, достигшая backup-лимита, исключается из кандидатов, но поиск продолжается на остальных нодах.

Счётчики обновляются централизованно при смене volatile state. Это исключает расхождение между несколькими путями завершения старта, отключением ноды и удалением таблетки.

Все лимиты являются hard limits и никогда не обходятся по таймеру.

### 5. Foreground priority и отсутствие fail-open aging

Foreground queue всегда обрабатывается первой. Backup использует только capacity, оставшуюся после foreground scheduling.

Если foreground workload постоянно исчерпывает start capacity, backup может не продвигаться. Это намеренное QoS-поведение: доступность пользовательских таблеток важнее завершения фонового export.

Для контроля starvation добавляются:

- метрика возраста старейшей backup-записи;
- warning/alert при превышении операционного порога;
- возможность отменить или перепланировать export на уровне вызывающей подсистемы.

Возраст записи не снимает rate, global, per-node или общие hard limits. В частности, не вводится режим, который после `MaxDelay` принудительно запускает backup на перегруженной или полностью занятой ноде.

### 6. Очереди ready, deferred и wait

Строгий FIFO не используется как единственный источник готовых записей, поскольку одна таблетка с будущим `PostponedStart` может заблокировать всю backup queue.

Предлагается разделить состояние:

- `BackupReadyQueue` — записи, готовые к попытке запуска;
- `BackupDeferredQueue` — записи с известным `PostponedStart`, упорядоченные по времени готовности;
- `BackupWaitQueue` — записи, для которых нет допустимой ноды по устойчивым ограничениям размещения.

Каждый проход ограничивает число просмотренных записей. При наступлении ближайшего `PostponedStart` записи возвращаются в ready queue. Wait queue пересматривается при изменении состава/состояния нод и страховочным редким timer tick.

Enqueue time хранится отдельно для метрик и не участвует в снятии hard limits.

### 7. Размещение

В MVP используются существующий `FindBestNode`, общие resource restrictions Hive и новый per-node backup-start limit.

Отдельный запрет размещения по глобальному p90 node usage не вводится. Текущий `NodeUsage` отражает compute/memory/actor-pool нагрузку, но не PDisk/VDisk pressure и не нагрузку конкретных storage groups. Такой сигнал недостаточно точно соответствует основной нагрузке export.

Адаптивное ограничение по нагрузке может быть добавлено отдельным RFC после появления измерений и подходящего сигнала с hysteresis.

### 8. Tablet balancer

В рамках этого RFC balancer не изменяется:

- backup tablets не получают `POLICY_IGNORE`;
- их balancer weight не увеличивается;
- работающая backup-таблетка не перемещается только ради снижения фоновой нагрузки.

`POLICY_IGNORE` опасен тем, что при перегруженной backup-нагрузкой ноде балансер может начать перемещать foreground-таблетки. Предпочтительное перемещение самих backup-таблеток также не принимается без доказательства, что restart не создаёт повторное чтение, upload amplification и дополнительную нагрузку.

Если потребуется preemption backup-work, оно должно проектироваться как отдельное явное поведение, а не как побочный эффект балансировки.

### 9. Динамическая конфигурация

Минимальная конфигурация boot pacing:

```text
BackupBootPacingEnabled
BackupBootRate
BackupBootBurst
MaxBackupTabletsStarting
MaxBackupTabletsStartingPerNode
```

Все числовые настройки валидируются до применения:

- значения конечны и не отрицательны;
- `rate > 0`, если pacing включён;
- `burst >= 1`;
- global/per-node limits положительны;
- per-node limit не превышает общий per-node start limit без явного обоснования.

При включении pacing на работающем Hive уже поставленные в main queue backup records должны быть переклассифицированы. Иначе часть backlog обойдёт limiter.

При отключении pacing backup records переносятся в main queue без потери wait/deferred state.

### 10. Restart и rollback

После restart Hive очередь восстанавливается из persisted tablet state и `IsBackup`.

Инварианты после восстановления:

- global и per-node starting counters пересобраны из фактического volatile state либо начинают с заведомо безопасного состояния;
- token bucket не стартует с большим burst;
- backup tablets не теряются между ready/wait состояниями;
- correctness не зависит от volatile enqueue time;
- откат на версию без pacing сохраняет корректность таблеток, хотя QoS возвращается к прежнему поведению.

### 11. Пейсинг удаления backup-таблеток

Пейсинг удаления реализуется отдельной фазой после подтверждения, что Hive/BlobStorage deletion control plane является существенной частью завершающего всплеска.

#### Общий лимит

Foreground и backup deletions разделяют существующий общий hard limit:

```text
totalDeleteInFlight <= MaxDeleteTabletInProgress
backupDeleteInFlight <= MaxBackupDeleteInProgress
```

`MaxBackupDeleteInProgress` задаёт максимальную долю общего окна, которую может занять backup. Например, при общем лимите 100 и backup limit 16 не меньше 84 слотов всегда остаются доступными foreground deletion после завершения уже начатых операций.

Не создаётся второе независимое окно поверх `MaxDeleteTabletInProgress`.

#### Приоритет и rate

- foreground delete queue обслуживается первой;
- backup delete queue использует оставшуюся общую capacity;
- backup deletions дополнительно ограничены отдельным token bucket;
- возраст очереди не снимает hard limits.

#### In-flight state

Каждая запущенная операция хранится в tagged registry:

```text
tabletId -> {class: foreground|backup, generation/requestId}
```

Completion освобождает слот только для известной операции соответствующего класса. Duplicate, stale или неизвестный результат не должен декрементировать чужой счётчик.

После restart deleting tablets заново классифицируются по persisted `IsBackup`, а in-flight state восстанавливается или безопасно переигрывается согласно существующей idempotency удаления.

#### Ограничение эффекта

Hive pacing управляет блокировкой и удалением tablet storage. Он не управляет compaction borrowed parts на исходных DataShard'ах. Поэтому успех этой фазы оценивается отдельно от общего disk spike после export.

## Наблюдаемость

Минимальные gauges:

- `BackupBootQueueSize`;
- `BackupBootDeferredQueueSize`;
- `BackupBootWaitQueueSize`;
- `BackupTabletsStarting`;
- `BackupBootOldestDelayMs`;
- `BackupBootEffectiveRate`;
- `BackupDeleteQueueSize`;
- `BackupDeleteInFlight`;
- `BackupDeleteOldestDelayMs`;
- суммарный `DeleteInFlight` по классам.

Минимальные cumulative counters:

- backup starts attempted/succeeded/failed;
- записи, пропущенные как stale;
- постановки в deferred/wait queue;
- throttling по rate/global/per-node/total capacity;
- backup deletions attempted/succeeded/retried;
- duplicate/stale delete completions.

На monitoring page Hive должны быть видны текущая конфигурация, размеры очередей, effective rate и причины throttling.

## Критерии успеха

До rollout задаются численные SLO. Как минимум измеряются:

- изменение p99 пользовательских запросов во время стадии boot;
- latency запуска foreground-таблетки при большой backup queue;
- максимальное global и per-node число backup tablets в `STARTING`;
- фактический средний и burst rate;
- длительность стадии `CopyTables` и время schema locks;
- полное время export;
- нагрузка Hive actor/event queue и Interconnect;
- для deletion-фазы — BSC/BlobStorage latency и суммарный deletion concurrency.

Функциональный критерий:

- foreground tablet, добавленная при непустой backup queue, не ждёт rate token backup-класса;
- ни один hard limit не превышается;
- backup queue продвигается при наличии свободной общей capacity;
- restart/config toggle не позволяют backlog обойти pacing.

## Риски и проблемы решения

### Решение не ограничивает основную export-работу

После завершения `CopyTables` SchemeShard может запустить backup-транзакции на всех шардах. Если основная деградация создаётся чтением и S3 upload, эффект Hive pacing будет ограниченным.

### Увеличение времени CopyTable и schema locks

Низкий boot rate увеличивает время, в течение которого consistent-copy операция ждёт destination shards. Это может задерживать несовместимые DDL и увеличивать общее время export.

Нужны отдельные метрики длительности `CopyTables` и операционный upper bound. Этот риск нельзя устранять обходом hard limits.

### Starvation при постоянной foreground-нагрузке

Backup может долго не продвигаться, если foreground полностью использует start capacity. Это сознательный выбор QoS. Длительное ожидание должно приводить к alert или отмене export, а не к fail-open запуску.

### Ошибочно выбранные rate и limits

Слишком высокие значения не защитят latency; слишком низкие значительно увеличат время export и schema locks. Значения должны подбираться на canary по метрикам, а не становиться активными defaults без измерений.

### Нагрузка на сам Hive

Слишком частые timer events могут превратить pacing в значимый источник Hive transactions. Частота tick и scan budget должны иметь жёсткую нижнюю/верхнюю границу.

### Head-of-line blocking и рост памяти

Миллионы записей требуют bounded processing и предсказуемого memory footprint. Необходимо разделение ready/deferred/wait, а не строгий FIFO с блокировкой всей очереди одной записью.

### Restart и динамический config

Неполная переклассификация backlog, полный burst после restart или неправильное восстановление counters могут временно отключить защиту либо навсегда остановить очередь.

### Duplicate и stale events

Actor events и delete results могут быть повторными или относиться к уже удалённой таблетке. Все счётчики in-flight должны изменяться идемпотентно по tagged operation state.

### Неполный сигнал нагрузки

Hive node usage не отражает PDisk/VDisk и конкретные storage groups. Добавление adaptive rate на этом сигнале может остановить backup по несвязанной причине или не заметить реальную disk overload.

### Взаимодействие с balancer

Даже без изменений балancer может перемещать работающие backup-таблетки. Это следует наблюдать метриками. Автоматическое предпочтение backup как жертв может увеличить повторное чтение и не входит в MVP.

### Ограниченный эффект deletion pacing

Удаление tablet storage — только часть завершающей нагрузки. Compaction returned borrowed parts остаётся вне контроля Hive.

## Совместимость

- Новых wire- или persisted-форматов не требуется: используется существующий `IsBackup`.
- Новые config fields являются additive и по умолчанию выключены.
- Старые версии Hive игнорируют неизвестные config fields и сохраняют прежнее QoS-поведение.
- Откат не должен влиять на correctness export или tablet lifecycle; исчезает только pacing.
- Dynamic enable/disable требует явной миграции записей между очередями.

## Rollout

1. Добавить метрики без изменения поведения и снять baseline.
2. Включить boot pacing на тестовом кластере с консервативными значениями.
3. Проверить функциональные инварианты и длительность `CopyTables`.
4. Выполнить canary на одном production database/domain.
5. Сравнить пользовательский p99, foreground boot latency и время export.
6. Постепенно расширять rollout.
7. Delete pacing включать отдельно, после независимой оценки его эффекта.

Rollback выполняется отключением feature flag. При отключении все backup records переносятся в общие очереди.

## Декомпозиция задач

### Этап 0. Baseline и контракты

#### 0.1. Разметить стадии export

- Добавить или проверить метрики длительности create/copy/boot/transfer/drop.
- Отделить boot spike от DataShard backup traffic.
- Зафиксировать baseline p99 и Interconnect/Hive load.

**Результат:** подтверждено, что boot является самостоятельным значимым источником деградации.

#### 0.2. Проверить контракт CopyTable

- Определить DDL, блокируемые состоянием copying.
- Проверить cancellation/retry/restart при долгом ожидании destination shards.
- Задать допустимый бюджет увеличения `CopyTables`.

**Результат:** выбран rate, не нарушающий schema-operation SLO.

### Этап 1. Boot pacing MVP

#### 1.1. Классификация и структуры очередей

- Маршрутизировать `IsBackup` в backup queue.
- Добавить ready/deferred/wait структуры.
- Ограничить scan budget одного прохода.
- Исключить head-of-line blocking.

#### 1.2. Реализовать token bucket

- Rate и burst.
- Безопасное floating-point округление.
- Ограниченная частота timer tick.
- Без большого initial burst после restart.
- Unit tests граничных значений.

#### 1.3. Реализовать global/per-node admission

- Global backup starting counter.
- Per-node backup starting counter.
- Обновление в единой точке volatile-state transition.
- Продолжение поиска по другим нодам при достижении per-node cap.
- Сохранение общего `MaxTabletsScheduled` как hard limit.

#### 1.4. Интегрировать с main boot loop

- Main queue обрабатывается первой.
- Backup использует оставшуюся capacity.
- Backup имеет собственный batch/scan budget.
- Throttling foreground не обходится background-проходом.

#### 1.5. Dynamic config и restart

- Валидация полей.
- Переклассификация существующего backlog при enable.
- Слияние очередей при disable.
- Восстановление после restart Hive.
- Проверка rollback на версию без pacing.

#### 1.6. Метрики и monitoring

- Размеры всех очередей.
- In-flight global/per-node.
- Effective rate и причины throttling.
- Oldest delay и stale records.
- Отображение active config.

#### 1.7. Тесты boot pacing

- Rate и burst.
- Global и per-node limits.
- Foreground tablet не ждёт backup token.
- Постоянная foreground saturation.
- Deferred head не блокирует ready records.
- No-node/wait-queue recovery.
- Restart Hive с backlog и in-flight starts.
- Enable/disable с уже непустой main queue.
- Stale/duplicate status events.
- Полный регресс Hive tests.

#### 1.8. Нагрузочный тест и canary

- Export таблицы с тысячами шардов.
- Одновременная пользовательская нагрузка.
- Сравнение boot phase p99, total export time и CopyTable lock time.

### Этап 2. Delete pacing

#### 2.1. Подтвердить необходимость

- Измерить долю Hive/BSC deletion traffic в завершающем spike.
- Отделить её от borrowed-parts compaction.

#### 2.2. Разделить delete queues при общем окне

- Foreground и backup queues.
- Единый `totalDeleteInFlight` hard limit.
- Максимальный backup share.
- Foreground priority.

#### 2.3. Добавить delete token bucket

- Отдельные rate/burst.
- Переиспользование проверенного pacer primitive.
- Никакого bypass hard limits по возрасту.

#### 2.4. Сделать completion идемпотентным

- Tagged in-flight registry с class и request generation.
- Duplicate/stale completion не изменяет counters.
- Корректное восстановление после restart.

#### 2.5. Метрики и тесты удаления

- Общий и class-specific inflight.
- Foreground delete при большом backup backlog.
- Проверка `total <= MaxDeleteTabletInProgress`.
- Retry, duplicate result, no-storage tablet, restart Hive.
- Нагрузочный тест BSC/BlobStorage.

### Этап 3. Возможные последующие улучшения

Эти задачи не входят в исходный rollout и требуют отдельного design review:

- adaptive pacing по релевантным IC/PDisk/VDisk сигналам;
- hysteresis/AIMD controller;
- явное pause/preemption для backup work;
- изменения balancer policy;
- pacing создания таблеток;
- ограничение concurrency backup-транзакций в SchemeShard/DataShard.

## Открытые вопросы

1. Какой вклад boot phase в наблюдаемую деградацию p99 относительно самой backup-транзакции?
2. Каков допустимый рост длительности `CopyTables` и schema locks?
3. Какие initial значения rate, burst и global/per-node limits безопасны на разных размерах кластеров?
4. Нужно ли резервировать background share или starvation при полной foreground saturation считается допустимым?
5. Какой timer tick даёт достаточную плавность без заметной нагрузки на Hive?
6. Является ли Hive/BlobStorage deletion значимой частью завершающего spike?
7. Какие события гарантированно пробуждают wait queue и нужна ли страховочная периодическая проверка?

## Решение, предлагаемое к утверждению

Для первой поставки утверждается только статический boot pacing с двумя классами очередей, token bucket, global/per-node backup limits, общими hard limits и полной наблюдаемостью.

Не утверждаются в первой поставке:

- fail-open aging;
- глобальная адаптация по p90 node usage;
- изменение tablet balancer;
- независимое дополнительное окно удаления;
- заявления об ограничении DataShard/S3 backup concurrency.

Delete pacing поставляется отдельной фазой и использует общий deletion hard limit.

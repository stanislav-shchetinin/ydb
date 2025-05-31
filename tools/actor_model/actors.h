#include <iostream>
#include <library/cpp/actors/core/actor.h>
#include <util/generic/ptr.h>
#include <library/cpp/actors/util/should_continue.h>

NActors::IActor* CreateReadActor(NActors::TActorId writer, std::istream& strm);
NActors::IActor* CreateMaximumPrimeDevisorActor(NActors::TActorId reader, NActors::TActorId writer, int64_t value);
NActors::IActor* CreateWriteActor();

THolder<NActors::IActor> CreateSelfPingActor(const TDuration& latency);

std::shared_ptr<TProgramShouldContinue> GetProgramShouldContinue();

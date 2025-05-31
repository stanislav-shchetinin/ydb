#include "actors.h"
#include "events.h"
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>

static auto ShouldContinue = std::make_shared<TProgramShouldContinue>();

/*
Вам нужно написать реализацию TReadActor, TMaximumPrimeDevisorActor, TWriteActor
*/

/*
Требования к TReadActor:
1. Рекомендуется отнаследовать этот актор от NActors::TActorBootstrapped
2. В Boostrap этот актор отправляет себе NActors::TEvents::TEvWakeup
3. После получения этого сообщения считывается новое int64_t значение из strm
4. После этого порождается новый TMaximumPrimeDevisorActor который занимается вычислениями
5. Далее актор посылает себе сообщение NActors::TEvents::TEvWakeup чтобы не блокировать поток этим актором
6. Актор дожидается завершения всех TMaximumPrimeDevisorActor через TEvents::TEvDone
7. Когда чтение из файла завершено и получены подтверждения от всех TMaximumPrimeDevisorActor,
этот актор отправляет сообщение NActors::TEvents::TEvPoisonPill в TWriteActor

TReadActor
    Bootstrap:
        send(self, NActors::TEvents::TEvWakeup)

    NActors::TEvents::TEvWakeup:
        if read(strm) -> value:
            register(TMaximumPrimeDevisorActor(value, self, receipment))
            send(self, NActors::TEvents::TEvWakeup)
        else:
            ...

    TEvents::TEvDone:
        if Finish:
            send(receipment, NActors::TEvents::TEvPoisonPill)
        else:
            ...
*/

class TReadActor : public NActors::TActorBootstrapped<TReadActor> {
    NActors::TActorId WriteActor;
    int64_t Requests = 0;
    bool Readable = true;
    std::istream& Stream;

public:
    TReadActor(NActors::TActorId target, std::istream& strm)
        : WriteActor(target)
        , Stream(strm)
    {}

    void Bootstrap(){
        Become(&TThis::StateProgress);
        Send(SelfId(), new NActors::TEvents::TEvWakeup());
    }

    STRICT_STFUNC(StateProgress,
        hFunc(NActors::TEvents::TEvWakeup, Handle);
        hFunc(TEvents::TEvDone, Handle);
    )

    void Handle(TEvents::TEvDone::TPtr&){
        Requests--;
        if (!Requests && !Readable){
            Send(WriteActor, new NActors::TEvents::TEvPoisonPill());
            PassAway();
        }
    }

    void Handle(NActors::TEvents::TEvWakeup::TPtr&){
        int64_t value;
        if (Stream >> value){
            Register(CreateMaximumPrimeDevisorActor(SelfId(), WriteActor, value));
            Requests++;
            Send(SelfId(), new NActors::TEvents::TEvWakeup());
        } else {
            Readable = false;
        }
    }
};

// TODO: напишите реализацию TReadActor

/*
Требования к TMaximumPrimeDevisorActor:
1. Рекомендуется отнаследовать этот актор от NActors::TActorBootstrapped
2. В конструкторе этот актор принимает:
 - значение для которого нужно вычислить простое число
 - ActorId отправителя (ReadActor)
 - ActorId получателя (WriteActor)
2. В Boostrap этот актор отправляет себе NActors::TEvents::TEvWakeup по вызову которого происходит вызов Handler для вычислений
3. Вычисления нельзя проводить больше 10 миллисекунд
4. По истечении этого времени нужно сохранить текущее состояние вычислений в акторе и отправить себе NActors::TEvents::TEvWakeup
5. Когда результат вычислен он посылается в TWriteActor c использованием сообщения TEvWriteValueRequest
6. Далее отправляет ReadActor сообщение TEvents::TEvDone
7. Завершает свою работу

TMaximumPrimeDevisorActor
    Bootstrap:
        send(self, NActors::TEvents::TEvWakeup)

    NActors::TEvents::TEvWakeup:
        calculate
        if > 10 ms:
            Send(SelfId(), NActors::TEvents::TEvWakeup)
        else:
            Send(WriteActor, TEvents::TEvWriteValueRequest)
            Send(ReadActor, TEvents::TEvDone)
            PassAway()
*/

// TODO: напишите реализацию TMaximumPrimeDevisorActor

class TMaximumPrimeDevisorActor : public NActors::TActorBootstrapped<TMaximumPrimeDevisorActor> {
    NActors::TActorId ReadActor;
    NActors::TActorId WriteActor;
    int64_t Number;

public:
    TMaximumPrimeDevisorActor(const NActors::TActorId& readActor, NActors::TActorId writeTarget, int64_t number)
        : ReadActor(readActor)
        , WriteActor(writeTarget)
        , Number(number)
    {}

    STRICT_STFUNC(StateCalculation,
        hFunc(NActors::TEvents::TEvWakeup, Handle);
    )

    void Bootstrap(){
        Become(&TThis::StateCalculation);
        Send(SelfId(), new NActors::TEvents::TEvWakeup());
    }

    void Handle(NActors::TEvents::TEvWakeup::TPtr&){
        auto end = TInstant::Now() + TDuration::MilliSeconds(10);
        int64_t currentNumber = 2;
        int64_t maxPrime = 1;

        while(TInstant::Now() < end && currentNumber <= Number){
            if (Number % currentNumber == 0 && IsPrime(currentNumber)) {
                maxPrime = currentNumber;
            }
            currentNumber++;
        }

        if (currentNumber <= Number) {
            Send(SelfId(), new NActors::TEvents::TEvWakeup());
            return;
        }

        Send(WriteActor, new TEvents::TEvWriteValueRequest(maxPrime));
        Send(ReadActor, new TEvents::TEvDone());
        PassAway();
    }

private:
    bool IsPrime(int64_t number) {
        if (number <= 1) {
            return false;
        }

        for (int64_t i = 2; i * i <= number; ++i) {
            if (number % i == 0) {
                return false;
            }
        }

        return true;
    }
};

/*
Требования к TWriteActor:
1. Рекомендуется отнаследовать этот актор от NActors::TActor
2. Этот актор получает два типа сообщений NActors::TEvents::TEvPoisonPill::EventType и TEvents::TEvWriteValueRequest
2. В случае TEvents::TEvWriteValueRequest он принимает результат посчитанный в TMaximumPrimeDevisorActor и прибавляет его к локальной сумме
4. В случае NActors::TEvents::TEvPoisonPill::EventType актор выводит в Cout посчитанную локальнкую сумму, проставляет ShouldStop и завершает свое выполнение через PassAway

TWriteActor
    TEvents::TEvWriteValueRequest ev:
        Sum += ev->Value

    NActors::TEvents::TEvPoisonPill::EventType:
        Cout << Sum << Endl;
        ShouldStop()
        PassAway()
*/

// TODO: напишите реализацию TWriteActor

class TWriteActor : public NActors::TActor<TWriteActor> {
    int64_t Sum = 0;

public:
    TWriteActor()
        : TActor(&TThis::StateProgress)
    {}

    STRICT_STFUNC(StateProgress,
        hFunc(TEvents::TEvWriteValueRequest, Handle);
        hFunc(NActors::TEvents::TEvPoisonPill, Handle);
    )

    void Handle(TEvents::TEvWriteValueRequest::TPtr& ev){
        Sum += ev->Get()->Value;
    }

    void Handle(NActors::TEvents::TEvPoisonPill::TPtr&){
        Cout << Sum << Endl;
        ShouldContinue->ShouldStop(0);
        PassAway();
    }

};

class TSelfPingActor : public NActors::TActorBootstrapped<TSelfPingActor> {
    TDuration Latency;
    TInstant LastTime;

public:
    TSelfPingActor(const TDuration& latency)
        : Latency(latency)
    {}

    void Bootstrap() {
        LastTime = TInstant::Now();
        Become(&TSelfPingActor::StateFunc);
        Send(SelfId(), std::make_unique<NActors::TEvents::TEvWakeup>());
    }

    STRICT_STFUNC(StateFunc, {
        cFunc(NActors::TEvents::TEvWakeup::EventType, HandleWakeup);
    });

    void HandleWakeup() {
        auto now = TInstant::Now();
        TDuration delta = now - LastTime;
        Y_VERIFY(delta <= Latency, "Latency too big");
        LastTime = now;
        Send(SelfId(), std::make_unique<NActors::TEvents::TEvWakeup>());
    }
};

NActors::IActor* CreateReadActor(NActors::TActorId writer, std::istream& strm){
    return new TReadActor(writer, strm);
}

NActors::IActor* CreateMaximumPrimeDevisorActor(const NActors::TActorId& reader, const NActors::TActorId& writer, int64_t value){
    return new TMaximumPrimeDevisorActor(reader, writer, value);
}

NActors::IActor* CreateWriteActor(){
    return new TWriteActor();
}

THolder<NActors::IActor> CreateSelfPingActor(const TDuration& latency) {
    return MakeHolder<TSelfPingActor>(latency);
}

std::shared_ptr<TProgramShouldContinue> GetProgramShouldContinue() {
    return ShouldContinue;
}

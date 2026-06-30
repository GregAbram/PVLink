#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"

struct FOutgoingMessage
{
    int32         Type;
    TArray<uint8> Payload;
};

/**
 * Background thread that listens on a TCP port for outbound messages (UE → Python).
 *
 * Game-thread code calls EnqueueMessage(); this thread drains the queue and
 * sends each message, waiting for a 4-byte ack from Python before continuing.
 *
 * Same framing as the inbound channel:
 *   [int32 Count][int32 Type][Count bytes payload]  →  ack [int32 Status]
 */
class FSocketSenderRunnable : public FRunnable
{
public:
    explicit FSocketSenderRunnable(int32 InPort);
    virtual ~FSocketSenderRunnable();

    virtual bool   Init()   override;
    virtual uint32 Run()    override;
    virtual void   Stop()   override;

    /** Safe to call from any thread at any time. */
    void EnqueueMessage(int32 Type, TArray<uint8> Payload);

private:
    int32    Port;
    FSocket* ListenerSocket   = nullptr;
    FSocket* ConnectionSocket = nullptr;
    TAtomic<bool> bShouldStop{false};

    TQueue<FOutgoingMessage, EQueueMode::Mpsc> OutgoingQueue;

    bool ReadExact(FSocket* Socket, uint8* Buffer, int32 NumBytes);
    bool SendFramed(FSocket* Socket, int32 Type, const TArray<uint8>& Payload);
    bool ReadAck(FSocket* Socket, int32& OutStatus);
};

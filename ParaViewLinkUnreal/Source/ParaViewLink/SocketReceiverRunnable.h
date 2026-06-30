#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

/**
 * Background thread that listens on a TCP port for inbound messages (Python → UE).
 *
 * Wire format (little-endian):
 *   [int32 Count][int32 Type][Count bytes payload]  →  ack [int32 Status]
 *
 * When a complete message arrives, OnMessageReceived is called on the NETWORK
 * thread. The owner (USocketReceiverSubsystem) sets this before starting the thread.
 */
class FSocketReceiverRunnable : public FRunnable
{
public:
    explicit FSocketReceiverRunnable(int32 InPort);
    virtual ~FSocketReceiverRunnable();

    virtual bool   Init()   override;
    virtual uint32 Run()    override;
    virtual void   Stop()   override;

    /**
     * Called on the network thread for every complete inbound message.
     * Set by the owning subsystem before the thread starts.
     * Clear it (set to nullptr) before destroying the subsystem.
     */
    TFunction<void(int32 Cmd, TArray<uint8> Payload)> OnMessageReceived;

private:
    int32    Port;
    FSocket* ListenerSocket   = nullptr;
    FSocket* ConnectionSocket = nullptr;
    TAtomic<bool> bShouldStop{false};

    bool ReadExact(FSocket* Socket, uint8* Buffer, int32 NumBytes);
    bool SendAck(FSocket* Socket, int32 Status);
};

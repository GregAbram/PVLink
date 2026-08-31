#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

/**
 * Background thread that dials out to the DataManager and reads inbound
 * messages (Python → UE) from that connection.
 *
 * Wire format (little-endian):
 *   [int32 Count][int32 Type][Count bytes payload]  →  ack [int32 Status]
 *
 * When a complete message arrives, OnMessageReceived is called on the NETWORK
 * thread. The owner (USocketReceiverSubsystem) sets this before starting the thread.
 *
 * If the connection can't be established, or drops, this keeps retrying
 * (about once a second) rather than giving up -- the DataManager may not be
 * up yet, or may restart independently of this client.
 */
class FSocketReceiverRunnable : public FRunnable
{
public:
    FSocketReceiverRunnable(const FString& InHost, int32 InPort);
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
    FString  Host;
    int32    Port;
    FSocket* ConnectionSocket = nullptr;
    TAtomic<bool> bShouldStop{false};

    /** Runs the read/dispatch/ack loop over an already-connected socket
     *  until the peer disconnects or bShouldStop is set. */
    void ReadMessageLoop(FSocket* Socket);

    bool ReadExact(FSocket* Socket, uint8* Buffer, int32 NumBytes);
    bool SendAck(FSocket* Socket, int32 Status);
};

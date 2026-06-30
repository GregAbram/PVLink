#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SocketReceiverSubsystem.generated.h"

class FSocketReceiverRunnable;
class FSocketSenderRunnable;

/**
 * Base singleton socket manager — inbound (port 9001) + outbound (port 9002).
 * Automatically created with the Game Instance; nothing needs placing in the level.
 *
 * Subclass this and override HandleRawMessage() to process inbound messages.
 * HandleRawMessage is called on the NETWORK thread — do not touch UObjects there.
 * Push work to a TQueue and process it in a Tick (see UMeshReceiverSubsystem).
 *
 * DefaultGame.ini:
 *   [/Script/Greg.SocketReceiverSubsystem]
 *   InboundPort=9001
 *   OutboundPort=9002
 */
UCLASS()
class PARAVIEWLINK_API USocketReceiverSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** Send raw bytes to Python on the outbound connection. Game-thread safe. */
    bool SendMessage(int32 Type, const TArray<uint8>& Payload);

    /** Convenience: encodes Text as UTF-8 and calls SendMessage. */
    bool SendString(int32 Type, const FString& Text);

protected:
    /**
     * Called on the NETWORK thread when a complete inbound message arrives.
     * Default implementation logs the payload as a UTF-8 string.
     * Override to parse and dispatch — keep it fast, no UObject access.
     */
    virtual void HandleRawMessage(int32 Cmd, TArray<uint8> Payload);

private:
    FSocketReceiverRunnable* Receiver       = nullptr;
    FRunnableThread*         ReceiverThread = nullptr;

    FSocketSenderRunnable*   Sender         = nullptr;
    FRunnableThread*         SenderThread   = nullptr;
};

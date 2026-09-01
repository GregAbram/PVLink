#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "DataManagerDiscovery.h"
#include "SocketReceiverSubsystem.generated.h"

class FSocketReceiverRunnable;
class FSocketSenderRunnable;

/**
 * Base singleton socket manager — dials out to the DataManager (inbound
 * messages) + a still-unused outbound (port 9002) listener for a UE→Python
 * direction nothing in this plugin currently sends on.
 * Automatically created with the Game Instance; nothing needs placing in the level.
 *
 * Subclass this and override HandleRawMessage() to process inbound messages.
 * HandleRawMessage is called on the NETWORK thread — do not touch UObjects there.
 * Push work to a TQueue and process it in a Tick (see UMeshReceiverSubsystem).
 *
 * Also starts a UDP discovery listener (FDataManagerDiscovery) alongside the
 * TCP connection, so DataManagers currently on the LAN can be enumerated
 * via GetDiscoveredDataManagers() and switched to via ConnectToDataManager()
 * -- e.g. once a deployed build shouldn't have its DataManager address
 * hardcoded. No UI is built on top of this yet.
 *
 * If DataManagerHost is empty, nothing is dialed at startup -- instead this
 * waits AutoConnectSettleSeconds for the discovery list to settle, and
 * auto-connects only if exactly one DataManager was found (ambiguous or
 * empty results just stay idle until ConnectToDataManager() is called
 * explicitly, e.g. from a UI once one exists).
 *
 * DefaultGame.ini:
 *   [/Script/ParaViewLink.SocketReceiverSubsystem]
 *   DataManagerHost=
 *   DataManagerPort=9010
 *   OutboundPort=9002
 *   DiscoveryPort=9011
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

    /** Snapshot of DataManagers currently visible via UDP discovery broadcast.
     *  No UI reads this yet -- it's the underlying capability for whatever
     *  picker/settings UI comes later. */
    UFUNCTION(BlueprintCallable, Category = "PVLink")
    TArray<FDiscoveredDataManager> GetDiscoveredDataManagers() const;

    /** Re-point the live connection at a specific DataManager, e.g. one
     *  chosen from GetDiscoveredDataManagers(). Stops the current connection
     *  and starts a fresh one -- same connect-with-retry logic, just
     *  re-targeted. Does not touch DefaultGame.ini; the new target only
     *  lasts for this running instance. */
    UFUNCTION(BlueprintCallable, Category = "PVLink")
    void ConnectToDataManager(const FString& Host, int32 Port);

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

    FDataManagerDiscovery*   Discovery       = nullptr;
    FRunnableThread*         DiscoveryThread = nullptr;

    static constexpr float AutoConnectSettleSeconds = 3.0f;
    FTSTicker::FDelegateHandle AutoConnectTickerHandle;

    /** Stops Receiver/ReceiverThread if running, then creates and starts a
     *  fresh FSocketReceiverRunnable(Host, Port) in their place. Used by
     *  both Initialize() and ConnectToDataManager(). */
    void StartReceiver(const FString& Host, int32 Port);

    /** One-shot ticker callback (see Initialize()): auto-connects if exactly
     *  one DataManager was discovered by now, otherwise stays idle and logs
     *  how many were found. Always returns false (unregisters itself). */
    bool TryAutoConnect(float DeltaTime);
};

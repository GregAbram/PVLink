#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/CriticalSection.h"
#include "DataManagerDiscovery.generated.h"

class FSocket;

/** One DataManager currently visible on the LAN. */
USTRUCT(BlueprintType)
struct FDiscoveredDataManager
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "PVLink")
    FString Host;

    UPROPERTY(BlueprintReadOnly, Category = "PVLink")
    int32 ClientPort = 0;

    /** Empty if the DataManager hasn't received a PROJECT message yet
     *  (live mode with no ParaView source connected yet). */
    UPROPERTY(BlueprintReadOnly, Category = "PVLink")
    FString ProjectName;
};

/**
 * Background thread that listens for PVLink DataManager UDP broadcast
 * announcements and maintains a thread-safe, continuously-pruned list of
 * what's currently on the LAN.
 *
 * Wire format (plain text, UDP, separate from the TCP mesh-streaming
 * protocol -- see DataManager/datamanager.py's announce_loop):
 *   PVLINK-DISCOVERY 1
 *   client_port=9010
 *   project=Sphere
 *
 * A DataManager is identified by (source IP of the packet, client_port) --
 * it never needs to report its own IP. An entry is pruned if not
 * re-announced within StaleAfterSeconds (~3 missed broadcasts), so a
 * DataManager that exits eventually drops off the list on its own.
 *
 * This class only builds the discovered list and exposes it (see
 * USocketReceiverSubsystem::GetDiscoveredDataManagers) -- no UI reads it
 * yet; that's deferred to whatever picker/settings UI comes later.
 */
class FDataManagerDiscovery : public FRunnable
{
public:
    explicit FDataManagerDiscovery(int32 InDiscoveryPort);
    virtual ~FDataManagerDiscovery();

    virtual bool   Init()   override;
    virtual uint32 Run()    override;
    virtual void   Stop()   override;

    /** Thread-safe snapshot of currently-known DataManagers. */
    TArray<FDiscoveredDataManager> GetDiscovered() const;

private:
    int32    Port;
    FSocket* ListenerSocket = nullptr;
    TAtomic<bool> bShouldStop{false};

    struct FEntry
    {
        FString Host;
        int32   ClientPort = 0;
        FString ProjectName;
        double  LastSeenSeconds = 0.0;
    };

    static constexpr double StaleAfterSeconds = 6.0;   // ~3 missed broadcasts

    mutable FCriticalSection EntriesLock;
    TMap<FString, FEntry> Entries;   // key: "Host:ClientPort"

    void ParseAndRecord(const FString& Text, const FString& Host);
    void PruneStale();
};

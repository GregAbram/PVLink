#include "DataManagerDiscovery.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

FDataManagerDiscovery::FDataManagerDiscovery(int32 InDiscoveryPort)
    : Port(InDiscoveryPort)
{
}

FDataManagerDiscovery::~FDataManagerDiscovery()
{
    Stop();
}

bool FDataManagerDiscovery::Init()
{
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    ListenerSocket = SS->CreateSocket(NAME_DGram, TEXT("DataManagerDiscovery"), false);
    if (!ListenerSocket)
    {
        UE_LOG(LogTemp, Error, TEXT("DataManagerDiscovery: could not create UDP socket"));
        return false;
    }
    ListenerSocket->SetReuseAddr(true);

    TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
    Addr->SetAnyAddress();
    Addr->SetPort(Port);
    if (!ListenerSocket->Bind(*Addr))
    {
        UE_LOG(LogTemp, Error, TEXT("DataManagerDiscovery: bind failed on UDP port %d"), Port);
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("DataManagerDiscovery: listening for broadcasts on UDP port %d"), Port);
    return true;
}

uint32 FDataManagerDiscovery::Run()
{
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    while (!bShouldStop)
    {
        // Wait times out every 200ms so bShouldStop is checked ~5x/sec,
        // matching the poll cadence used elsewhere in this plugin
        // (SocketReceiverRunnable's WaitForPendingConnection, 100ms).
        if (ListenerSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200)))
        {
            uint8 Buffer[2048];
            int32 BytesRead = 0;
            TSharedRef<FInternetAddr> Sender = SS->CreateInternetAddr();
            if (ListenerSocket->RecvFrom(Buffer, sizeof(Buffer) - 1, BytesRead, *Sender) && BytesRead > 0)
            {
                Buffer[BytesRead] = 0;
                const FString Text = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buffer)));
                ParseAndRecord(Text, Sender->ToString(/*bAppendPort=*/false));
            }
        }
        PruneStale();
    }

    if (ListenerSocket)
    {
        SS->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
    }
    return 0;
}

void FDataManagerDiscovery::Stop()
{
    // Signal only -- Run() owns the socket for its entire lifetime, same
    // rationale as SocketReceiverRunnable::Stop().
    bShouldStop = true;
}

void FDataManagerDiscovery::ParseAndRecord(const FString& Text, const FString& Host)
{
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines);
    if (Lines.Num() == 0 || !Lines[0].StartsWith(TEXT("PVLINK-DISCOVERY")))
        return;   // not our protocol -- ignore silently, some other UDP traffic on this port

    int32 ClientPort = 0;
    FString ProjectName;
    for (int32 i = 1; i < Lines.Num(); i++)
    {
        FString Key, Value;
        if (Lines[i].Split(TEXT("="), &Key, &Value))
        {
            if (Key == TEXT("client_port"))     ClientPort = FCString::Atoi(*Value);
            else if (Key == TEXT("project"))    ProjectName = Value;
        }
    }
    if (ClientPort <= 0)
        return;

    const FString MapKey = FString::Printf(TEXT("%s:%d"), *Host, ClientPort);
    const double Now = FPlatformTime::Seconds();

    FScopeLock Lock(&EntriesLock);
    const FEntry* Existing = Entries.Find(MapKey);
    const bool bIsNew          = (Existing == nullptr);
    const bool bProjectChanged = Existing && Existing->ProjectName != ProjectName;
    // Existing is not dereferenced again after this point -- FindOrAdd below
    // may rehash the map and invalidate it.

    FEntry& E = Entries.FindOrAdd(MapKey);
    E.Host          = Host;
    E.ClientPort    = ClientPort;
    E.ProjectName   = ProjectName;
    E.LastSeenSeconds = Now;

    if (bIsNew || bProjectChanged)
    {
        UE_LOG(LogTemp, Verbose, TEXT("DataManagerDiscovery: %s -- project='%s'%s"),
            *MapKey, *ProjectName, bIsNew ? TEXT(" (new)") : TEXT(" (updated)"));
    }
}

void FDataManagerDiscovery::PruneStale()
{
    const double Now = FPlatformTime::Seconds();
    FScopeLock Lock(&EntriesLock);
    for (auto It = Entries.CreateIterator(); It; ++It)
    {
        if (Now - It->Value.LastSeenSeconds > StaleAfterSeconds)
        {
            UE_LOG(LogTemp, Verbose, TEXT("DataManagerDiscovery: %s -- pruned (stale)"), *It->Key);
            It.RemoveCurrent();
        }
    }
}

TArray<FDiscoveredDataManager> FDataManagerDiscovery::GetDiscovered() const
{
    TArray<FDiscoveredDataManager> Result;
    FScopeLock Lock(&EntriesLock);
    Result.Reserve(Entries.Num());
    for (const auto& Pair : Entries)
    {
        FDiscoveredDataManager D;
        D.Host        = Pair.Value.Host;
        D.ClientPort  = Pair.Value.ClientPort;
        D.ProjectName = Pair.Value.ProjectName;
        Result.Add(D);
    }
    return Result;
}

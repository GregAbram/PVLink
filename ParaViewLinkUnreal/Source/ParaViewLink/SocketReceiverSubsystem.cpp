#include "SocketReceiverSubsystem.h"
#include "SocketReceiverRunnable.h"
#include "SocketSenderRunnable.h"
#include "HAL/RunnableThread.h"
#include "UObject/UObjectIterator.h"

// ---------------------------------------------------------------------------

bool USocketReceiverSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    if (!Super::ShouldCreateSubsystem(Outer))
        return false;

    // If a more-derived subclass exists, let it handle everything instead.
    TArray<UClass*> ChildClasses;
    GetDerivedClasses(GetClass(), ChildClasses, /*bRecursive=*/true);
    return ChildClasses.Num() == 0;
}

void USocketReceiverSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    FString DataManagerHost;   // empty = unconfigured -> auto-connect via discovery
    int32   DataManagerPort = 9010;
    int32   OutboundPort    = 9002;
    int32   DiscoveryPort   = 9011;
    GConfig->GetString(TEXT("/Script/ParaViewLink.SocketReceiverSubsystem"), TEXT("DataManagerHost"), DataManagerHost, GGameIni);
    GConfig->GetInt(TEXT("/Script/ParaViewLink.SocketReceiverSubsystem"), TEXT("DataManagerPort"), DataManagerPort, GGameIni);
    GConfig->GetInt(TEXT("/Script/ParaViewLink.SocketReceiverSubsystem"), TEXT("OutboundPort"), OutboundPort, GGameIni);
    GConfig->GetInt(TEXT("/Script/ParaViewLink.SocketReceiverSubsystem"), TEXT("DiscoveryPort"), DiscoveryPort, GGameIni);

    Sender       = new FSocketSenderRunnable(OutboundPort);
    SenderThread = FRunnableThread::Create(Sender, TEXT("SocketSenderThread"));

    Discovery       = new FDataManagerDiscovery(DiscoveryPort);
    DiscoveryThread = FRunnableThread::Create(Discovery, TEXT("DataManagerDiscoveryThread"));

    if (DataManagerHost.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SocketReceiverSubsystem: no DataManagerHost configured -- waiting %.1fs for discovery to settle"),
            AutoConnectSettleSeconds);
        AutoConnectTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateUObject(this, &USocketReceiverSubsystem::TryAutoConnect),
            AutoConnectSettleSeconds);
    }
    else
    {
        StartReceiver(DataManagerHost, DataManagerPort);
    }

    UE_LOG(LogTemp, Warning, TEXT("SocketReceiverSubsystem: DataManager=%s  Outbound=%d  Discovery=%d"),
           DataManagerHost.IsEmpty() ? TEXT("(auto)") : *FString::Printf(TEXT("%s:%d"), *DataManagerHost, DataManagerPort),
           OutboundPort, DiscoveryPort);
}

void USocketReceiverSubsystem::Deinitialize()
{
    if (AutoConnectTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(AutoConnectTickerHandle);
        AutoConnectTickerHandle.Reset();
    }

    // Clear callback FIRST so the network thread stops calling into this subsystem.
    if (Receiver) Receiver->OnMessageReceived = nullptr;

    if (Receiver) Receiver->Stop();
    if (ReceiverThread) { ReceiverThread->WaitForCompletion(); delete ReceiverThread; ReceiverThread = nullptr; }
    if (Receiver)       { delete Receiver; Receiver = nullptr; }

    if (Sender) Sender->Stop();
    if (SenderThread) { SenderThread->WaitForCompletion(); delete SenderThread; SenderThread = nullptr; }
    if (Sender)       { delete Sender; Sender = nullptr; }

    if (Discovery) Discovery->Stop();
    if (DiscoveryThread) { DiscoveryThread->WaitForCompletion(); delete DiscoveryThread; DiscoveryThread = nullptr; }
    if (Discovery)       { delete Discovery; Discovery = nullptr; }

    UE_LOG(LogTemp, Warning, TEXT("SocketReceiverSubsystem: Stopped"));
    Super::Deinitialize();
}

void USocketReceiverSubsystem::StartReceiver(const FString& Host, int32 Port)
{
    if (Receiver) Receiver->OnMessageReceived = nullptr;
    if (Receiver) Receiver->Stop();
    if (ReceiverThread) { ReceiverThread->WaitForCompletion(); delete ReceiverThread; ReceiverThread = nullptr; }
    if (Receiver)       { delete Receiver; Receiver = nullptr; }

    Receiver = new FSocketReceiverRunnable(Host, Port);
    Receiver->OnMessageReceived = [this](int32 Cmd, TArray<uint8> Payload)
    {
        this->HandleRawMessage(Cmd, MoveTemp(Payload));
    };
    ReceiverThread = FRunnableThread::Create(Receiver, TEXT("SocketReceiverThread"));
}

TArray<FDiscoveredDataManager> USocketReceiverSubsystem::GetDiscoveredDataManagers() const
{
    return Discovery ? Discovery->GetDiscovered() : TArray<FDiscoveredDataManager>();
}

void USocketReceiverSubsystem::ConnectToDataManager(const FString& Host, int32 Port)
{
    UE_LOG(LogTemp, Warning, TEXT("SocketReceiverSubsystem: switching DataManager to %s:%d"), *Host, Port);
    StartReceiver(Host, Port);
}

bool USocketReceiverSubsystem::TryAutoConnect(float DeltaTime)
{
    TArray<FDiscoveredDataManager> Found = GetDiscoveredDataManagers();
    if (Found.Num() == 1)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SocketReceiverSubsystem: auto-connecting to the single discovered DataManager %s:%d"),
            *Found[0].Host, Found[0].ClientPort);
        StartReceiver(Found[0].Host, Found[0].ClientPort);
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SocketReceiverSubsystem: %d DataManager(s) found -- not auto-connecting (need exactly 1). ")
            TEXT("Call ConnectToDataManager() manually."),
            Found.Num());
    }
    AutoConnectTickerHandle.Reset();
    return false;   // one-shot -- unregister
}

bool USocketReceiverSubsystem::SendMessage(int32 Type, const TArray<uint8>& Payload)
{
    if (!Sender) return false;
    TArray<uint8> Copy = Payload;
    Sender->EnqueueMessage(Type, MoveTemp(Copy));
    return true;
}

bool USocketReceiverSubsystem::SendString(int32 Type, const FString& Text)
{
    FTCHARToUTF8 Converter(*Text);
    TArray<uint8> Payload(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
    return SendMessage(Type, Payload);
}

void USocketReceiverSubsystem::HandleRawMessage(int32 Cmd, TArray<uint8> Payload)
{
    // Default: log payload as a UTF-8 string.
    Payload.Add(0);
    FString Msg = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Payload.GetData()));
    UE_LOG(LogTemp, Warning, TEXT("SocketReceiverSubsystem: Cmd=%d  \"%s\""), Cmd, *Msg);
}

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

    int32 InboundPort  = 9001;
    int32 OutboundPort = 9002;
    GConfig->GetInt(TEXT("/Script/ParaViewLink.SocketReceiverSubsystem"), TEXT("InboundPort"),  InboundPort,  GGameIni);
    GConfig->GetInt(TEXT("/Script/ParaViewLink.SocketReceiverSubsystem"), TEXT("OutboundPort"), OutboundPort, GGameIni);

    Receiver = new FSocketReceiverRunnable(InboundPort);

    // Bind the callback — captures 'this', cleared in Deinitialize before thread stops.
    Receiver->OnMessageReceived = [this](int32 Cmd, TArray<uint8> Payload)
    {
        this->HandleRawMessage(Cmd, MoveTemp(Payload));
    };

    ReceiverThread = FRunnableThread::Create(Receiver, TEXT("SocketReceiverThread"));

    Sender       = new FSocketSenderRunnable(OutboundPort);
    SenderThread = FRunnableThread::Create(Sender, TEXT("SocketSenderThread"));

    UE_LOG(LogTemp, Warning, TEXT("SocketReceiverSubsystem: Inbound=%d  Outbound=%d"),
           InboundPort, OutboundPort);
}

void USocketReceiverSubsystem::Deinitialize()
{
    // Clear callback FIRST so the network thread stops calling into this subsystem.
    if (Receiver) Receiver->OnMessageReceived = nullptr;

    if (Receiver) Receiver->Stop();
    if (ReceiverThread) { ReceiverThread->WaitForCompletion(); delete ReceiverThread; ReceiverThread = nullptr; }
    if (Receiver)       { delete Receiver; Receiver = nullptr; }

    if (Sender) Sender->Stop();
    if (SenderThread) { SenderThread->WaitForCompletion(); delete SenderThread; SenderThread = nullptr; }
    if (Sender)       { delete Sender; Sender = nullptr; }

    UE_LOG(LogTemp, Warning, TEXT("SocketReceiverSubsystem: Stopped"));
    Super::Deinitialize();
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

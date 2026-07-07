#include "SocketReceiverRunnable.h"
#include "MeshTypes.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

FSocketReceiverRunnable::FSocketReceiverRunnable(int32 InPort)
    : Port(InPort)
{
}

FSocketReceiverRunnable::~FSocketReceiverRunnable()
{
    Stop();
}

bool FSocketReceiverRunnable::Init()
{
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    ListenerSocket = SS->CreateSocket(NAME_Stream, TEXT("ReceiverListener"), false);
    if (!ListenerSocket)
    {
        UE_LOG(LogTemp, Error, TEXT("SocketReceiver: Could not create socket"));
        return false;
    }

    TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
    Addr->SetAnyAddress();
    Addr->SetPort(Port);
    ListenerSocket->SetReuseAddr(true);

    if (!ListenerSocket->Bind(*Addr) || !ListenerSocket->Listen(1))
    {
        UE_LOG(LogTemp, Error, TEXT("SocketReceiver: Bind/listen failed on port %d"), Port);
        return false;
    }

    UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Listening on port %d"), Port);
    return true;
}

uint32 FSocketReceiverRunnable::Run()
{
    while (!bShouldStop)
    {
        // WaitForPendingConnection blocks for up to 100 ms then returns, so the
        // loop checks bShouldStop at least 10×/sec without any external socket
        // close being needed to interrupt it.
        bool bHasPending = false;
        if (!ListenerSocket->WaitForPendingConnection(bHasPending, FTimespan::FromMilliseconds(100)) || !bHasPending)
            continue;

        ConnectionSocket = ListenerSocket->Accept(TEXT("ReceiverClient"));
        if (!ConnectionSocket) continue;
        UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Client connected"));

        while (!bShouldStop)
        {
            uint8 Header[8];
            if (!ReadExact(ConnectionSocket, Header, 8))
            {
                UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Connection closed"));
                break;
            }

            int32 Count = *reinterpret_cast<int32*>(Header);
            int32 Type  = *reinterpret_cast<int32*>(Header + 4);

            if (Count < 0 || Count > 64 * 1024 * 1024)    // sanity: max 64 MB
            {
                UE_LOG(LogTemp, Error, TEXT("SocketReceiver: Bad count %d"), Count);
                SendAck(ConnectionSocket, 2);
                break;
            }

            TArray<uint8> Payload;
            Payload.SetNumUninitialized(Count);

            if (Count > 0 && !ReadExact(ConnectionSocket, Payload.GetData(), Count))
            {
                UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Lost connection reading payload"));
                break;
            }

            // For UPDATE: dispatch first (HandleRawMessage blocks until the game
            // thread completes the buffer swap), then send the ack so ParaView
            // knows the flip is done.  All other message types are fire-and-forget
            // — no ack is sent and the IO thread does not block.
            if (OnMessageReceived)
                OnMessageReceived(Type, MoveTemp(Payload));

            if (Type == MeshCmd::Update)
                SendAck(ConnectionSocket, 0);
        }

        ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        ConnectionSocket->Close();
        SS->DestroySocket(ConnectionSocket);
        ConnectionSocket = nullptr;
    }

    // Run() is the sole owner of the sockets — destroy them here, after the
    // loop exits, so there is never a race between Stop() and this thread.
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (ConnectionSocket) { ConnectionSocket->Close(); SS->DestroySocket(ConnectionSocket); ConnectionSocket = nullptr; }
    if (ListenerSocket)   { ListenerSocket->Close();   SS->DestroySocket(ListenerSocket);   ListenerSocket   = nullptr; }

    return 0;
}

void FSocketReceiverRunnable::Stop()
{
    // Signal only — do NOT touch the sockets here.
    //
    // Run() owns ListenerSocket / ConnectionSocket for its entire lifetime.
    // Closing or freeing them from this thread while Run() may be mid-call on
    // them is the exact race that causes EXC_BAD_ACCESS.
    //
    // WaitForPendingConnection() in Run() times out every 100 ms, so the
    // thread will see bShouldStop and exit cleanly on its own.  The caller
    // (Deinitialize) must call FRunnableThread::WaitForCompletion() before
    // deleting this object to guarantee Run() has finished and the sockets
    // are gone.
    bShouldStop = true;
}

bool FSocketReceiverRunnable::ReadExact(FSocket* Socket, uint8* Buffer, int32 NumBytes)
{
    int32 Total = 0;
    while (Total < NumBytes && !bShouldStop)
    {
        // HasPendingData is non-blocking.  If nothing is ready yet, sleep
        // briefly and loop — this lets bShouldStop be checked even when
        // Python is connected but has gone idle, preventing WaitForCompletion
        // from blocking until Python disconnects.
        uint32 Pending = 0;
        if (!Socket->HasPendingData(Pending) || Pending == 0)
        {
            FPlatformProcess::Sleep(0.005f);
            continue;
        }

        int32 Read = 0;
        if (!Socket->Recv(Buffer + Total, NumBytes - Total, Read)) return false;
        Total += Read;
    }
    return Total == NumBytes;
}

bool FSocketReceiverRunnable::SendAck(FSocket* Socket, int32 Status)
{
    int32 Sent = 0;
    return Socket->Send(reinterpret_cast<uint8*>(&Status), 4, Sent) && Sent == 4;
}

#include "SocketReceiverRunnable.h"
#include "MeshTypes.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

FSocketReceiverRunnable::FSocketReceiverRunnable(const FString& InHost, int32 InPort)
    : Host(InHost)
    , Port(InPort)
{
}

FSocketReceiverRunnable::~FSocketReceiverRunnable()
{
    Stop();
}

bool FSocketReceiverRunnable::Init()
{
    // Connection is established (and re-established) in Run() -- nothing to
    // do here, unlike the old listen/bind setup this replaced.
    return true;
}

uint32 FSocketReceiverRunnable::Run()
{
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    while (!bShouldStop)
    {
        TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
        bool bValidIP = false;
        Addr->SetIp(*Host, bValidIP);
        Addr->SetPort(Port);

        if (bValidIP)
        {
            ConnectionSocket = SS->CreateSocket(NAME_Stream, TEXT("DataManagerConnection"), false);
            // NOTE: Connect() on a genuinely unreachable (not merely refused)
            // host can block for the OS-level TCP connect timeout -- tens of
            // seconds -- before bShouldStop is checked again. Acceptable for
            // a LAN/localhost DataManager; not worth a non-blocking-connect
            // rewrite for this use case.
            if (ConnectionSocket && ConnectionSocket->Connect(*Addr))
            {
                UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Connected to DataManager %s:%d"), *Host, Port);
                ReadMessageLoop(ConnectionSocket);
                UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Disconnected from DataManager"));
            }

            if (ConnectionSocket)
            {
                ConnectionSocket->Close();
                SS->DestroySocket(ConnectionSocket);
                ConnectionSocket = nullptr;
            }
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("SocketReceiver: invalid DataManager host '%s'"), *Host);
        }

        // Wait before retrying, polling bShouldStop every 50ms so Stop()
        // is observed promptly rather than after a long blind sleep.
        for (int32 i = 0; i < 20 && !bShouldStop; i++)
            FPlatformProcess::Sleep(0.05f);
    }

    return 0;
}

void FSocketReceiverRunnable::ReadMessageLoop(FSocket* Socket)
{
    while (!bShouldStop)
    {
        uint8 Header[8];
        if (!ReadExact(Socket, Header, 8))
        {
            UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Connection closed"));
            break;
        }

        int32 Count = *reinterpret_cast<int32*>(Header);
        int32 Type  = *reinterpret_cast<int32*>(Header + 4);

        if (Count < 0 || Count > 64 * 1024 * 1024)    // sanity: max 64 MB
        {
            UE_LOG(LogTemp, Error, TEXT("SocketReceiver: Bad count %d"), Count);
            SendAck(Socket, 2);
            break;
        }

        TArray<uint8> Payload;
        Payload.SetNumUninitialized(Count);

        if (Count > 0 && !ReadExact(Socket, Payload.GetData(), Count))
        {
            UE_LOG(LogTemp, Warning, TEXT("SocketReceiver: Lost connection reading payload"));
            break;
        }

        // For UPDATE: dispatch first (HandleRawMessage blocks until the game
        // thread completes the buffer swap), then send the ack so the
        // DataManager knows the flip is done.  All other message types are
        // fire-and-forget -- no ack is sent and the IO thread does not block.
        if (OnMessageReceived)
            OnMessageReceived(Type, MoveTemp(Payload));

        if (Type == MeshCmd::Update)
            SendAck(Socket, 0);
    }
}

void FSocketReceiverRunnable::Stop()
{
    // Signal only -- do NOT touch the socket here.
    //
    // Run() owns ConnectionSocket for its entire lifetime. Closing or
    // freeing it from this thread while Run() may be mid-call on it is the
    // exact race that causes EXC_BAD_ACCESS.
    //
    // Both the retry-wait loop and ReadExact's poll check bShouldStop at
    // least every 50ms, so the thread will see this and exit cleanly on its
    // own. The caller (Deinitialize) must call
    // FRunnableThread::WaitForCompletion() before deleting this object to
    // guarantee Run() has finished and the socket is gone.
    bShouldStop = true;
}

bool FSocketReceiverRunnable::ReadExact(FSocket* Socket, uint8* Buffer, int32 NumBytes)
{
    int32 Total = 0;
    while (Total < NumBytes && !bShouldStop)
    {
        // HasPendingData is non-blocking.  If nothing is ready yet, sleep
        // briefly and loop -- this lets bShouldStop be checked even when
        // the DataManager is connected but has gone idle, preventing
        // WaitForCompletion from blocking until it disconnects.
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

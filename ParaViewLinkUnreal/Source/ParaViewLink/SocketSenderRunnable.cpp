#include "SocketSenderRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

FSocketSenderRunnable::FSocketSenderRunnable(int32 InPort)
    : Port(InPort)
{
}

FSocketSenderRunnable::~FSocketSenderRunnable()
{
    Stop();
}

bool FSocketSenderRunnable::Init()
{
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

    ListenerSocket = SS->CreateSocket(NAME_Stream, TEXT("SenderListener"), false);
    if (!ListenerSocket)
    {
        UE_LOG(LogTemp, Error, TEXT("SocketSender: Could not create socket"));
        return false;
    }

    TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
    Addr->SetAnyAddress();
    Addr->SetPort(Port);
    ListenerSocket->SetReuseAddr(true);

    if (!ListenerSocket->Bind(*Addr) || !ListenerSocket->Listen(1))
    {
        UE_LOG(LogTemp, Error, TEXT("SocketSender: Bind/listen failed on port %d"), Port);
        return false;
    }

    UE_LOG(LogTemp, Warning, TEXT("SocketSender: Listening on port %d"), Port);
    return true;
}

uint32 FSocketSenderRunnable::Run()
{
    while (!bShouldStop)
    {
        bool bHasPending = false;
        if (!ListenerSocket->WaitForPendingConnection(bHasPending, FTimespan::FromMilliseconds(100)) || !bHasPending)
            continue;

        ConnectionSocket = ListenerSocket->Accept(TEXT("SenderClient"));
        if (!ConnectionSocket) continue;

        UE_LOG(LogTemp, Warning, TEXT("SocketSender: Client connected"));

        bool bConnected = true;
        while (!bShouldStop && bConnected)
        {
            FOutgoingMessage Msg;
            if (OutgoingQueue.Dequeue(Msg))
            {
                if (!SendFramed(ConnectionSocket, Msg.Type, Msg.Payload))
                {
                    UE_LOG(LogTemp, Warning, TEXT("SocketSender: Lost connection sending message"));
                    bConnected = false;
                    break;
                }

                int32 AckStatus = 0;
                if (!ReadAck(ConnectionSocket, AckStatus))
                {
                    UE_LOG(LogTemp, Warning, TEXT("SocketSender: Lost connection waiting for ack"));
                    bConnected = false;
                    break;
                }

                UE_LOG(LogTemp, Warning, TEXT("SocketSender: OUTBOUND Type=%d  ack=%d"), Msg.Type, AckStatus);
            }
            else
            {
                FPlatformProcess::Sleep(0.001f);
            }
        }

        ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        ConnectionSocket->Close();
        SS->DestroySocket(ConnectionSocket);
        ConnectionSocket = nullptr;

        UE_LOG(LogTemp, Warning, TEXT("SocketSender: Connection closed, waiting for next client"));
    }

    // Run() is the sole owner of the sockets — destroy them here after the loop
    // exits so there is no race with Stop().
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (ConnectionSocket) { ConnectionSocket->Close(); SS->DestroySocket(ConnectionSocket); ConnectionSocket = nullptr; }
    if (ListenerSocket)   { ListenerSocket->Close();   SS->DestroySocket(ListenerSocket);   ListenerSocket   = nullptr; }

    return 0;
}

void FSocketSenderRunnable::Stop()
{
    // Signal only — see SocketReceiverRunnable::Stop() for the full rationale.
    // Run() owns the sockets and will clean them up after the loop exits.
    // Callers must WaitForCompletion() before deleting this object.
    bShouldStop = true;
}

void FSocketSenderRunnable::EnqueueMessage(int32 Type, TArray<uint8> Payload)
{
    FOutgoingMessage Msg;
    Msg.Type    = Type;
    Msg.Payload = MoveTemp(Payload);
    OutgoingQueue.Enqueue(MoveTemp(Msg));
}

bool FSocketSenderRunnable::ReadExact(FSocket* Socket, uint8* Buffer, int32 NumBytes)
{
    int32 TotalRead = 0;
    while (TotalRead < NumBytes && !bShouldStop)
    {
        uint32 Pending = 0;
        if (!Socket->HasPendingData(Pending) || Pending == 0)
        {
            FPlatformProcess::Sleep(0.005f);
            continue;
        }

        int32 BytesRead = 0;
        if (!Socket->Recv(Buffer + TotalRead, NumBytes - TotalRead, BytesRead))
            return false;
        TotalRead += BytesRead;
    }
    return TotalRead == NumBytes;
}

bool FSocketSenderRunnable::SendFramed(FSocket* Socket, int32 Type, const TArray<uint8>& Payload)
{
    uint8 Header[8];
    int32 Count = Payload.Num();
    FMemory::Memcpy(Header,     &Count, 4);
    FMemory::Memcpy(Header + 4, &Type,  4);

    int32 Sent = 0;
    if (!Socket->Send(Header, 8, Sent) || Sent != 8)
        return false;

    if (Count > 0)
    {
        Sent = 0;
        if (!Socket->Send(Payload.GetData(), Count, Sent) || Sent != Count)
            return false;
    }
    return true;
}

bool FSocketSenderRunnable::ReadAck(FSocket* Socket, int32& OutStatus)
{
    uint8 Buf[4];
    if (!ReadExact(Socket, Buf, 4))
        return false;
    OutStatus = *reinterpret_cast<int32*>(Buf);
    return true;
}

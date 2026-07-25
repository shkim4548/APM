#pragma once
#include "NetAddress.h"
#include "RecvBuffer.h"

class Service;

/*-------------
    Session
---------------*/

class Session : public enable_shared_from_this<Session>
{
    friend class Listener;
    friend class Service;

    enum {
        BUFFER_SIZE = 0x10000
    };

public:
    Session();
    virtual ~Session();

public:
    void                    Send(SendBufferRef sendBuffer);
    bool                    Connect();
    void                    Disconnect(const string& cause);

    shared_ptr<Service>     GetService() {
        return _service.lock();
    }
    void                    SetService(shared_ptr<Service> service) {
        _service = service;
    }

public:
    void                            SetNetAddress(NetAddress address) {
        _netAddress = address;
    }
    bool                            IsConnected() {
        return _connected;
    }
    SessionRef                      GetSessionRef() {
        return static_pointer_cast<Session>(shared_from_this());
    }
    asio::ip::tcp::socket& GetSocket() {
        return _socket;
    }

private:
    bool                RegisterConnect();
    void                RegisterRecv();
    void                RegisterSend();

    void                ProcessConnect();
    void                ProcessDisconnect();
    void                ProcessRecv(int32 numOfBytes);
    void                ProcessSend(int32 numOfBytes);

    void                HandleError(const asio::error_code& ec);

protected:
    /* ÄÁÅÙÃ÷ ÄÚµå¿¡¼­ ÀçÁ¤ÀÇ */
    virtual void        OnConnected() {
    }
    virtual int32       OnRecv(BYTE* buffer, int32 len) {
        return len;
    }
    virtual void        OnSend(int32 len) {
    }
    virtual void        OnDisconnected() {
    }

private:
    weak_ptr<Service>           _service;
    asio::ip::tcp::socket       _socket;
    NetAddress                  _netAddress = {};
    Atomic<bool>                _connected = false;

    USE_LOCK;
    RecvBuffer                  _recvBuffer;
    Queue<SendBufferRef>        _sendQueue;
    atomic<bool>                _sendRegistered = false;
    vector<SendBufferRef>       _sendingBuffers;
};

/*-----------------
    PacketSession
------------------*/

struct PacketHeader
{
    uint16 size;
    uint16 id;
};

class PacketSession : public Session
{
public:
    PacketSession();
    virtual ~PacketSession();

    PacketSessionRef    GetPacketSessionRef() {
        return static_pointer_cast<PacketSession>(shared_from_this());
    }

protected:
    virtual int32       OnRecv(BYTE* buffer, int32 len) final;
    virtual void        OnRecvPacket(BYTE* buffer, int32 len) = 0;
};
#ifndef __NET_H__
#define __NET_H__

#include <WinSock2.h>

#include <Windows.h>

#include <cassert>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <new>


#pragma comment(lib,"ws2_32.lib")
#define MAX_PACKET_SZ 16*1024
typedef unsigned short packet_header_t;

#define PACKET_HEAD_SZ sizeof(packet_header_t)

struct PER_IO_CONTEXT {
    OVERLAPPED _overlapped;
    enum opCode {OP_CONNECT, OP_RECV, OP_SEND};
    opCode _op;

    WSABUF _wsaBuf;

    PER_IO_CONTEXT(opCode op):_op(op) {
        ::ZeroMemory(&_overlapped, sizeof(_overlapped));
    }
};

class NetBuf {
    char *_buf;
    int _readOffset, _writeOffset;
    int _bufSize;
    bool _isLocked;

public:
    NetBuf():_buf(NULL),_readOffset(0),_writeOffset(0),_bufSize(0),_isLocked(false) {}
    
    ~NetBuf()  { 
        if (_buf) 
            delete[] _buf; 
    }

    bool init()
    {
        _bufSize = 16 * MAX_PACKET_SZ;
        _buf = new (std::nothrow) char[_bufSize];
        if (!_buf) return false;
        _readOffset = _writeOffset = 0;
        _isLocked = false;
        return true;
    }

    int leftBufSize()
    {
        return _bufSize - _writeOffset + _readOffset;
    }

    char *reserve(int len)
    {
        if (leftBufSize() < len) 
            return NULL;
        defrag(len);
        if (_bufSize - _writeOffset >= len) 
            return _buf + _writeOffset;
        return NULL;
    }

    void commit(int len)
    {
        _writeOffset += len;
        assert(_writeOffset <= _bufSize);
    }

    char *get(int len)
    {
        if (len > (_writeOffset - _readOffset)) return NULL;
        return _buf + _readOffset;
    }

    void free(int len)
    {
        _isLocked = false;
        _readOffset += len;
        assert(_readOffset <= _writeOffset);
    }

    void defrag(int threshold)
    {
        if (!_isLocked && _bufSize - _writeOffset < threshold) {
            memmove(_buf, _buf + _readOffset, _writeOffset - _readOffset);
            _writeOffset = _writeOffset - _readOffset;
            _readOffset = 0;
        }
    }

    char *lock()
    {
        _isLocked = true;
        return _buf + _readOffset;
    }

    bool isLocked()
    {
        return _isLocked;
    }

    int len()
    {
        return _writeOffset - _readOffset;
    }
};


class Locker {
    CRITICAL_SECTION m_cs;
public:
    Locker(CRITICAL_SECTION cs):m_cs(cs)
    {
        ::EnterCriticalSection(&m_cs);
    }
    ~Locker()
    {
        ::LeaveCriticalSection(&m_cs);
    }
        
};

class PacketQueue {

    NetBuf _netBuf;
    CRITICAL_SECTION m_cs;

public:
    bool init() {
        ::InitializeCriticalSection(&m_cs);
        return _netBuf.init();
    }

    bool push(char *fullPacket, int fullLen)
    {
        Locker locker(m_cs);
        char *buf = _netBuf.reserve(fullLen);
        if (!buf) return false;
        _netBuf.commit(fullLen);
        memcpy(buf, fullPacket, fullLen);
        return true;
    }

    bool top(char **pPacket, int *pPacketSize)
    {
        Locker locker(m_cs);
        char *header = _netBuf.get(PACKET_HEAD_SZ);
        if (!header) return false;
        *pPacketSize = *(packet_header_t *)header;
        char *fullPacket = _netBuf.get(*pPacketSize + PACKET_HEAD_SZ);
        assert(fullPacket);
        *pPacket = fullPacket + PACKET_HEAD_SZ;
        return true;
    }

    void pop(int packetSize = 0)
    {
        Locker locker(m_cs);
        if (packetSize == 0) {
            char *header = _netBuf.get(PACKET_HEAD_SZ);
            if (!header) return;
            packetSize = *(packet_header_t *)header;
        }
        _netBuf.free(packetSize + PACKET_HEAD_SZ);
    }
};

class Net {

   // enum { kConcurrentVal = 2 };
    HANDLE _hIOCP;
    HANDLE _hThread;
    
    NetBuf _readBuf;
    PER_IO_CONTEXT *_readContext;

    PacketQueue _packetQueue;
    SOCKET _sock;
    bool _closed;
    bool _connected;

    NetBuf _writeBuf;
    CRITICAL_SECTION _WBCS;
    PER_IO_CONTEXT *_sendContext;

public:
    bool start(const char *ip, int port);
    void stop();

    bool sendPacket(char *packet, int packetSize);

    bool readPacketQueue_top(char **pPacket, int *pPacketSize);
    void readPacketQueue_pop(int packetSize = 0);

    void workRoutine();

private:
    bool postRecv(SOCKET s, PER_IO_CONTEXT *context);
    bool postSend(SOCKET s, PER_IO_CONTEXT *context);
};
#endif
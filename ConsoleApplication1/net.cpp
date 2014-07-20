#include <process.h>



#include "net.h"
#include <MSWSock.h>
#include <algorithm>


#define IO_FAILED_TRY_TIMEs 8
#define MAX_BYTES_PER_SEND 16 * 1024

#define ERR_RET(fmt, ...) do {          \
    fprintf(stderr, fmt, __VA_ARGS__);  \
    return false;                       \
    } while (0)



void work_routine(void *arg)
{
    Net *net = (Net *)arg;
    net->workRoutine();
}

void Net::workRoutine()
{
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED pOverlapped;

    while (true) {
        BOOL ok = ::GetQueuedCompletionStatus(_hIOCP, &bytesTransferred, &completionKey, &pOverlapped, INFINITE);
        if (!ok) { // ERROR EXIT
            fprintf(stderr, "GetQueuedCompletionStatus failed error code = %d", ::GetLastError());
            break;
        }
        if (completionKey == INVALID_SOCKET) { // notified EXIT
            break;
        }

        /////////////////////////////////////////////////////////

        PER_IO_CONTEXT *context = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, _overlapped);
        if (bytesTransferred == 0 && 
            ( context->_op == PER_IO_CONTEXT::OP_RECV)) {// disconnect EXIT
                break; 
        }

        switch (context->_op) {
            case PER_IO_CONTEXT::OP_CONNECT:
                _connected = true;
                postRecv(completionKey, context); 

                _sendContext->_wsaBuf.len = 0;
                ::EnterCriticalSection(&_WBCS);
                if (!_writeBuf.isLocked() && _writeBuf.len() > 0) {
                    _sendContext->_wsaBuf.buf = _writeBuf.lock();
                    _sendContext->_wsaBuf.len = std::min<int>(_writeBuf.len(), MAX_BYTES_PER_SEND);
                }
                ::LeaveCriticalSection(&_WBCS);
                if (_sendContext->_wsaBuf.len > 0)
                    postSend(completionKey, _sendContext);
                break;
            case PER_IO_CONTEXT::OP_RECV:
                _readBuf.commit(bytesTransferred);

                while (true) {
                    char *head = _readBuf.get(PACKET_HEAD_SZ);
                    if (!head) break;
                    int packetSize = *(packet_header_t *)head;
                    char *fullPacket = _readBuf.get(PACKET_HEAD_SZ + packetSize);
                    if (!fullPacket) break;
                    if (!_packetQueue.push(fullPacket, packetSize + PACKET_HEAD_SZ))
                        break;
                    _readBuf.free(packetSize + PACKET_HEAD_SZ);
                }

                postRecv(completionKey, context);
                break;
            case PER_IO_CONTEXT::OP_SEND:
                ::EnterCriticalSection(&_WBCS);
                _writeBuf.free(bytesTransferred);
                _writeBuf.defrag(MAX_PACKET_SZ);

                context->_wsaBuf.len = 0;
                if (_writeBuf.len() > 0) {
                    context->_wsaBuf.buf = _writeBuf.lock();
                    context->_wsaBuf.len = std::min<int>(_writeBuf.len(), MAX_BYTES_PER_SEND);
                }
                ::LeaveCriticalSection(&_WBCS);

                if (context->_wsaBuf.len > 0)
                    postSend(completionKey, context);
                break;
            default:
                fprintf(stderr, "NOT FOUND OPCODE");
                break;
        }
    }
    _closed = true;
}

bool Net::postRecv(SOCKET s, PER_IO_CONTEXT *context)
{
    context->_wsaBuf.buf = _readBuf.reserve(MAX_PACKET_SZ);   // 因为单线程顺序执行，reserver的内存不会被破坏
    if (!context->_wsaBuf.buf)
        return false;
    context->_wsaBuf.len = MAX_PACKET_SZ;
    ::ZeroMemory(&context->_overlapped, sizeof(OVERLAPPED));
    context->_op = PER_IO_CONTEXT::OP_RECV;
    DWORD bytesRecvd = 0;
    DWORD flags = 0;

    for (int i = 0; i < IO_FAILED_TRY_TIMEs; i++) {
        int ret = ::WSARecv(s, &context->_wsaBuf, 1, &bytesRecvd, &flags, &(context->_overlapped), NULL);
        if (ret == SOCKET_ERROR && (::WSAGetLastError() != ERROR_IO_PENDING)) {
            fprintf(stderr, "WSARecv failed error code = %d", ::WSAGetLastError());
            ::Sleep(50 + i * 50);
        } else
            return true;
    }
    return false;
}

bool Net::postSend(SOCKET s, PER_IO_CONTEXT *context)
{
    assert(context->_wsaBuf.len > 0);
      
    ::ZeroMemory(&context->_overlapped, sizeof(OVERLAPPED));
    context->_op = PER_IO_CONTEXT::OP_SEND;

    for (int i = 0; i < IO_FAILED_TRY_TIMEs; i++) {
        int ret = ::WSASend(s, &(context->_wsaBuf), 1, NULL, 0, &(context->_overlapped), NULL);
        if (ret == SOCKET_ERROR && (::WSAGetLastError() != ERROR_IO_PENDING)) {
            fprintf(stderr, "WSASend failed error code = %d", ::WSAGetLastError());
            ::Sleep(50 + i * 50);
        } else
            return true;
    }
    return false;
}

bool Net::start(const char *ip, int port)
{
    WSAData wsaData;
    if (::WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
        ERR_RET("WSAStartup failed error code = %d", ::WSAGetLastError());
    _closed = _connected = false;
    if (!_readBuf.init())
        return false;
    if (!_writeBuf.init())
        return false;
    if (!_packetQueue.init())
        return false;
    ::InitializeCriticalSection(&_WBCS);

    _hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (_hIOCP == NULL)
        ERR_RET("CreateIoCompletionPort failed error code = %d", ::GetLastError());

    SOCKET sock = ::WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    _sock = sock;
    if (sock == INVALID_SOCKET)
        ERR_RET("WSASocket failed error code = %d", ::WSAGetLastError());
    if (NULL == ::CreateIoCompletionPort((HANDLE)sock, _hIOCP, (ULONG_PTR)sock, 0))
        ERR_RET("sock bind IOCP failed error code = %d", ::GetLastError());

    struct sockaddr_in addr;
    ::ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (::bind(sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        ERR_RET("bind failed error code = %d", ::GetLastError());

    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX ConnectEx;
    DWORD bytes;
    if (::WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, 
        sizeof(guid), &ConnectEx, sizeof(ConnectEx), &bytes, NULL, NULL) != 0)
        ERR_RET("WSAIoctl failed error code = %d", ::WSAGetLastError());

    struct sockaddr_in srv_addr;
    ::ZeroMemory(&srv_addr, sizeof(srv_addr));
    srv_addr.sin_addr.s_addr = inet_addr(ip);
    srv_addr.sin_port = htons(port);
    srv_addr.sin_family = AF_INET;

    PER_IO_CONTEXT *_readContext = new PER_IO_CONTEXT(PER_IO_CONTEXT::OP_CONNECT);
    BOOL ok = ConnectEx(sock, (SOCKADDR *)&srv_addr, sizeof(srv_addr), NULL, 0, NULL, (LPOVERLAPPED)&(_readContext->_overlapped));
    if (!ok && (WSAGetLastError() != ERROR_IO_PENDING))
       ERR_RET("ConnectEx failed error code = %d", ::WSAGetLastError());

    _hThread = (HANDLE)::_beginthread(work_routine, 0, this);

    _sendContext = new (std::nothrow) PER_IO_CONTEXT(PER_IO_CONTEXT::OP_SEND);
    return true;
}

bool Net::sendPacket(char *packet, int packetSize)
{
    if (_closed) 
        return false;
    int fullPacketSize = packetSize + PACKET_HEAD_SZ;
    PER_IO_CONTEXT *context = NULL;
    ::EnterCriticalSection(&_WBCS);
    char *fullPacket = _writeBuf.reserve(fullPacketSize);
    _writeBuf.commit(fullPacketSize);
    if (!_writeBuf.isLocked() && _connected) {
        context = _sendContext;
        context->_wsaBuf.buf = _writeBuf.lock();
        context->_wsaBuf.len = std::min<int>(_writeBuf.len(), MAX_BYTES_PER_SEND);
    }
    ::LeaveCriticalSection(&_WBCS);

    if (!fullPacket)
        return false;   // TODO: send buf is OOM
    *(packet_header_t *)(packet - PACKET_HEAD_SZ) = packetSize;
    memcpy(fullPacket, packet - PACKET_HEAD_SZ, fullPacketSize);

    if (context) {
        postSend(_sock, context);
    }
    return true;
}

void Net::stop()
{
    ::PostQueuedCompletionStatus(_hIOCP, 0, INVALID_SOCKET, NULL);
    DWORD waitRet = ::WaitForSingleObject(_hThread, 2);
    if (waitRet != WAIT_OBJECT_0) {
        fprintf(stderr, "wait thread exit failed error code = %d", ::GetLastError());
    }

    ::CloseHandle(_hIOCP);
    ::closesocket(_sock);

    ::WSACleanup();
}

void Net::readPacketQueue_pop(int packetSize)
{
    _packetQueue.pop(packetSize);
}

bool Net::readPacketQueue_top(char **pPacket, int *pPacketSize)
{
    return _packetQueue.top(pPacket, pPacketSize);
}
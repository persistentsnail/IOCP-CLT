#include "net.h"

#include <Windows.h>
#include <process.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <vector>


#define ERR_EXIT(fmt, ...)  do {        \
    fprintf(stderr, fmt, __VA_ARGS__);  \
    exit(1);                            \
    } while(0);


using namespace std;

#define SRV_IP "192.168.56.101"
#define SRV_PORT 40713

char protoBuf[8 * 1024];


int main(int argc, char *argv[])
{
    Net net;
    if (!net.start(SRV_IP, SRV_PORT))
        return 1;

    const char *msg = "hello world!!!";
    memcpy(protoBuf + PACKET_HEAD_SZ, msg, strlen(msg) + 1); 
    for (int i = 0; i < 5; i++) {
        assert(net.sendPacket(protoBuf + PACKET_HEAD_SZ, strlen(msg) + 1));
        char *packet;
        int packetSize;
        while (net.readPacketQueue_top(&packet, &packetSize)) {
            printf("received msg : %s \n", packet);
            net.readPacketQueue_pop(packetSize);
        }
        ::Sleep(1000);  
    }
    net.stop();
    return 0;
}

/*
vector<DWORD> g_whoWorking;

void worker_routine(void *arg)
{
    HANDLE hIOCP = (HANDLE)arg;
    DWORD dwBytesTransfered;
    ULONG_PTR ulCompletionKey;
    OVERLAPPED *pOverlapped;
    DWORD dwThreadId = GetCurrentThreadId();
    Sleep(2000);
    printf("------- start thread %d\n", dwThreadId);
    while (true) {
        BOOL bRet = ::GetQueuedCompletionStatus(hIOCP, &dwBytesTransfered, &ulCompletionKey, &pOverlapped, INFINITE);
        if (!bRet)
            ERR_EXIT("GetQueuedCompletionStatus failed error code = %d", GetLastError());
        g_whoWorking.push_back(dwThreadId);
    }
}

int main(int argc, char *argv[])
{
    HANDLE hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 1);
    if (hIOCP == NULL)
        ERR_EXIT("CreateIoCompletionPort failed error code = %d", GetLastError());
    HANDLE hThreads[3];
    for (int i = 0; i < sizeof(hThreads)/sizeof(hThreads[0]); i++) {
        hThreads[i] = (HANDLE)_beginthread(worker_routine, 0, hIOCP);
    }
    
    while (true) {
        for (int i = 0; i < 3; i++)
            ::PostQueuedCompletionStatus(hIOCP, 0, NULL, NULL);
        ::Sleep(1000);
        for (size_t i = 0; i < g_whoWorking.size(); i++) {
            printf("thread %d working ... \n", g_whoWorking[i]);
        }
        g_whoWorking.clear();
        printf("-----------------------------------\n");
    }
    return 0;
}
*/


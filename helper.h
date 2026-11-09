#pragma once

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int IO_BUFFER_SIZE = 64 * 1024;
const int CHECKSUM_SIZE = 4;
const char STATUS_OK = 0;
const char STATUS_ERROR = 1;

struct Variable {
    string name;
    int32_t value;
};

struct Frame {
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot {
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader {
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct FuncEntry {
    string funcName;
    int64_t byteOffsetInResolveBin;
};

struct PendingPatch {
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

// ---- CRC-32 checksum ----
uint32_t crcTable[256];

void initCrcTable(){
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            if (c & 1){
                c = 0xEDB88320 ^ (c >> 1);
            }
            else {
                c = c >> 1;
            }
        }
        crcTable[i] = c;
    }
}

// starting withcrc = 0xFFFFFFFF
//update with every chunk, at the end crc ^ 0xFFFFFFFF

uint32_t crcUpdate(uint32_t crc, const char* data, int n) {
    for (int i = 0; i < n; i++) {
        crc = crcTable[(crc ^ (unsigned char)data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

void crcToBytes(uint32_t crc, char out[4]) {
    out[0] = crc & 0xFF;
    out[1] = (crc >> 8) & 0xFF;
    out[2] = (crc >> 16) & 0xFF;
    out[3] = (crc >> 24) & 0xFF;
}

class TrailerChecker {
    char held[CHECKSUM_SIZE];
    int heldCount;
    uint32_t crc;
    int64_t bodyCount;

public:
    TrailerChecker() {
        heldCount = 0;
        crc = 0xFFFFFFFF;
        bodyCount = 0;
    }
    int feed(const char* in, int n, char* out) {
        for (int i = 0; i < heldCount; i++) {
            out[i] = held[i];
        }
        for (int i = 0; i < n; i++) {
            out[heldCount + i] = in[i];
        }
        int total = heldCount + n;

        if (total <= CHECKSUM_SIZE) {
            for (int i = 0; i < total; i++) {
                held[i] = out[i];
            }
            heldCount = total;
            return 0;
        }

        int release = total - CHECKSUM_SIZE;
        for (int i = 0; i < CHECKSUM_SIZE; i++) {
            held[i] = out[release + i];
        }
        heldCount = CHECKSUM_SIZE;
        crc = crcUpdate(crc, out, release);
        bodyCount = bodyCount + release;
        return release;
    }
    bool verify() {
        if (heldCount != CHECKSUM_SIZE || bodyCount == 0) {
            return false;
        }
        char expected[CHECKSUM_SIZE];
        crcToBytes(crc ^ 0xFFFFFFFF, expected);
        for (int i = 0; i < CHECKSUM_SIZE; i++) {
            if (expected[i] != held[i]) {
                return false;
            }
        }
        return true;
    }
    char* trailer() {
        return held;
    }
    int64_t bodySize() {
        return bodyCount;
    }
};

// socket helper funcs
bool sendAll(int sock, const char* buf, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        sent = sent + r;
    }
    return true;
}

bool recvAll(int sock, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(sock, buf + got, n - got, 0);
        if (r <= 0) {
            return false;
        }
        got = got + r;
    }
    return true;
}

// format: [status][text][checksum]
bool sendMessage(int sock, char status, const string& text) {
    uint32_t crc = crcUpdate(0xFFFFFFFF, &status, 1);
    crc = crcUpdate(crc, text.c_str(), text.size());
    char trailer[CHECKSUM_SIZE];
    crcToBytes(crc ^ 0xFFFFFFFF, trailer);

    if (!sendAll(sock, &status, 1)) {
        return false;
    }
    if (!sendAll(sock, text.c_str(), text.size())) {
        return false;
    }
    return sendAll(sock, trailer, CHECKSUM_SIZE);
}

// format:[status][file contents][checksum]
bool sendFileMessage(int sock, char status, const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint32_t crc = crcUpdate(0xFFFFFFFF, &status, 1);
    if (!sendAll(sock, &status, 1)) {
        fclose(f);
        return false;
    }

    char* buf = new char[IO_BUFFER_SIZE];
    bool ok = true;
    while (true) {
        int n = fread(buf, 1, IO_BUFFER_SIZE, f);
        if (n == 0) {
            break;
        }
        crc = crcUpdate(crc, buf, n);
        if (!sendAll(sock, buf, n)) {
            ok = false;
            break;
        }
    }
    delete[] buf;
    fclose(f);
    if (!ok) {
        return false;
    }

    char trailer[CHECKSUM_SIZE];
    crcToBytes(crc ^ 0xFFFFFFFF, trailer);
    return sendAll(sock, trailer, CHECKSUM_SIZE);
}

int createListenSocket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(sock, 16) < 0) {
        perror("listen");
        exit(1);
    }
    return sock;
}

void setRecvTimeout(int sock, int seconds) {
    timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


int connectTo(const char* host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        return -1;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}


// read and throw away what the other side still sends before closing so nothing needed is lost
void closeAfterReply(int sock, int seconds) {
    shutdown(sock, SHUT_WR);
    setRecvTimeout(sock, seconds);
    char buf[4096];
    while (recv(sock, buf, sizeof(buf), 0) > 0) {
    }
    close(sock);
}





// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <fcntl.h>
#include <sstream> // for runCommand
#include <cerrno>    // for
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;
const int HEADER_SIZE = 20;     // 4 magic + 4 version + 4 stepCount + 8 indexOffset
const int MAX_NAME_LENGTH = 4096;  //longer name = damages file
const char* DOWNLOAD_FILE = "ttdb_download.tmp"; // new download stored seperately so that the old version still is there just in case
const int CLIENT_TIMEOUT_SEC = 30;

// These two must be the same as in Phase 1
// this is now in helper.h 
//const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
//const int MAX_VARS_PER_FRAME = 16;
//const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) 
// removing structs from here since they are in my helper.h
// struct Variable
// {
//     string name;
//     int32_t value;
// };
// struct Frame
// {
//     string func_name;
//     int32_t argc;
//     Variable argv[MAX_VARS_PER_FRAME];
//     int32_t returnLine;
//     Variable locals[MAX_VARS_PER_FRAME];
//     int32_t localCount;
// };
// struct Snapshot
// {
//     Frame callStack[MAX_STACK_DEPTH];
//     int32_t stackDepth;
// };
// struct TTDBHeader 
// {
//     char magic[4]; // "TTDB"
//     int32_t version;
//     int32_t stepCount;
//     int64_t indexOffset;
// };

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

// these 2 functions are also in helper.h
// bool recvAll(int sock, char *buf, size_t n)
// {
//     // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
//     // Return false if the connection closes or fails before n bytes arrive.
// }
// bool sendAll(int sock, const char *buf, size_t n)
// {
//     // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
//     // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
// }

int connectToHost(const char* host, int port) {
    return connectTo(host, port);
}

bool uploadSource(int sock, const char* sourcePath) {
    if (!sendFileMessage(sock, STATUS_OK, sourcePath)) {
        return false;
    }
    shutdown(sock, SHUT_WR); // "upload finished"
    return true;
}

bool receiveResponse(int sock, const char* outTdbgPath, string& outError) {
    TrailerChecker checker;
    char* buf = new char[IO_BUFFER_SIZE];
    char* body = new char[IO_BUFFER_SIZE + CHECKSUM_SIZE];
    bool gotStatus = false;
    char status = STATUS_ERROR;
    FILE* out = NULL;
    string errorText = "";
    bool timedOut = false;
    bool connectionLost = false;

    while (true) {
        int n = recv(sock, buf, IO_BUFFER_SIZE, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timedOut = true;
            }
            else {
                connectionLost = true;
            }
            break;
        }

        int released = checker.feed(buf, n, body);
        int start = 0;
        if (released > 0 && !gotStatus) {
            gotStatus = true;
            status = body[0];
            start = 1;
            if (status == STATUS_OK) {
                out = fopen(outTdbgPath, "wb");
            }
        }
        if (status == STATUS_OK && out != NULL) {
            fwrite(body + start, 1, released - start, out);
        }
        else {
            errorText.append(body + start, released - start);
        }
    }
    delete[] buf;
    delete[] body;
    if (out != NULL) {
        fclose(out);
    }

    if (timedOut || connectionLost) {
        remove(outTdbgPath);
        if (timedOut) {
            outError = "no reply from the server (timed out)";
        }
        else {
            outError = "connection to the load balancer was lost";
        }
        return false;
    }
    if (!gotStatus) {
        outError = "connection closed with no reply";
        return false;
    }
    if (!checker.verify()) {
        remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status != STATUS_OK) {
        outError = errorText;
        return false;
    }
    if (out == NULL) {
        outError = "could not save the downloaded file";
        return false;
    }
    return true;
}

bool requestDebugFile(const string& host, int port, const string& sourcePath,
                      const char* outTdbgPath, string& outError) {
    FILE* test = fopen(sourcePath.c_str(), "rb");
    if (test == NULL) {
        outError = "cannot open " + sourcePath;
        return false;
    }
    fclose(test);

    int sock = connectToHost(host.c_str(), port);
    if (sock < 0) {
        outError = "cannot connect to the load balancer at " + host + ":" + to_string(port);
        return false;
    }
    setRecvTimeout(sock, CLIENT_TIMEOUT_SEC);

    if (!uploadSource(sock, sourcePath.c_str())) {
        close(sock);
        outError = "upload failed";
        return false;
    }
    bool ok = receiveResponse(sock, outTdbgPath, outError);
    close(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool readExact(int fd, void* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) {
            return false;
        }
        got = got + r;
    }
    return true;
}
 
bool readString(int fd, string& out) {
    int32_t len;
    if (!readExact(fd, &len, sizeof(int32_t))) {
        return false;
    }
    if (len < 0 || len > MAX_NAME_LENGTH) {
        return false;
    }
    out.resize(len);
    if (len > 0 && !readExact(fd, &out[0], len)) {
        return false;
    }
    return true;
}
 
bool readVariable(int fd, Variable& v) {
    if (!readString(fd, v.name)) {
        return false;
    }
    return readExact(fd, &v.value, sizeof(int32_t));
}
 
bool openTdbg(const char* path, TdbgReader& r) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
 
    TTDBHeader h;
    bool ok = readExact(fd, h.magic, 4)
        && readExact(fd, &h.version, sizeof(int32_t))
        && readExact(fd, &h.stepCount, sizeof(int32_t))
        && readExact(fd, &h.indexOffset, sizeof(int64_t));
 
    // the index must end exactly where the file ends
    int64_t fileSize = lseek(fd, 0, SEEK_END);
    if (!ok || h.magic[0] != 'T' || h.magic[1] != 'T' || h.magic[2] != 'D' || h.magic[3] != 'B'
        || h.version != 1 || h.stepCount <= 0 || h.indexOffset < HEADER_SIZE
        || h.indexOffset + (int64_t)h.stepCount * 8 != fileSize) {
        close(fd);
        return false;
    }
 
    vector<int64_t> index(h.stepCount);
    lseek(fd, h.indexOffset, SEEK_SET);
    for (int i = 0; i < h.stepCount; i++) {
        if (!readExact(fd, &index[i], sizeof(int64_t)) || index[i] < HEADER_SIZE || index[i] >= h.indexOffset) {
            close(fd);
            return false;
        }
    }
 
    r.file_index = fd;
    r.header = h;
    r.index = index;
    return true;
}
 
void closeTdbg(TdbgReader& r) {
    if (r.file_index >= 0) {
        close(r.file_index);
    }
    r.file_index = -1;
    r.index.clear();
}
 
Snapshot* readSnapshotAt(TdbgReader& r, int32_t step) {
    if (step < 0 || step >= r.header.stepCount) {
        return NULL;
    }
    int fd = r.file_index;
    lseek(fd, r.index[step], SEEK_SET);
 
    Snapshot* s = new Snapshot;
    bool ok = readExact(fd, &s->stackDepth, sizeof(int32_t));
    if (ok && (s->stackDepth < 0 || s->stackDepth > MAX_STACK_DEPTH)) {
        ok = false;
    }
 
    for (int i = 0; ok && i < s->stackDepth; i++) {
        Frame& fr = s->callStack[i];
        ok = readString(fd, fr.func_name) && readExact(fd, &fr.argc, sizeof(int32_t));
        if (ok && (fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)) {
            ok = false;
        }
        for (int j = 0; ok && j < fr.argc; j++) {
            ok = readVariable(fd, fr.argv[j]);
        }
        if (ok) {
            ok = readExact(fd, &fr.returnLine, sizeof(int32_t)) && readExact(fd, &fr.localCount, sizeof(int32_t));
        }
        if (ok && (fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)) {
            ok = false;
        }
        for (int j = 0; ok && j < fr.localCount; j++) {
            ok = readVariable(fd, fr.locals[j]);
        }
    }
 
    if (!ok) {
        delete s;
        return NULL;
    }
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession& s) {
    s.active_flag = false;
    s.reader.file_index = -1;
    s.currentStep = 0;
}
 
void closeSession(DebugSession& s) {
    if (s.active_flag) {
        closeTdbg(s.reader);
    }
    s.active_flag = false;
}
 


// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s);

bool showCurrent(DebugSession& s) {
    Snapshot* snap = readSnapshotAt(s.reader, s.currentStep);
    if (snap == NULL) {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot(s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}

bool moveBy(DebugSession& s, int32_t delta) {
    if (!s.active_flag) {
        cout << "no session yet, type: start <file>" << endl;
        return false;
    }
    int64_t total = s.reader.header.stepCount;

    if (delta > 0 && s.currentStep == total - 1) {
        cout << "already at the last step (" << total << ")" << endl;
        return false;
    }
    if (delta < 0 && s.currentStep == 0) {
        cout << "already at the first step" << endl;
        return false;
    }

    int64_t target = s.currentStep + delta;
    if (target >= total) {
        cout << "(only " << total << " steps, stopped at the last one)" << endl;
        target = total - 1;
    }
    if (target < 0) {
        cout << "(stopped at the first step)" << endl;
        target = 0;
    }
    s.currentStep = target;
    return showCurrent(s);
}

bool cmdStart(DebugSession& s, const string& sourcePath) {
    string error;
    cout << "sending " << sourcePath << " ..." << endl;
    if (!requestDebugFile(LB_HOST, LB_PORT, sourcePath, DOWNLOAD_FILE, error)) {
        cout << "error: " << error << endl;
        if (s.active_flag) {
            cout << "(your previous session is still open)" << endl;
        }
        return false;
    }

    TdbgReader newReader;
    newReader.file_index = -1;
    if (!openTdbg(DOWNLOAD_FILE, newReader)) {
        remove(DOWNLOAD_FILE);
        cout << "server sent a damaged .tdbg file" << endl;
        if (s.active_flag) {
            cout << "(your previous session is still open)" << endl;
        }
        return false;
    }

    // only now replace the old session
    if (s.active_flag) {
        closeTdbg(s.reader);
    }
    rename(DOWNLOAD_FILE, TDBG_FILE);
    s.reader = newReader;
    s.currentStep = 0;
    s.active_flag = true;

    cout << "loaded " << s.reader.header.stepCount << " steps" << endl;
    return showCurrent(s);
}

bool cmdNext(DebugSession& s) {
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession& s) {
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession& s, int32_t n) {
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession& s, int32_t n) {
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printVariables(const Variable vars[], int32_t count) {
    for (int i = 0; i < count; i++) {
        cout << vars[i].name << "=" << vars[i].value;
        if (i < count - 1) {
            cout << ", ";
        }
    }
}
 
void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s) {
    cout << "---- step " << step << " of " << totalSteps << " ----" << endl;
    if (s.stackDepth == 0) {
        cout << "  (program finished, call stack is empty)" << endl;
        return;
    }
    for (int i = 0; i < s.stackDepth; i++) {
        const Frame& fr = s.callStack[i];
        if (i == 0) {
            cout << "  -> ";
        }
        else {
            cout << "     ";
        }
        cout << fr.func_name;
        if (fr.argc > 0) {
            cout << "   params: ";
            printVariables(fr.argv, fr.argc);
        }
        if (fr.localCount > 0) {
            cout << "   locals: ";
            printVariables(fr.locals, fr.localCount);
        }
        cout << endl;
    }
}
 
void printHelp() {
    cout << "commands:" << endl;
    cout << "  start <file>     send a C-- program and load its timeline" << endl;
    cout << "  next             one step forward" << endl;
    cout << "  prv              one step back" << endl;
    cout << "  jmp next <n>     n steps forward" << endl;
    cout << "  jmp prv <n>      n steps back" << endl;
    cout << "  help             show this list" << endl;
    cout << "  quit / exit      leave" << endl;
}


// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool parsePositive(const string& s, int32_t& out) {
    if (s.size() == 0 || s.size() > 10) {
        return false;
    }
    for (int i = 0; i < (int)s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    long long v = atoll(s.c_str());
    if (v <= 0 || v > MAX_JUMP) {
        return false;
    }
    out = v;
    return true;
}

bool runCommand(DebugSession& s, const string& line, bool& quitRequested) {
    vector<string> words;
    stringstream ss(line);
    string w;
    while (ss >> w) {
        words.push_back(w);
    }
    if (words.size() > 0 && words[0] == PROGRAM_NAME) {
        words.erase(words.begin());
    }
    if (words.size() == 0) {
        return true;
    }

    string cmd = words[0];
    if (cmd == "start" && words.size() == 2) {
        return cmdStart(s, words[1]);
    }
    if (cmd == "next" && words.size() == 1) {
        return cmdNext(s);
    }
    if (cmd == "prv" && words.size() == 1) {
        return cmdPrv(s);
    }
    if (cmd == "jmp" && words.size() == 3) {
        int32_t n;
        if (!parsePositive(words[2], n)) {
            cout << "jmp needs a positive number, e.g. jmp next 3" << endl;
            return false;
        }
        if (words[1] == "next") {
            return cmdJmpNext(s, n);
        }
        if (words[1] == "prv") {
            return cmdJmpPrv(s, n);
        }
    }
    if (cmd == "help" && words.size() == 1) {
        printHelp();
        return true;
    }
    if ((cmd == "quit" || cmd == "exit") && words.size() == 1) {
        quitRequested = true;
        return true;
    }

    cout << "unknown command" << endl;
    printHelp();
    return false;
}

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        LB_HOST = argv[1];
    }
    if (argc >= 3) {
        LB_PORT = atoi(argv[2]);
    }
    signal(SIGPIPE, SIG_IGN);
    initCrcTable();

    DebugSession session;
    initSession(session);

    cout << "Time-Travel Debugger (load balancer " << LB_HOST << ":" << LB_PORT << ")" << endl;
    printHelp();

    bool quitRequested = false;
    string line;
    while (!quitRequested) {
        cout << PROMPT << flush;
        if (!getline(cin, line)) {
            cout << endl;
            break;
        }
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    return 0;
}

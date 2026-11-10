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

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
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

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession &s, int32_t delta)
{
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
}
bool cmdNext(DebugSession &s) {}
bool cmdPrv(DebugSession &s) {}
bool cmdJmpNext(DebugSession &s, int32_t n) {}
bool cmdJmpPrv(DebugSession &s, int32_t n) { }

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

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

}
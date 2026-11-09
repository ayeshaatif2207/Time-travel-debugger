// ======================= TIME-TRAVEL DEBUGGER - SERVER =======================

//   0. Receive    -- the upload from the load balancer is saved to source.bin
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy every line into resolve.bin as [offset][size][string], then patch CALL targets
//   3. Pass 0X2   -- execute resolve.bin line by line, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg
//   5. Reply      -- [0][session.tdbg][checksum] or [1][error message][checksum]
//
// Run: ./server 5001

#include <fstream>
#include <sstream>
#include <signal.h>
#include "helper.h"

const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const int32_t SOCKET_TIMEOUT_SEC = 5;

// the server keeps running after an error, so errors are saved here and sent to the client
string errorMessage = "";

bool setError(const string& msg) {
    errorMessage = msg;
    return false;
}

// Stack: back the live Call Stack during execution
template <typename T>
class Stack {
    struct Node {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack() {
        top = NULL;
        count = 0;
    }
    ~Stack() {
        while (top != NULL) {
            pop();
        }
    }
    bool push(const T& val) {
        if (count == MAX_STACK_DEPTH) {
            return false;
        }
        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
        return true;
    }
    T pop() {
        Node* temp = top;
        T value = temp->data;
        top = top->next;
        delete temp;
        count--;
        return value;
    }
    T& peek() {
        return top->data;
    }
    bool isEmpty() {
        return top == NULL;
    }
    int32_t depth() {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen) {
        int32_t i = 0;
        Node* curr = top;
        while (curr != NULL && i < maxLen) {
            out[i] = curr->data;
            i++;
            curr = curr->next;
        }
        return i;
    }
};

// Timeline : doubly linked list of Snapshots
struct TimelineNode {
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline {
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    Timeline() {
        head = NULL;
        tail = NULL;
        stepCount = 0;
    }
    ~Timeline() {
        TimelineNode* curr = head;
        while (curr != NULL) {
            TimelineNode* next = curr->next;
            delete curr->data;
            delete curr;
            curr = next;
        }
    }
    void record(Snapshot* s) {
        TimelineNode* newNode = new TimelineNode;
        newNode->data = s;
        newNode->next = NULL;
        newNode->prev = tail;
        if (head == NULL) {
            head = newNode;
            tail = newNode;
        }
        else {
            tail->next = newNode;
            tail = newNode;
        }
        stepCount++;
    }
    TimelineNode* begin() {
        return head;
    }
    int32_t getStepCount() {
        return stepCount;
    }
};

struct CallInfo {
    string argNames[MAX_VARS_PER_FRAME];
    int32_t argc;
};

void writeHeader(FILE* f, const TTDBHeader& h) {
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// PASS 0x0: READING source.bin + VALIDITY CHECK

bool readSourceLine(ifstream& in, string& out) {
    while (getline(in, out)) {
        if (out.size() > 0 && out[out.size() - 1] == '\r') {
            out.erase(out.size() - 1);
        }
        int start = 0;
        while (start < (int)out.size() && (out[start] == ' ' || out[start] == '\t')) {
            start++;
        }
        int end = (int)out.size() - 1;
        while (end >= start && (out[end] == ' ' || out[end] == '\t')) {
            end--;
        }
        if (start <= end) {
            out = out.substr(start, end - start + 1);
            return true;
        }
    }
    return false;
}

string firstWord(const string& line) {
    stringstream ss(line);
    string word = "";
    ss >> word;
    return word;
}

string secondWord(const string& line) {
    stringstream ss(line);
    string word1 = "";
    string word2 = "";
    ss >> word1 >> word2;
    return word2;
}

bool validateProgram(const char* sourcePath) {
    ifstream in(sourcePath);
    if (!in) {
        return setError("could not open the source file");
    }

    bool insideFunc = false;
    string line;
    int lineNo = 0;

    while (readSourceLine(in, line)) {
        lineNo++;
        string word = firstWord(line);
        string where = "line " + to_string(lineNo) + ": ";

        if (word == "func") {
            if (insideFunc) {
                return setError(where + "nested func is not allowed");
            }
            if (secondWord(line) == "") {
                return setError(where + "func has no name");
            }
            insideFunc = true;
        }
        else if (word == "func_end") {
            if (!insideFunc) {
                return setError(where + "func_end without func");
            }
            insideFunc = false;
        }
        else if (word == "set" || word == "add" || word == "sub" || word == "mul" || word == "div" || word == "call") {
            if (!insideFunc) {
                return setError(where + "instruction outside of a function");
            }
        }
        else {
            return setError(where + "unknown instruction " + word);
        }
    }

    if (insideFunc) {
        return setError("missing func_end");
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text) {
    int64_t pos = ftell(f);
    int32_t size = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, size, f);
    return pos;
}

int64_t readResolveRecord(FILE* f, string& outText) {
    int64_t offset;
    int32_t size;
    if (fread(&offset, sizeof(int64_t), 1, f) != 1) {
        return -1;
    }
    if (fread(&size, sizeof(int32_t), 1, f) != 1) {
        return -1;
    }
    outText = "";
    for (int i = 0; i < size; i++) {
        char c;
        if (fread(&c, 1, 1, f) != 1) {
            return -1;
        }
        outText = outText + c;
    }
    return offset;
}

// returns main's offset, or -1 with errorMessage set
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath) {
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath);
    FILE* f = fopen(resolveBinPath, "wb+");
    if (!in || f == NULL) {
        if (f != NULL) {
            fclose(f);
        }
        setError("could not open files for resolve");
        return -1;
    }

    string line;
    while (readSourceLine(in, line)) {
        int64_t pos = ftell(f);
        writeResolveRecord(f, pos, line);

        string word = firstWord(line);
        string name = secondWord(line);

        if (word == "func") {
            if (funcCount == MAX_FUNCS) {
                fclose(f);
                setError("too many functions");
                return -1;
            }
            for (int i = 0; i < funcCount; i++) {
                if (funcArray[i].funcName == name) {
                    fclose(f);
                    setError("function " + name + " is defined twice");
                    return -1;
                }
            }
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (word == "call") {
            if (patchCount == MAX_PATCHES) {
                fclose(f);
                setError("too many calls");
                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = pos;
            patches[patchCount].targetFuncName = name;
            patchCount++;
        }
    }

    for (int i = 0; i < patchCount; i++) {
        int found = -1;
        for (int j = 0; j < funcCount; j++) {
            if (funcArray[j].funcName == patches[i].targetFuncName) {
                found = j;
            }
        }
        if (found == -1) {
            fclose(f);
            setError("function " + patches[i].targetFuncName + " does not exist");
            return -1;
        }
        fseek(f, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&funcArray[found].byteOffsetInResolveBin, sizeof(int64_t), 1, f);
    }

    int64_t mainOffset = -1;
    for (int j = 0; j < funcCount; j++) {
        if (funcArray[j].funcName == "main") {
            mainOffset = funcArray[j].byteOffsetInResolveBin;
        }
    }
    fclose(f);

    if (mainOffset == -1) {
        setError("no main function");
    }
    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType {
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token {
    TokenType type;
    string text;
};

// returns the number of words, or -1 if there are too many
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens) {
    stringstream ss(line);
    string word;
    int32_t n = 0;
    while (ss >> word) {
        if (n == maxTokens) {
            return -1;
        }
        if (n == 0) {
            tokens[n].type = KEYWORD;
        }
        else if (n == 1) {
            tokens[n].type = IDENTIFIER;
        }
        else {
            tokens[n].type = PARAM;
        }
        tokens[n].text = word;
        n++;
    }
    return n;
}

bool isNumber(const string& s) {
    if (s.size() == 0) {
        return false;
    }
    int start = 0;
    if (s[0] == '-') {
        start = 1;
    }
    if (start == (int)s.size()) {
        return false;
    }
    for (int i = start; i < (int)s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

Variable* findVar(Frame& fr, const string& name) {
    for (int i = 0; i < fr.argc; i++) {
        if (fr.argv[i].name == name) {
            return &fr.argv[i];
        }
    }
    for (int i = 0; i < fr.localCount; i++) {
        if (fr.locals[i].name == name) {
            return &fr.locals[i];
        }
    }
    return NULL;
}

// puts the value of a number or a variable into out
bool getValue(Frame& fr, const string& token, int32_t& out) {
    if (isNumber(token)) {
        out = atoi(token.c_str());
        return true;
    }
    Variable* v = findVar(fr, token);
    if (v == NULL) {
        return setError("variable " + token + " does not exist in " + fr.func_name);
    }
    out = v->value;
    return true;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack) {
    Snapshot* s = new Snapshot;
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

bool executeLine(FILE* f, Stack<Frame>& callStack, Stack<CallInfo>& callInfos, Timeline& timeline, bool& finished) {
    string text;
    Token tokens[MAX_TOKENS];

    int64_t offset = readResolveRecord(f, text);
    if (offset == -1) {
        return setError("reached end of resolve.bin");
    }
    int32_t n = tokenizeLine(text, tokens, MAX_TOKENS);
    if (n == -1) {
        return setError("too many words in line: " + text);
    }
    string word = tokens[0].text;
    Frame& cur = callStack.peek();

    if (word == "set") {
        if (n < 3) {
            return setError("wrong set instruction: " + text);
        }
        int32_t value;
        if (!getValue(cur, tokens[2].text, value)) {
            return false;
        }
        Variable* v = findVar(cur, tokens[1].text);
        if (v != NULL) {
            v->value = value;
        }
        else {
            if (cur.localCount == MAX_VARS_PER_FRAME) {
                return setError("too many variables in " + cur.func_name);
            }
            cur.locals[cur.localCount].name = tokens[1].text;
            cur.locals[cur.localCount].value = value;
            cur.localCount++;
        }
    }
    else if (word == "add" || word == "sub" || word == "mul" || word == "div") {
        if (n < 3) {
            return setError("wrong " + word + " instruction: " + text);
        }
        Variable* a = findVar(cur, tokens[1].text);
        if (a == NULL) {
            return setError("variable " + tokens[1].text + " does not exist in " + cur.func_name);
        }
        int32_t b;
        if (!getValue(cur, tokens[2].text, b)) {
            return false;
        }
        if (word == "add") {
            a->value = a->value + b;
        }
        else if (word == "sub") {
            a->value = a->value - b;
        }
        else if (word == "mul") {
            a->value = a->value * b;
        }
        else {
            if (b == 0) {
                return setError("division by zero");
            }
            a->value = a->value / b;
        }
    }
    else if (word == "call") {
        int32_t argCount = n - 2;
        string args[MAX_VARS_PER_FRAME];
        for (int i = 0; i < argCount; i++) {
            args[i] = tokens[i + 2].text;
        }

        int64_t returnPos = ftell(f);
        fseek(f, (long)offset, SEEK_SET);
        readResolveRecord(f, text);
        int32_t m = tokenizeLine(text, tokens, MAX_TOKENS);
        int32_t paramCount = m - 2;
        if (argCount != paramCount) {
            return setError("wrong number of arguments for " + tokens[1].text);
        }

        Frame newFrame;
        newFrame.func_name = tokens[1].text;
        newFrame.argc = paramCount;
        newFrame.localCount = 0;
        newFrame.returnLine = (int32_t)returnPos;
        for (int i = 0; i < paramCount; i++) {
            newFrame.argv[i].name = tokens[i + 2].text;
            if (!getValue(cur, args[i], newFrame.argv[i].value)) {
                return false;
            }
        }

        CallInfo info;
        info.argc = argCount;
        for (int i = 0; i < argCount; i++) {
            if (isNumber(args[i])) {
                info.argNames[i] = "";
            }
            else {
                info.argNames[i] = args[i];
            }
        }

        if (!callStack.push(newFrame)) {
            return setError("stack overflow (more than 64 nested calls)");
        }
        callInfos.push(info);
    }
    else if (word == "func_end") {
        Frame done = callStack.pop();
        CallInfo info = callInfos.pop();

        if (callStack.isEmpty()) {
            finished = true;
        }
        else {
            Frame& caller = callStack.peek();
            for (int i = 0; i < info.argc; i++) {
                if (info.argNames[i] != "") {
                    Variable* v = findVar(caller, info.argNames[i]);
                    if (v != NULL) {
                        v->value = done.argv[i].value;
                    }
                }
            }
            fseek(f, (long)done.returnLine, SEEK_SET);
        }
    }
    else {
        return setError("unexpected line: " + text);
    }

    timeline.record(buildSnapshot(callStack));
    return true;
}

bool executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline) {
    FILE* f = fopen(resolveBinPath, "rb");
    if (f == NULL) {
        return setError("could not open resolve.bin");
    }

    Stack<Frame> callStack;
    Stack<CallInfo> callInfos;
    string text;

    fseek(f, (long)mainOffset, SEEK_SET);
    readResolveRecord(f, text);

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);

    CallInfo mainInfo;
    mainInfo.argc = 0;
    callInfos.push(mainInfo);

    bool finished = false;
    bool ok = true;
    while (ok && !finished) {
        ok = executeLine(f, callStack, callInfos, timeline, finished);
    }

    fclose(f);
    return ok;
}

// PASS 0x3: SERIALIZE TIMELINE

void writeString(FILE* f, const string& s) {
    int32_t len = (int32_t)s.size();
    fwrite(&len, sizeof(int32_t), 1, f);
    fwrite(s.c_str(), 1, len, f);
}

void writeSnapshot(FILE* f, Snapshot* s) {
    fwrite(&s->stackDepth, sizeof(int32_t), 1, f);
    for (int i = 0; i < s->stackDepth; i++) {
        Frame& fr = s->callStack[i];
        writeString(f, fr.func_name);
        fwrite(&fr.argc, sizeof(int32_t), 1, f);
        for (int j = 0; j < fr.argc; j++) {
            writeString(f, fr.argv[j].name);
            fwrite(&fr.argv[j].value, sizeof(int32_t), 1, f);
        }
        fwrite(&fr.returnLine, sizeof(int32_t), 1, f);
        fwrite(&fr.localCount, sizeof(int32_t), 1, f);
        for (int j = 0; j < fr.localCount; j++) {
            writeString(f, fr.locals[j].name);
            fwrite(&fr.locals[j].value, sizeof(int32_t), 1, f);
        }
    }
}

bool writeTdbg(Timeline& timeline, const char* tdbgPath) {
    FILE* f = fopen(tdbgPath, "wb");
    if (f == NULL) {
        return setError("could not create session.tdbg");
    }

    int32_t steps = timeline.getStepCount();
    TTDBHeader h;
    h.magic[0] = 'T';
    h.magic[1] = 'T';
    h.magic[2] = 'D';
    h.magic[3] = 'B';
    h.version = 1;
    h.stepCount = steps;
    h.indexOffset = 0;
    writeHeader(f, h);

    int64_t* index = new int64_t[steps];
    int i = 0;
    TimelineNode* curr = timeline.begin();
    while (curr != NULL) {
        index[i] = ftell(f);
        writeSnapshot(f, curr->data);
        i++;
        curr = curr->next;
    }

    h.indexOffset = ftell(f);
    for (i = 0; i < steps; i++) {
        fwrite(&index[i], sizeof(int64_t), 1, f);
    }
    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);

    delete[] index;
    fclose(f);
    return true;
}

// ---- Networking ----

// receives [status][source code][checksum] and saves the source code to sourcePath
bool receiveUpload(int sock, const char* sourcePath) {
    FILE* src = fopen(sourcePath, "wb");
    if (src == NULL) {
        return false;
    }

    TrailerChecker checker;
    char* buf = new char[IO_BUFFER_SIZE];
    char* body = new char[IO_BUFFER_SIZE + CHECKSUM_SIZE];
    bool gotStatus = false;
    bool ok = true;

    while (true) {
        int n = recv(sock, buf, IO_BUFFER_SIZE, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        int released = checker.feed(buf, n, body);
        int start = 0;
        if (released > 0 && !gotStatus) {
            gotStatus = true;
            start = 1; // skip the status byte
        }
        fwrite(body + start, 1, released - start, src);
    }

    delete[] buf;
    delete[] body;
    fclose(src);
    return ok && gotStatus && checker.verify();
}

bool runPasses(Timeline& timeline) {
    if (!validateProgram("source.bin")) {
        return false;
    }
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset == -1) {
        return false;
    }
    if (!executeProgram("resolve.bin", mainOffset, timeline)) {
        return false;
    }
    return writeTdbg(timeline, "session.tdbg");
}

void handleClient(int sock) {
    if (!receiveUpload(sock, "source.bin")) {
        cout << "[server] upload rejected" << endl;
        sendMessage(sock, STATUS_ERROR, "upload checksum mismatch, upload corrupted");
        return;
    }

    Timeline timeline;
    if (runPasses(timeline)) {
        cout << "[server] sending session.tdbg, " << timeline.getStepCount() << " steps" << endl;
        sendFileMessage(sock, STATUS_OK, "session.tdbg");
    }
    else {
        cout << "[server] error: " << errorMessage << endl;
        sendMessage(sock, STATUS_ERROR, errorMessage);
    }
}

int main(int argc, char* argv[]) {
    int port = 5001;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }
    signal(SIGPIPE, SIG_IGN);
    initCrcTable();

    int listenSock = createListenSocket(port);
    cout << "[server] listening on port " << port << endl;

    while (true) {
        int sock = accept(listenSock, NULL, NULL);
        if (sock < 0) {
            continue;
        }
        setRecvTimeout(sock, SOCKET_TIMEOUT_SEC);
        handleClient(sock);
        closeAfterReply(sock, 1);
    }
    return 0;
}
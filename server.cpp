// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)



#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

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
    void push(const T& val) {
        if (count == MAX_STACK_DEPTH) {
            cout << "Error: stack overflow" << endl;
            exit(1);
        }
        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
    }
    T pop() {
        if (top == NULL) {
            cout << "Error: stack is empty" << endl;
            exit(1);
        }
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
struct Snapshot;
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

// Core structs
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
struct CallInfo {
    string argNames[MAX_VARS_PER_FRAME];
    int32_t argc;
};
struct TTDBHeader {
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h) {
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry {
    string funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch {
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

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
        cout << "Error: could not open " << sourcePath << endl;
        return false;
    }

    bool insideFunc = false;
    string line;
    int lineNo = 0;

    while (readSourceLine(in, line)) {
        lineNo++;
        string word = firstWord(line);

        if (word == "func") {
            if (insideFunc) {
                cout << "Error line " << lineNo << ": nested func is not allowed" << endl;
                return false;
            }
            if (secondWord(line) == "") {
                cout << "Error line " << lineNo << ": func has no name" << endl;
                return false;
            }
            insideFunc = true;
        }
        else if (word == "func_end") {
            if (!insideFunc) {
                cout << "Error line " << lineNo << ": func_end without func" << endl;
                return false;
            }
            insideFunc = false;
        }
        else if (word == "set" || word == "add" || word == "sub" || word == "mul" || word == "div" || word == "call") {
            if (!insideFunc) {
                cout << "Error line " << lineNo << ": instruction outside of a function" << endl;
                return false;
            }
        }
        else {
            cout << "Error line " << lineNo << ": unknown instruction " << word << endl;
            return false;
        }
    }

    if (insideFunc) {
        cout << "Error: missing func_end" << endl;
        return false;
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
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath) {
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath);

    FILE* f = fopen(resolveBinPath, "wb+");
    if (!in || f == NULL) {


        cout << "Error: could not open files" << endl;
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
                cout << "Error: too many functions" << endl;
                fclose(f);
                return -1;
            }

            for (int i = 0; i < funcCount; i++) {
                if (funcArray[i].funcName == name) {
                    cout << "Error: function " << name << " is defined twice" << endl;
                    fclose(f);
                    return -1;
                }
            }


            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;

        }
        else if (word == "call") {
            if (patchCount == MAX_PATCHES) {
                cout << "Error: too many calls" << endl;
                fclose(f);
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
            cout << "Error: function " << patches[i].targetFuncName << " does not exist" << endl;
            fclose(f);
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


    if (mainOffset == -1) {
        cout << "Error: no main function" << endl;
    }

    fclose(f);
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
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens) {
    stringstream ss(line);
    string word;
    int32_t n = 0;
    while (ss >> word) {
        if (n == maxTokens) {
            cout << "Error: too many words in line " << line << endl;
            exit(1);
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
int32_t getValue(Frame& fr, const string& token) {
    if (isNumber(token)) {
        return atoi(token.c_str());
    }
    Variable* v = findVar(fr, token);
    if (v == NULL) {
        cout << "Error: variable " << token << " does not exist in " << fr.func_name << endl;
        exit(1);
    }
    return v->value;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack) {
    Snapshot* s = new Snapshot;
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline) {
    FILE* f = fopen(resolveBinPath, "rb");
    if (f == NULL) {
        cout << "Error: could not open " << resolveBinPath << endl;
        exit(1);
    }

    Stack<Frame> callStack;
    Stack<CallInfo> callInfos;
    string text;
    Token tokens[MAX_TOKENS];

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

    while (true) {
        int64_t offset = readResolveRecord(f, text);
        if (offset == -1) {
            cout << "Error: reached end of resolve.bin" << endl;
            exit(1);
        }

        int32_t n = tokenizeLine(text, tokens, MAX_TOKENS);
        string word = tokens[0].text;
        Frame& cur = callStack.peek();

        if (word == "set") {
            if (n < 3) {
                cout << "Error: wrong set instruction " << text << endl;
                exit(1);
            }
            int32_t value = getValue(cur, tokens[2].text);
            Variable* v = findVar(cur, tokens[1].text);
            if (v != NULL) {
                v->value = value;
            }
            else {
                if (cur.localCount == MAX_VARS_PER_FRAME) {
                    cout << "Error: too many variables in " << cur.func_name << endl;
                    exit(1);
                }
                cur.locals[cur.localCount].name = tokens[1].text;
                cur.locals[cur.localCount].value = value;
                cur.localCount++;
            }
        }
        else if (word == "add" || word == "sub" || word == "mul" || word == "div") {
            if (n < 3) {
                cout << "Error: wrong " << word << " instruction " << text << endl;
                exit(1);
            }
            Variable* a = findVar(cur, tokens[1].text);
            if (a == NULL) {
                cout << "Error: variable " << tokens[1].text << " does not exist in " << cur.func_name << endl;
                exit(1);
            }
            int32_t b = getValue(cur, tokens[2].text);

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
                    cout << "Error: division by zero" << endl;
                    exit(1);
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
                cout << "Error: wrong number of arguments for " << tokens[1].text << endl;
                exit(1);
            }

            Frame newFrame;
            newFrame.func_name = tokens[1].text;
            newFrame.argc = paramCount;
            newFrame.localCount = 0;
            newFrame.returnLine = (int32_t)returnPos;
            for (int i = 0; i < paramCount; i++) {
                newFrame.argv[i].name = tokens[i + 2].text;
                newFrame.argv[i].value = getValue(cur, args[i]);
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

            callStack.push(newFrame);
            callInfos.push(info);
        }
        else if (word == "func_end") {
            Frame finished = callStack.pop();
            CallInfo info = callInfos.pop();

            if (callStack.isEmpty()) {
                timeline.record(buildSnapshot(callStack));
                break;
            }

            Frame& caller = callStack.peek();
            for (int i = 0; i < info.argc; i++) {
                if (info.argNames[i] != "") {
                    Variable* v = findVar(caller, info.argNames[i]);
                    if (v != NULL) {
                        v->value = finished.argv[i].value;
                    }
                }
            }

            fseek(f, (long)finished.returnLine, SEEK_SET);
        }
        else {
            cout << "Error: unexpected line " << text << endl;
            exit(1);
        }

        timeline.record(buildSnapshot(callStack));
    }

    fclose(f);
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
void writeTdbg(Timeline& timeline, const char* tdbgPath) {
    FILE* f = fopen(tdbgPath, "wb");
    if (f == NULL) {
        cout << "Error: could not create " << tdbgPath << endl;
        exit(1);
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
}
// main section



int32_t main() {

    if (!validateProgram("source.bin")) {
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    if (mainOffset == -1) {
        return 1;
    }

    FILE* f = fopen("resolve.bin", "rb");
    string text;

    while (true) {
        int64_t pos = ftell(f);
        int64_t offset = readResolveRecord(f, text);
        if (offset == -1) {
            break;
        }
        cout << pos << "  " << offset << "  " << text << endl;
    }

    fclose(f);
    cout << "main is at " << mainOffset << endl;



    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    int step = 1;
    TimelineNode* curr = timeline.begin();
    while (curr != NULL) {
        Snapshot* s = curr->data;
        cout << "step " << step << ": ";
        if (s->stackDepth == 0) {
            cout << "program finished";
        }
        for (int i = 0; i < s->stackDepth; i++) {
            Frame& fr = s->callStack[i];
            cout << fr.func_name << "{ ";
            for (int j = 0; j < fr.argc; j++) {
                cout << fr.argv[j].name << "=" << fr.argv[j].value << " ";
            }
            for (int j = 0; j < fr.localCount; j++) {
                cout << fr.locals[j].name << "=" << fr.locals[j].value << " ";
            }
            cout << "}";
            if (i < s->stackDepth - 1) {
                cout << " -> ";
            }
        }
        cout << endl;
        step++;
        curr = curr->next;
    }

    writeTdbg(timeline, "session.tdbg");
    cout << "session.tdbg written with " << timeline.getStepCount() << " steps" << endl;

    return 0;
}

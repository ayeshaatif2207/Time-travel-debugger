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



//consts
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int IO_BUFFER_SIZE = 64 * 1024;
const int CHECKSUM_SIZE = 4;
const uint8_t STATUS_OK = 0;
const uint8_t STATUS_ERROR = 1;


// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 
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




struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};










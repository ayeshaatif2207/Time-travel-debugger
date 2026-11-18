// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code).
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctime>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
// const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes   (already in helper.h)
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
const int CLIENT_TIMEOUT_SEC = 30;
const int RELAY_TIMEOUT_SEC = 10;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = NULL;
        rear = NULL;
        count = 0;
    }
    ~Queue()
    {
        while (front != NULL) {
            dequeue();
        }
    }
    void enqueue(const T &val)
    {
        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = NULL;
        if (rear == NULL) {
            front = newNode;
            rear = newNode;
        }
        else {
            rear->next = newNode;
            rear = newNode;
        }
        count++;
    }
    T dequeue()
    {
        Node* temp = front;
        T value = temp->data;
        front = front->next;
        if (front == NULL) {
            rear = NULL;
        }
        delete temp;
        count--;
        return value;
    }
    bool isEmpty()
    {
        return front == NULL;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        int r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        sent = sent + r;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    return sendMessage(sock, STATUS_ERROR, message);
}
int connectToServer(const char *host, int port)
{
    return connectTo(host, port);
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    string command = string(SERVER_PATH) + " " + to_string(port) + " > server.log 2>&1 &";
    cout << "[lb] starting server: " << command << endl;
    return system(command.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0) {
        return sock;
    }
    if (!startServer(serverPort)) {
        return -1;
    }
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0) {
            return sock;
        }
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


int relayMessage(int from, int to)
{
    TrailerChecker checker;
    char* buf = new char[IO_BUFFER_SIZE];
    char* body = new char[IO_BUFFER_SIZE + CHECKSUM_SIZE];
    bool connectionOk = true;

    while (true) {
        int n = recv(from, buf, IO_BUFFER_SIZE, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            connectionOk = false;
            break;
        }
        int released = checker.feed(buf, n, body);
        if (released > 0 && !sendAll(to, body, released)) {
            connectionOk = false;
            break;
        }
    }
    delete[] buf;
    delete[] body;

    if (!connectionOk || checker.bodySize() == 0) {
        return -1;
    }
    if (!checker.verify()) {
        return 0;
    }
    sendAll(to, checker.trailer(), CHECKSUM_SIZE);
    return 1;
}

void relayOneClient(ClientInfo &c)
{
    if (time(NULL) - c.arrivedAt > CLIENT_TIMEOUT_SEC) {
        cout << "[lb] " << c.address << ":" << c.port << " waited too long, dropped" << endl;
        close(c.sock);
        return;
    }

    int serverSock = connectToServerWithWakeup();
    if (serverSock == -1) {
        cout << "[lb] backend server failed to start" << endl;
        sendError(c.sock, "backend server failed to start");
        closeAfterReply(c.sock, 2);
        return;
    }
    setRecvTimeout(c.sock, RELAY_TIMEOUT_SEC);
    setRecvTimeout(serverSock, RELAY_TIMEOUT_SEC);

    // client -> server
    int upload = relayMessage(c.sock, serverSock);
    if (upload != 1) {
        cout << "[lb] bad upload from " << c.address << ":" << c.port << endl;
        close(serverSock);
        if (upload == 0) {
            sendError(c.sock, "upload checksum mismatch, upload corrupted");
        }
        else {
            sendError(c.sock, "upload incomplete");
        }
        closeAfterReply(c.sock, 2);
        return;
    }
    shutdown(serverSock, SHUT_WR);

    // server -> client
    int reply = relayMessage(serverSock, c.sock);
    if (reply == -1) {
        sendError(c.sock, "backend server closed without replying");
    }
    else if (reply == 0) {
        cout << "[lb] reply checksum mismatch" << endl;
    }
    else {
        cout << "[lb] served " << c.address << ":" << c.port << endl;
    }
    close(serverSock);
    close(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    while (flag) {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int sock = accept4(listenSock, (sockaddr*)&addr, &len, SOCK_CLOEXEC);
        if (sock < 0) {
            if (flag) {
                perror("accept");
            }
            continue;
        }

        ClientInfo c;
        c.sock = sock;
        c.address = inet_ntoa(addr.sin_addr);
        c.port = ntohs(addr.sin_port);
        c.arrivedAt = time(NULL);

        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {
            queueMutex.unlock();
            cout << "[lb] queue full, refusing " << c.address << ":" << c.port << endl;
            sendError(sock, "server busy, try again later");
            closeAfterReply(sock, 1);
        }
        else {
            pendingClients.enqueue(c);
            int waiting = pendingClients.size();
            queueMutex.unlock();
            cout << "[lb] queued " << c.address << ":" << c.port << " (" << waiting << " waiting)" << endl;
        }
    }
}
void dispatcherLoop()
{
    while (flag) {
        bool gotClient = false;
        ClientInfo c;

        queueMutex.lock();
        if (!pendingClients.isEmpty()) {
            c = pendingClients.dequeue();
            gotClient = true;
        }
        queueMutex.unlock();

        if (gotClient) {
            relayOneClient(c);
        }
        else {
            usleep(10 * 1000);
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    signal(SIGINT, handler); 
    signal(SIGPIPE, SIG_IGN);
    initCrcTable();

    // ./load_balancer <server_ip> <server_port>
    if (argc >= 2) {
        serverHost = argv[1];
    }
    if (argc >= 3) {
        serverPort = atoi(argv[2]);
    }
    if (argc >= 4) {
        lbPort = atoi(argv[3]);
    }

    int listenSock = createListenSocket(lbPort);
    cout << "[lb] listening on port " << lbPort << ", server is " << serverHost << ":" << serverPort << endl;

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    cout << "\n[lb] shutting down" << endl;
    queueMutex.lock();
    while (!pendingClients.isEmpty()) {
        ClientInfo c = pendingClients.dequeue();
        close(c.sock);
    }
    queueMutex.unlock();
    close(listenSock);
    return 0;
}







#define RAPIDJSON_HAS_STDSTRING 1

#include <chrono>
#include <iostream>
#include <thread>
#include <functional>
#include <map>
#include <list>
#include <string>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <sstream>
#include <vector>
#include <future>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"


// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")

static unsigned short SERVER_PORT = 27015;

using namespace rapidjson;
using namespace std;


struct Command {
    string type;
    list<string> args;

    Command(string type, list<string> args) {
        this->type = type;
        this->args = args;
    }

    Command(const std::string& jsonString) {
        Document d;
        d.Parse(jsonString);

        Value& s = d["type"];
        type = s.GetString();
        s = d["args"];
        for (auto& field : s.GetArray()) {
            args.push_back(field.GetString());
        }

    }

    string toJson() const {
        Document d;
        d.SetObject();

        Value a(kArrayType);
        for (string arg : args) {
            Value strVal;
            strVal.SetString(arg.c_str(), arg.length(), d.GetAllocator());
            a.PushBack(strVal, d.GetAllocator());
        }
        d.AddMember("type", type, d.GetAllocator());
        d.AddMember("args", a, d.GetAllocator());

        StringBuffer buffer;
        Writer<StringBuffer> writer(buffer);
        d.Accept(writer);

        return buffer.GetString();
    }

};

int sendCommand(const Command& cmd, SOCKET sock) {

    int dataLen = cmd.toJson().length();
    int r = 0;
    // 길이를 먼저 보낸다.
    // binary 로 4bytes 를 길이로 encoding 한다.
    // 이 때 network byte order 로 변환하기 위해서 htonl 을 호출해야된다.
    int dataLenNetByteOrder = htonl(dataLen);
    int offset = 0;
    while (offset < 4) {
        r = send(sock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "failed to send length: " << WSAGetLastError() << std::endl;
            return 1;
        }
        offset += r;
    }

    // send 로 데이터를 보낸다.
    offset = 0;
    while (offset < dataLen) {
        r = send(sock, cmd.toJson().c_str() + offset, dataLen - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return 1;
        }
        offset += r;
    }
}


typedef function<int(const Command, SOCKET)> Handler;


map<string, list<Handler>> handlers;

int doMove(const Command& command, SOCKET socket) {
    string x;
    string y;
    cin >> x;
    cin >> y;
    if (stoi(x) < -3 || stoi(x) > 3 || stoi(y) > 3 || stoi(y) < -3) {
        cout << "error: x,y is too big" << endl;
        return 0;
    }

    list<string> args;
    args.push_back(x);
    args.push_back(y);
    Command cmd(command.type, args);
    return sendCommand(cmd, socket);
}

int doChat(const Command& command, SOCKET socket) {
    string to;
    string message;
    cin >> to;
    cin >> message;
    list<string> args;
    args.push_back(to);
    args.push_back(message);
    Command cmd(command.type, args);
    return sendCommand(cmd, socket);
}

int doBot(const Command& command, SOCKET socket) {
    //초기 세팅
    map<int, string> c;
    c.insert(make_pair(0, "move"));
    c.insert(make_pair(1, "attack"));
    c.insert(make_pair(2, "monsters"));
    c.insert(make_pair(3, "users"));
    c.insert(make_pair(4, "chat"));

    while (true) {
        this_thread::sleep_for(std::chrono::seconds(1));
        list<string> args;
        int i = rand() % 5;
        // 이동
        if (i == 0) {
            int x = rand() % 7 - 3;
            int y = rand() % 7 - 3;
            args.push_back(to_string(x));
            args.push_back(to_string(y));
        }
        //챗
        if (i == 4) {
            string to = "aa";
            string msg = "hi";
            args.push_back(to);
            args.push_back(msg);
        }
        //명령어 출력
        cout << c[i] << " ";
        for (auto& arg : args) {
            cout << arg << " ";
        }
        cout << endl;
        Command cmd(c[i], args);
        sendCommand(cmd, socket);
    }
    return 1;
}

void addCommandHandler(const string& commandType, const Handler& h) {
    handlers[commandType].push_back(h);
}

bool recvResponse(SOCKET sock) {
    bool lenCompleted = false;
    int packetLen = 0;
    char packet[65536];  // 최대 64KB 로 패킷 사이즈 고정
    int offset = 0;
    if (lenCompleted == false) {
        // 길이 정보를 받기 위해서 4바이트를 읽는다.
        // network byte order 로 전성되기 때문에 ntohl() 을 호출한다.
        int r = recv(sock, (char*)&(packetLen)+offset, 4 - offset, 0);
        if (r == SOCKET_ERROR) {
            cerr << "recv failed with error " << WSAGetLastError() << endl;
            exit(1);
        }
        else if (r == 0) {
            // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
            // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
            cerr << "Socket closed: " << sock << endl;
            exit(1);
        }
        offset += r;


        // network byte order 로 전송했었다.
        // 따라서 이를 host byte order 로 변경한다.
        int dataLen = ntohl(packetLen);
        packetLen = dataLen;

        // 우리는 Client class 안에 packet 길이를 최대 64KB 로 제한하고 있다.
        // 혹시 우리가 받을 데이터가 이것보다 큰지 확인한다.
        // 만일 크다면 처리 불가능이므로 오류로 처리한다.
        if (packetLen > sizeof(packet)) {
            cerr << "[" << sock << "] Too big data: " << packetLen << endl;
            return false;
        }

        // 이제 packetLen 을 완성했다고 기록하고 offset 을 초기화해준다.
        lenCompleted = true;
        offset = 0;
    }


    int r = recv(sock, packet + offset, packetLen - offset, 0);
    if (r == SOCKET_ERROR) {
        cerr << "recv failed with error " << WSAGetLastError() << endl;
        exit(1);
    }
    else if (r == 0) {
        // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
        // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
        exit(1);
    }
    offset += r;

    // 완성한 경우와 partial recv 인 경우를 구분해서 로그를 찍는다.
    if (offset == packetLen) {
        string recvPacket = string(packet).substr(0, packetLen);

        const Command& cmd = Command(recvPacket);
        if (cmd.type.compare("error") == 0) {
            return false;
        }
        if (cmd.type.compare("double_logined_error") == 0) {
            exit(1);
        }
        if (cmd.type.compare("dead") == 0) {
            cout << cmd.args.front() << endl;
            exit(1);
        }
        if (cmd.type.compare("notification") == 0) {
            cout << cmd.args.front() << endl;
            return true;
        }
        cout << recvPacket << endl;
        // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
        lenCompleted = false;
        offset = 0;
        packetLen = 0;
    }
    else {
        cout << "[" << sock << "] Partial recv " << r << "bytes. " << offset << "/" << packetLen << endl;
    }
}

void responseThread(SOCKET sock) {
    while (true) {
        recvResponse(sock);
    }
    
}

int main()
{
    addCommandHandler("signup", sendCommand);
    addCommandHandler("login", sendCommand);
    addCommandHandler("move", doMove);
    addCommandHandler("attack", sendCommand);
    addCommandHandler("monsters", sendCommand);
    addCommandHandler("users", sendCommand);
    addCommandHandler("chat", doChat);
    addCommandHandler("bot", doBot);

    int r = 0;

    // Winsock 을 초기화한다.
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        std::cerr << "WSAStartup failed with error " << r << std::endl;
        return 1;
    }

    struct sockaddr_in serverAddr;

    // TCP socket 을 만든다.
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // TCP 는 연결 기반이다. 서버 주소를 정하고 connect() 로 연결한다.
    // connect 후에는 별도로 서버 주소를 기재하지 않고 send/recv 를 한다.
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    r = connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        std::cerr << "connect failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }


    //SignUp
    string signUp;
    cout << "Sign-Up?(y or n): ";
    cin >> signUp;
    if (signUp.compare("y") == 0) {
        list<string> args;
        string userName;
        string passwd;
        cout << "create user-name: ";
        cin >> userName;
        cout << "create password: ";
        cin >> passwd;
        args.push_back(userName);
        args.push_back(passwd);

        
        const Command& cmd = { "signup", args };
        for (const Handler& h : handlers["signup"]) {
            int res = h(cmd, sock);
            if (res == 1) {
                return 1;
            }
        }

        //response
        if (!recvResponse(sock)) {
            cout << "sign up failed" << endl;
            return 1;
        }
        else
        {
            cout << "sign up success" << endl;
        }
    }

    //LOGIN
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            cout << "남은 로그인 시도: " << 3 - i <<endl;
        }
        list<string> args;
        string userName;
        string passwd;
        cout << "user-name: ";
        cin >> userName;
        cout << "password: ";
        cin >> passwd;
        args.push_back(userName);
        args.push_back(passwd);

        
        const Command& cmd = { "login", args};
        for (const Handler& h : handlers["login"]) {
            int res = h(cmd, sock);
            if (res == 1) {
                return 1;
            }
        }

        //response
        if (!recvResponse(sock)) {
            cout << "login failed" << endl;
        }
        else
        {
            // 로그인 3회 실패
            if (i == 2) {
                exit(1);
            }
            cout << "login success" << endl;
            break;
        }
    }

    list<shared_ptr<thread> > threads;
    shared_ptr<thread> response(new thread(responseThread, sock));
    threads.push_back(response);

    // 커맨드를 계속 받는다.
    while (true) {
        string command;
        cin >> command;

        list<string> null;
        const Command& cmd = { command, null};
        for (const Handler& h : handlers[command]) {
            int res = h(cmd, sock);
            if (res == 1) {
                return 1;
            }
        }
    }

    // 이제 threads 들을 join 한다.
    for (shared_ptr<thread>& workerThread : threads) {
        workerThread->join();
    }

    // Socket 을 닫는다.
    r = closesocket(sock);
    if (r == SOCKET_ERROR) {
        std::cerr << "closesocket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();
    return 0;
}

#define RAPIDJSON_HAS_STDSTRING 1

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <functional>
#include <map>
#include <list>
#include <string>
#include <time.h>

#include <hiredis/hiredis.h>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include <WinSock2.h>
#include <WS2tcpip.h>


using namespace rapidjson;
using namespace std;

// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")


static const unsigned short SERVER_PORT = 27015;
static const int NUM_WORKER_THREADS = 10;

redisContext* redisConn;

class Client {
public:
    SOCKET sock;  // 이 클라이언트의 active socket

    atomic<bool> doingRecv;

    bool lenCompleted;
    int packetLen;
    char packet[65536];  // 최대 64KB 로 패킷 사이즈 고정
    int offset;

    Client(SOCKET sock) : sock(sock), doingRecv(false), lenCompleted(false), packetLen(0), offset(0) {
    }

    ~Client() {
        cout << "Client destroyed. Socket: " << sock << endl;
    }

};

class Player {
public:

    string userName;
    
    // position
    int x;
    int y;

    int hp;
    int str;

    int hpPortionNum;
    int strPortionNum;

    // 최초 생성
    Player(string userName) : userName(userName), x(rand()%30) , y(rand() % 30), hp(30), str(3), hpPortionNum(1), strPortionNum(1) {
    }

    ~Player() {
        cout << "player logouted: " << userName << endl;
    }
};

class Monster {
public:
    string name;

    int hp;
    int str;

    int portionType; // 0: hp 1: str

    // position
    int x;
    int y;

    // 최초 생성
    Monster() : name("슬라임"), x(rand() % 30), y(rand() % 30), hp(rand()%5 + 5), str(rand()%3 + 3), portionType(rand()%2) {
    }

    ~Monster() {
        cout << "monster die: " << name << endl;
    }
};


// 소켓으로부터 Client 객체 포인터를 얻어내기 위한 map
// 소켓을 key 로 Client 객체 포인터를 value 로 집어넣는다. (shared_ptr 을 사용한다.)
// 나중에 key 인 소켓으로 찾으면 연결된 Client 객체 포인터가 나온다.
// key 인 소켓으로 지우면 해당 엔트리는 사라진다.
// key 목록은 소켓 목록이므로 현재 남아있는 소켓들이라고 생각할 수 있다.
map<SOCKET, shared_ptr<Client> > activeClients;
mutex activeClientsMutex;

// 패킷이 도착한 client 들의 큐
queue<shared_ptr<Client> > jobQueue;
mutex jobQueueMutex;
condition_variable jobQueueFilledCv;

//로그인한 유저
map<SOCKET, shared_ptr<Player>> activePlayers;
mutex activePlayersMutex;

//몬스터
list<shared_ptr<Monster>> activeMonsters;
mutex activeMonstersMutex;

struct Command {
    string type;
    list<string> args;

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


};

typedef function<void(const Command, shared_ptr<Client>)> Handler;


map<string, list<Handler>> handlers;

int sendResponse(const Command& cmd, SOCKET sock) {
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

redisReply* rediscmd(string base, string key, string val) {
    string q1;
    string k =  " "+ key;

    q1.append(k);
    if (val.compare("") != 0) {
        q1.append(" " + val);
    }
    return (redisReply*)redisCommand(redisConn, (base + q1).c_str());
}

void setPersist(string userName, shared_ptr<Client> client) {
    string base = "PERSIST ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "", "");
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
}

void setExpire(string userName, shared_ptr<Client> client) {
    string base = "EXPIRE ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "", "600");
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
}

void setUserPwd(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "pwd", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

    redisReply* reply = rediscmd(base, "new", "1");
    if (reply->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setUserPosX(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "position_x", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void incrUserPosY(string userName, shared_ptr<Client> client, string value) {
    string base = "HINCRBY ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "position_y", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void incrUserPosX(string userName, shared_ptr<Client> client, string value) {
    string base = "HINCRBY ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "position_x", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setUserPosY(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "position_y", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void incrUserHp(string userName, shared_ptr<Client> client, string value) {
    string base = "HINCRBY ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "hp", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void incrUserStr(string userName, shared_ptr<Client> client, string value) {
    string base = "HINCRBY ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "str", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setUserHp(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "hp", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setUserStr(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "str", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setUserStrPortion(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "str_portion", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setUserHpPortion(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "hp_portion", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

void setNew(string userName, shared_ptr<Client> client, string value) {
    string base = "HSET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "new", value);
    if (reply1->type == REDIS_REPLY_ERROR) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }

}

string getUserPwd(string userName, shared_ptr<Client> client) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "pwd", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return reply1->str;
    }
    return string("");
}

int getUserPosX(string userName, shared_ptr<Client> client) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "position_x", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return stoi(reply1->str);
    }
    return 0;
}

int getUserPosY(string userName, shared_ptr<Client> client ) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "position_y", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return stoi(reply1->str);
    }
    return 0;
}

int getUserHp(string userName, shared_ptr<Client> client ) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "hp", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return stoi(reply1->str);
    }
    return 0;
}

int getUserStr(string userName, shared_ptr<Client> client) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "str", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return stoi(reply1->str);
    }
    return 0;
}

int getUserStrPortion(string userName, shared_ptr<Client> client) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "str_portion", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return stoi(reply1->str);
    }
    return 0;
}

int getUserHpPortion(string userName, shared_ptr<Client> client) {
    string base = "HGET ";
    base.append(userName);

    redisReply* reply1 = rediscmd(base, "hp_portion", "");
    if (reply1->type == REDIS_REPLY_ERROR || reply1->str == NULL) {
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    if (reply1->str != nullptr) {
        return stoi(reply1->str);
    }
    return 0;
}

//회원가입
void signup(const Command& command, shared_ptr<Client> client) {
    list<string> args(command.args);
    string userName = args.front();
    args.pop_front();

    // 유저생성 
    setUserPwd(userName, client, args.front());
    list<string> null;
    const Command& cmd = { "success", null };
    sendResponse(cmd, client->sock);


}

// 로그인
void login(const Command& command, shared_ptr<Client> client) {
    list<string> args(command.args);
    string userName = args.front();
    args.pop_front();

    string getPwd = getUserPwd(userName, client);
    if ( !getPwd.empty() && getPwd.compare(args.front()) == 0) {
        // 로그인 성공

        lock_guard<mutex> lg(activePlayersMutex);

        bool islogined = false;
        for (auto& entry : activePlayers) {
            shared_ptr<Player> player = entry.second;
            if (player->userName.compare(userName) == 0) {
                islogined = true;
            }
        }
        if (!islogined) {
            // 로그인된 플레이어 추가

            redisReply* re = rediscmd("HGET " + userName, "new", "");
            string n = "1";
            shared_ptr<Player> player(new Player(userName));
            if (re->type == REDIS_REPLY_STRING) {
                if (n.compare(re->str) == 0) {
                    //신규 로그인
                    setUserPosX(player->userName, client, to_string(player->x));
                    setUserPosY(player->userName, client, to_string(player->y));
                    setUserHp(player->userName, client, to_string(player->hp));
                    setUserStr(player->userName, client, to_string(player->str));
                    setUserHpPortion(player->userName, client, to_string(player->hpPortionNum));
                    setUserStrPortion(player->userName, client, to_string(player->strPortionNum));
                    setNew(player->userName, client, "0");
                }
                else {
                    // 기존 로그인
                    player->x = getUserPosX(userName, client);
                    player->y = getUserPosY(userName, client);
                    int hp = getUserHp(userName, client);
                    if (hp < 0) {
                        player->hp = 30;
                    }
                    player->str = getUserStr(userName, client);
                    player->hpPortionNum = getUserHpPortion(userName, client);
                    player->strPortionNum = getUserStrPortion(userName, client);
      
                }

            }

            setPersist(userName, client);
            activePlayers.insert(make_pair(client->sock, player));

            list<string> null;
            const Command& cmd = { "success", null };
            sendResponse(cmd, client->sock);
        }
        else {
            // 기존 플레이어 아웃
            for (auto& entry : activePlayers) {
                SOCKET sock = entry.first;
                shared_ptr<Player> player = entry.second;
                if (userName.compare(player->userName) == 0) {
                    list<string> msg;
                    msg.push_back("double logined");
                    const Command& cmd = { "double_logined_error", msg };
                    sendResponse(cmd, sock);
                }
            }
        }
    }
    else
    {
        // 아이디 오류
        cout << "error" << endl;
        list<string> null;
        const Command& cmd = { "error", null };
        sendResponse(cmd, client->sock);
    }
    
}


void doMove(const Command& command, shared_ptr<Client> client) {
    list<string> args(command.args);
    string x = args.front();
    args.pop_front();
    string y = args.front();
    {
        lock_guard<mutex> lg(activePlayersMutex);
        shared_ptr<Player> player = activePlayers[client->sock];
        int nx = player->x + stoi(x);
        int ny = player->y + stoi(y);
        if (0 <= nx&&nx < 30 && 0 <= ny&&ny < 30) {
            incrUserPosX(player->userName, client, x);
            incrUserPosY(player->userName, client, y);
            player->x += stoi(x);
            player->y += stoi(y);
        }
        else {
            list<string> res;
            res.push_back(string("out of map"));
            const Command& cmd = { "move_error", res };
            sendResponse(cmd, client->sock);
            return;
        }
    }
    list<string> null;
    const Command& cmd = { "success", null };
    sendResponse(cmd, client->sock);
}

void doAttack(const Command& command, shared_ptr<Client> client) {
    {
        lock_guard<mutex> lgp(activePlayersMutex);
        shared_ptr<Player> p = activePlayers[client->sock];
        lock_guard<mutex> lgm(activeMonstersMutex);
        list<shared_ptr<Monster>> to_del;
        for (auto& monster : activeMonsters) {
            if (p->x - 1 <= monster->x && monster->x <= p->x + 1 && p->y - 1 <= monster->y && monster->y <= p->y + 1) {
                monster->hp -= p->str;
                list<string> msg;

                //몬스터가 죽으면?
                if (monster->hp <= 0) {
                    if (monster->portionType == 0) {
                        // 0 == hp portion
                        p->hpPortionNum += 1;
                        incrUserHp(p->userName, client, "1");
                        list<string> res;
                        res.push_back(string("hp portion"));
                        const Command& cmd = { "new_item", res };
                        sendResponse(cmd, client->sock);
                    }
                    else {
                        p->strPortionNum += 1;
                        incrUserStr(p->userName, client, "1");
                        list<string> res;
                        res.push_back(string("str portion"));
                        const Command& cmd = { "new_item", res };
                        sendResponse(cmd, client->sock);
                    }
                    to_del.push_back(monster);
                }
                //notification
                msg.push_back(string("\"" + p->userName + "\"" + "이 " + "\"" + monster->name + "\""+ "을 공격해서 데미지" + to_string(p->str)+ "을/를 가했습니다."));
                for (auto& player : activePlayers) {
                    const Command& cmd = { "notification", msg };
                    sendResponse(cmd, player.first);
                }
            }
        }
        for (auto& del : to_del) {
            activeMonsters.remove(del);
        }
    }
}

void doMonsters(const Command& command, shared_ptr<Client> client) {
    list<string> monsters;
    {
        lock_guard<mutex> lg(activeMonstersMutex);
        for (auto& monster : activeMonsters) {
            monsters.push_back(monster->name + " (" + to_string(monster->x) + ", " + to_string(monster->y) + ")");
        }
    }
    const Command& cmd = { "success", monsters };
    sendResponse(cmd, client->sock);
}

void doUsers(const Command& command, shared_ptr<Client> client) {
    list<string> users;
    {
        lock_guard<mutex> lg(activePlayersMutex);

        for (auto& entry : activePlayers) {
            SOCKET sock = entry.first;
            shared_ptr<Player> player = entry.second;
            users.push_back("userName: "+player->userName + " (" + to_string(player->x) + ", " + to_string(player->y)+")");
        }
    }
    const Command& cmd = { "success", users };
    sendResponse(cmd, client->sock);
}

void doChat(const Command& command, shared_ptr<Client> client) {
    list<string> args(command.args);
    string to = args.front();
    args.pop_front();
    string message = args.front();
    list<string> msg;
    msg.push_back(message);
    bool isChatSuccess = false;
    {
        lock_guard<mutex> lg(activePlayersMutex);
        for (auto& entry : activePlayers) {
            SOCKET sock = entry.first;
            shared_ptr<Player> player = entry.second;
            if (to.compare(player->userName) == 0) {
                const Command& cmd = { "message", msg };
                sendResponse(cmd, sock);
                isChatSuccess = true;
            }
        }
    }
    // 채팅 실패시
    if (!isChatSuccess) {
        list<string> res;
        res.push_back("offline user");
        const Command& cmd = { "message_error", msg };
        sendResponse(cmd, client->sock);
    }
}

void doBot(const Command& command, shared_ptr<Client> client) {
    cout << "bot" << endl;
}

void addCommandHandler(const string& commandType, const Handler& h) {
    handlers[commandType].push_back(h);
}




SOCKET createPassiveSocket() {
    // TCP socket 을 만든다.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        cerr << "socket failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // socket 을 특정 주소, 포트에 바인딩 한다.
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int r = ::bind(passiveSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        cerr << "bind failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // TCP 는 연결을 받는 passive socket 과 실제 통신을 할 수 있는 active socket 으로 구분된다.
    // passive socket 은 socket() 뒤에 listen() 을 호출함으로써 만들어진다.
    // active socket 은 passive socket 을 이용해 accept() 를 호출함으로써 만들어진다.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        cerr << "listen faijled with error " << WSAGetLastError() << endl;
        return 1;
    }

    return passiveSock;
}


bool processClient(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    // 이전에 어디까지 작업했는지에 따라 다르게 처리한다.
    // 이전에 packetLen 을 완성하지 못했다. 그걸 완성하게 한다.
    if (client->lenCompleted == false) {
        // 길이 정보를 받기 위해서 4바이트를 읽는다.
        // network byte order 로 전성되기 때문에 ntohl() 을 호출한다.
        int r = recv(activeSock, (char*)&(client->packetLen) + client->offset, 4 - client->offset, 0);
        if (r == SOCKET_ERROR) {
            cerr << "recv failed with error " << WSAGetLastError() << endl;
            return false;
        }
        else if (r == 0) {
            // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
            // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
            cerr << "Socket closed: " << activeSock << endl;
            return false;
        }
        client->offset += r;

        // 완성 못했다면 다음번에 계속 시도할 것이다.
        if (client->offset < 4) {
            return true;
        }

        // network byte order 로 전송했었다.
        // 따라서 이를 host byte order 로 변경한다.
        int dataLen = ntohl(client->packetLen);
        //cout << "[" << activeSock << "] Received length info: " << dataLen << endl;
        client->packetLen = dataLen;

        // 우리는 Client class 안에 packet 길이를 최대 64KB 로 제한하고 있다.
        // 혹시 우리가 받을 데이터가 이것보다 큰지 확인한다.
        // 만일 크다면 처리 불가능이므로 오류로 처리한다.
        if (client->packetLen > sizeof(client->packet)) {
            cerr << "[" << activeSock << "] Too big data: " << client->packetLen << endl;
            return false;
        }

        // 이제 packetLen 을 완성했다고 기록하고 offset 을 초기화해준다.
        client->lenCompleted = true;
        client->offset = 0;
    }

    // 여기까지 도달했다는 것은 packetLen 을 완성한 경우다. (== lenCompleted 가 true)
    // packetLen 만큼 데이터를 읽으면서 완성한다.
    if (client->lenCompleted == false) {
        return true;
    }

    int r = recv(activeSock, client->packet + client->offset, client->packetLen - client->offset, 0);
    if (r == SOCKET_ERROR) {
        cerr << "recv failed with error " << WSAGetLastError() << endl;
        return false;
    }
    else if (r == 0) {
        // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
        // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
        return false;
    }
    client->offset += r;

    // 완성한 경우와 partial recv 인 경우를 구분해서 로그를 찍는다.
    if (client->offset == client->packetLen) {
        string recvPacket = string(client->packet).substr(0, client->packetLen);
        //cout << "[" << activeSock << "] Received " << client->packetLen << " bytes" << endl;
        //cout << "[" << recvPacket << "] Received " << client->packetLen << " bytes" << endl;
        
        const Command& cmd = Command(recvPacket);
        if (handlers[cmd.type].empty()) {
            cout << "등록되지 않은 명령어입니다." << endl;
        }
        for (const Handler& h : handlers[cmd.type]) {
            h(cmd, client);
        }

        // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
        client->lenCompleted = false;
        client->offset = 0;
        client->packetLen = 0;
    }
    else {
        cout << "[" << activeSock << "] Partial recv " << r << "bytes. " << client->offset << "/" << client->packetLen << endl;
    }
    return true;
}


void workerThreadProc(int workerId) {
    cout << "Worker thread is starting. WorkerId: " << workerId << endl;

    while (true) {
        // lock_guard 혹은 unique_lock 의 경우 scope 단위로 lock 범위가 지정되므로,
        // 아래처럼 새로 scope 을 열고 lock 을 잡는 것이 좋다.
        shared_ptr<Client> client;
        {
            unique_lock<mutex> ul(jobQueueMutex);

            // job queue 에 이벤트가 발생할 때까지 condition variable 을 잡을 것이다.
            while (jobQueue.empty()) {
                jobQueueFilledCv.wait(ul);
            }

            // while loop 을 나왔다는 것은 job queue 에 작업이 있다는 것이다.
            // queue 의 front 를 기억하고 front 를 pop 해서 큐에서 뺀다.
            client = jobQueue.front();
            jobQueue.pop();

        }

        // 위의 block 을 나왔으면 client 는 존재할 것이다.
        // 그러나 혹시 나중에 코드가 변경될 수도 있고 그러니 client 가 null 이 아닌지를 확인 후 처리하도록 하자.
        // shared_ptr 은 boolean 이 필요한 곳에 쓰일 때면 null 인지 여부를 확인해준다.
        if (client) {
            SOCKET activeSock = client->sock;
            bool successful = processClient(client);
            if (successful == false) {
                closesocket(activeSock);

                // 전체 동접 클라이언트 목록인 activeClients 에서 삭제한다.
                // activeClients 는 메인 쓰레드에서도 접근한다. 따라서 mutex 으로 보호해야될 대상이다.
                // lock_guard 가 scope 단위로 동작하므로 lock 잡히는 영역을 최소화하기 위해서 새로 scope 을 연다.
                {
                    lock_guard<mutex> lg(activeClientsMutex);
                    lock_guard<mutex> lgp(activePlayersMutex);
                    if (activePlayers[activeSock] != NULL) {
                        setExpire(activePlayers[activeSock]->userName, activeClients[activeSock]);
                    }
 
                    // activeClients 는 key 가 SOCKET 타입이고, value 가 shared_ptr<Client> 이므로 socket 으로 지운다.
                    activeClients.erase(activeSock);
                    activePlayers.erase(activeSock);
                }
            }
            else {
                // 다시 select 대상이 될 수 있도록 플래그를 꺼준다.
                // 참고로 오직 성공한 경우만 이 flag 를 다루고 있다.
                // 그 이유는 오류가 발생한 경우는 어차피 동접 리스트에서 빼버릴 것이고 select 를 할 일이 없기 때문이다.
                client->doingRecv.store(false);
            }
        }
    }

    cout << "Worker thread is quitting. Worker id: " << workerId << endl;
}

void monsterGenerator(){
    while (true) {    
        cout << "monsterGenerator" << endl;
        while (activeMonsters.size() < 10) {
            lock_guard<mutex> lg(activeMonstersMutex);
            shared_ptr<Monster> monster(new Monster());
            activeMonsters.push_back(monster);
        }

        // 1분만다 몬스터 10마리까지 채우기
        this_thread::sleep_for(std::chrono::seconds(60));
    }
}

void monsterAttacker() {
    while (true) {
        // 5초마다 주변 공격
        this_thread::sleep_for(std::chrono::seconds(5));
        {
            lock_guard<mutex> lgp(activePlayersMutex);
            lock_guard<mutex> lgm(activeMonstersMutex);
            list<shared_ptr<Player>> to_del;
            for (auto& monster : activeMonsters) {
                for (auto& entry : activePlayers) {
                    shared_ptr<Player> player = entry.second;
                    if (monster->x - 1 <= player->x && player->x <= monster->x + 1 && monster->y - 1 <= player->y && player->y <= monster->y + 1) {
                        player->hp -= monster->str;
                        //몬스터가 죽으면?
                        list<string> res;
                        res.push_back(string("you died"));
                        if (player->hp <= 0) {
                            const Command& cmd = { "dead", res };
                            sendResponse(cmd, entry.first);
                            to_del.push_back(player);
                        }
                        list<string> msg;
                        //notification
                        msg.push_back(string("\"" + monster->name + "\"" + "이 " + "\"" + player->userName + "\"" + "을 공격해서 데미지" + to_string(monster->str) + "을/를 가했습니다."));
                        for (auto& player : activePlayers) {
                            const Command& cmd = { "notification", msg };
                            sendResponse(cmd, player.first);
                        }
                    }
                }
            }
        }
    }
}

int main()
{
    srand(time(NULL));
    addCommandHandler("signup", signup);
    addCommandHandler("login", login);
    addCommandHandler("move", doMove);
    addCommandHandler("attack", doAttack);
    addCommandHandler("monsters", doMonsters);
    addCommandHandler("users", doUsers);
    addCommandHandler("chat", doChat);
    addCommandHandler("bot", doBot);
    int r = 0;

    redisConn = redisConnect("127.0.0.1", 6379);
    if (redisConn == NULL || redisConn->err) {
        if (redisConn) {
            printf("Error: %s\n", redisConn->errstr);
        }
        else
        {
            printf("Can't allocate redis context\n");
        }
    }

    // Winsock 을 초기화한다.
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        cerr << "WSAStartup failed with error " << r << endl;
        return 1;
    }

    // passive socket 을 만들어준다.
    SOCKET passiveSock = createPassiveSocket();

    // 매번 worker thread 갯수를 나열하는게 귀찮으니 고정 크기 배열 대신 여기서는 list 를 썼다.
    // loop 을 돌 때 worker thread 갯수를 한번만 나열하면 그 뒤에는 list 를 순회하는 방식으로 갯수 관계 없이 동작하게 한다.
    // new thread(workerThreadProc) 으로 인자 없이 thread 를 만들 수도 있으나,
    // 여기서는 연습용으로 worker id 를 인자로 넘겨보았다.
    list<shared_ptr<thread> > threads;
    for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
        shared_ptr<thread> workerThread(new thread(workerThreadProc, i));
        threads.push_back(workerThread);
    }

    //몬스터 생성기
    shared_ptr<thread> monsterGenThread(new thread(monsterGenerator));
    threads.push_back(monsterGenThread);

    //몬스터 공격
    shared_ptr<thread> monsterAttThread(new thread(monsterAttacker));
    threads.push_back(monsterAttThread);

    // 서버는 사용자가 중단할 때까지 프로그램이 계속 동작해야된다.
    // 따라서 loop 으로 반복 처리한다.
    while (true) {
        // select 를 이용해 읽기 이벤트와 예외 이벤트가 발생하는 소켓을 알아낼 것이다.
        // fd_set 은 C/C++ 에서 정한 것이 아니라 typedef 로 정해진 custom type 이다.
        // 그런데 우리는 구체적인 구현은 신경쓰지 않아도 되고 대신 FD_XXX() 의 매크로 함수를 이용해 접근할 것이다.
        fd_set readSet, exceptionSet;

        // 위의 socket set 을 초기화한다.
        FD_ZERO(&readSet);
        FD_ZERO(&exceptionSet);

        // select 의 첫번째 인자는 max socket 번호에 1을 더한 값이다.
        // 따라서 max socket 번호를 계산한다.
        SOCKET maxSock = -1;

        // passive socket 은 기본으로 각 socket set 에 포함되어야 한다.
        FD_SET(passiveSock, &readSet);
        FD_SET(passiveSock, &exceptionSet);
        maxSock = max(maxSock, passiveSock);

        // 현재 남아있는 active socket 들에 대해서도 모두 set 에 넣어준다.
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;

            // 이미 readable 하다고 해서 job queue 에 넣은 경우 다시 select 를 하면 다시 readable 하게 나온다.
            // 이렇게 되면 job queue 안에 중복으로 client 가 들어가게 되므로,
            // 아직 job queue 안에 안들어간 클라이언트만 select 확인 대상으로 한다.
            if (client->doingRecv.load() == false) {
                FD_SET(activeSock, &readSet);
                FD_SET(activeSock, &exceptionSet);
                maxSock = max(maxSock, activeSock);
            }
        }

        // select 를 해준다. 동접이 있더라도 doingRecv 가 켜진 것들은 포함하지 않았었다.
        // 이런 것들은 worker thread 가 처리 후 doingRecv 를 끄면 다시 select 대상이 되어야 하는데,
        // 아래는 timeout 없이 한정 없이 select 를 기다리므로 doingRecv 변경으로 다시 select 되어야 하는 것들이
        // 굉장히 오래 걸릴 수 있다. 그런 문제를 해결하기 위해서 select 의 timeout 을 100 msec 정도로 제한한다.
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100;
        r = select(maxSock + 1, &readSet, NULL, &exceptionSet, &timeout);

        // 회복할 수 없는 오류이다. 서버를 중단한다.
        if (r == SOCKET_ERROR) {
            cerr << "select failed: " << WSAGetLastError() << endl;
            break;
        }

        // 기존에는 이벤트가 발생할 때까지 한정 없이 기다려도 됐기 때문에 select 의 반환값이 에러인지만 확인했다.
        // 그러나 이제는 100msec timeout 을 걸기 때문에 아무 이벤트가 발생하지 않더라도 select 는 종료된다.
        // 이 때는 모든 socket 들을 FD_ISSET 을 하게 되면, 소켓 갯수가 많을 때 무의미하게 
        // loop 을 돌게 되는 꼴이 된다.
        // 따라서 아무것도 반환을 안한 경우는 아래를 처리하지 않고 바로 다시 select 를 하게 한다.
        // 다행히 select 의 반환값은 오류일 때 SOCKET_ERROR, 그 외의 경우 이벤트가 발생한 소켓 갯수이다.
        // 따라서 반환값 r 이 0인 경우는 아래를 스킵하게 한다.
        if (r == 0) {
            continue;
        }

        // passive socket 이 readable 하다면 이는 새 연결이 들어왔다는 것이다.
        // 새 클라이언트 객체를 동적으로 만들고 
        if (FD_ISSET(passiveSock, &readSet)) {
            // passive socket 을 이용해 accept() 를 한다.
            // accept() 는 blocking 이지만 우리는 이미 select() 를 통해 새 연결이 있음을 알고 accept() 를 호출한다.
            // 따라서 여기서는 blocking 되지 않는다.
            // 연결이 완료되고 만들어지는 소켓은 active socket 이다.
            cout << "Waiting for a connection" << endl;
            struct sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET activeSock = accept(passiveSock, (sockaddr*)&clientAddr, &clientAddrSize);

            // accpet() 가 실패하면 해당 연결은 이루어지지 않았음을 의미한다.
            // 그 여결이 잘못된다고 하더라도 다른 연결들을 처리해야되므로 에러가 발생했다고 하더라도 계속 진행한다.
            if (activeSock == INVALID_SOCKET) {
                cerr << "accept failed with error " << WSAGetLastError() << endl;
                return 1;
            }
            else {
                // 새로 client 객체를 만든다.
                shared_ptr<Client> newClient(new Client(activeSock));

                // socket 을 key 로 하고 해당 객체 포인터를 value 로 하는 map 에 집어 넣는다.
                activeClients.insert(make_pair(activeSock, newClient));

                // 로그를 찍는다.
                char strBuf[1024];
                inet_ntop(AF_INET, &(clientAddr.sin_addr), strBuf, sizeof(strBuf));
                cout << "New client from " << strBuf << ":" << ntohs(clientAddr.sin_port) << ". "
                    << "Socket: " << activeSock << endl;
            }
        }

        // 오류 이벤트가 발생하는 소켓의 클라이언트는 제거한다.
        // activeClients 를 순회하는 동안 그 내용을 변경하면 안되니 지우는 경우를 위해 별도로 list 를 쓴다.
        list<SOCKET> toDelete;
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;

            if (FD_ISSET(activeSock, &exceptionSet)) {
                cerr << "Exception on socket " << activeSock << endl;

                // 소켓을 닫는다.
                closesocket(activeSock);

                // 지울 대상에 포함시킨다.
                // 여기서 activeClients 에서 바로 지우지 않는 이유는 현재 activeClients 를 순회중이기 때문이다.
                toDelete.push_back(activeSock);

                // 소켓을 닫은 경우 더 이상 처리할 필요가 없으니 아래 read 작업은 하지 않는다.
                continue;
            }

            // 읽기 이벤트가 발생하는 소켓의 경우 recv() 를 처리한다.
            // 주의: 아래는 여전히 recv() 에 의해 blocking 이 발생할 수 있다.
            //       우리는 이를 producer-consumer 형태로 바꿀 것이다.
            if (FD_ISSET(activeSock, &readSet)) {
                // 이제 다시 select 대상이 되지 않도록 client 의 flag 를 켜준다.
                client->doingRecv.store(true);

                // 해당 client 를 job queue 에 넣자. lock_guard 를 써도 되고 unique_lock 을 써도 된다.
                // lock 걸리는 범위를 명시적으로 제어하기 위해서 새로 scope 을 열어준다.
                {
                    lock_guard<mutex> lg(jobQueueMutex);

                    bool wasEmpty = jobQueue.empty();
                    jobQueue.push(client);

                    // 그리고 worker thread 를 깨워준다.
                    // 무조건 condition variable 을 notify 해도 되는데,
                    // 해당 condition variable 은 queue 에 뭔가가 들어가서 더 이상 빈 큐가 아닐 때 쓰이므로
                    // 여기서는 무의미하게 CV 를 notify하지 않도록 큐의 길이가 0에서 1이 되는 순간 notify 를 하도록 하자.
                    if (wasEmpty) {
                        jobQueueFilledCv.notify_one();
                    }

                    // lock_guard 는 scope 이 벗어날 때 풀릴 것이다.
                }
            }
        }

        // 이제 지울 것이 있었다면 지운다.
        for (auto& closedSock : toDelete) {

            // 맵에서 지우고 객체도 지워준다.
            // shared_ptr 을 썼기 때문에 맵에서 지워서 더 이상 사용하는 곳이 없어지면 객체도 지워진다.
            activeClients.erase(closedSock);
            activePlayers.erase(closedSock);
        }
    }

    // 이제 threads 들을 join 한다.
    for (shared_ptr<thread>& workerThread : threads) {
        workerThread->join();
    }

    // 연결을 기다리는 passive socket 을 닫는다.
    r = closesocket(passiveSock);
    if (r == SOCKET_ERROR) {
        cerr << "closesocket(passive) failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();

    redisFree(redisConn);
    return 0;
}

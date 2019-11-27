#include "server.h"
#include <netinet/in.h>
#include <cstring>
#include "md5.h"

Server::Server(unsigned short port, unsigned short proxyPort)
    : m_serverSocketFd(-1), m_serverPort(port), m_proxyPort(proxyPort), m_pLogger(nullptr)
{
    initServer();
}

Server::~Server()
{
    printf("~~~~~~~gg~~~~~~~~\n");
    if (m_serverSocketFd != -1)
    {
        close(m_serverSocketFd);
    }
    if (m_proxySocketFd != -1)
    {
        close(m_proxySocketFd);
    }

    std::vector<int> clients;
    for (const auto &it : m_mapClients)
    {
        clients.push_back(it.first);
    }
    for (const auto &c : clients)
    {
        deleteClient(c);
    }

    m_pLogger->info("exit server...");
}

int Server::listenControl()
{
    m_serverSocketFd = tnet::tcp_socket();
    if (m_serverSocketFd == NET_ERR)
    {
        printf("make server socker err!\n");
        m_pLogger->err("make server socker err!");
        return -1;
    }

    int ret = tnet::tcp_listen(m_serverSocketFd, m_serverPort);
    if (ret == NET_ERR)
    {
        printf("server listen err!\n");
        m_pLogger->err("server listen err!");
        return -1;
    }

    tnet::non_block(m_serverSocketFd);
    m_reactor.registFileEvent(
        m_serverSocketFd,
        EVENT_READABLE,
        std::bind(
            &Server::serverAcceptProc,
            this,
            std::placeholders::_1,
            std::placeholders::_2
        )
    );

    return 0;
}

int Server::listenProxy()
{
    m_proxySocketFd = tnet::tcp_socket();
    if (m_proxySocketFd == NET_ERR)
    {
        printf("make proxy socker err!\n");
        m_pLogger->err("make proxy socker err!");
        return -1;
    }

    int ret = tnet::tcp_listen(m_proxySocketFd, m_proxyPort);
    if (ret == NET_ERR)
    {
        printf("server listen err!\n");
        m_pLogger->err("server listen err!");
        return -1;
    }

    m_reactor.registFileEvent(
        m_proxySocketFd, EVENT_READABLE,
        std::bind(
            &Server::proxyAcceptProc,
            this,
            std::placeholders::_1,
            std::placeholders::_2
        )
    );

    return 0;
}

void Server::initServer()
{
    int ret = listenControl();
    if (ret == -1)
    {
        exit(-1);
    }
    ret = listenProxy();
    if (ret == -1)
    {
        exit(-1);
    }
    m_heartbeatTimerId = m_reactor.registTimeEvent(
        HEARTBEAT_INTERVAL_MS,
        std::bind(
            &Server::checkHeartbeatTimerProc,
            this,
            std::placeholders::_1
        )
    );
}

void Server::serverAcceptProc(int fd, int mask)
{
    if (mask & EVENT_READABLE)
    {
        char ip[INET_ADDRSTRLEN];
        int port;
        int connfd = tnet::tcp_accept(fd, ip, INET_ADDRSTRLEN, &port);
        if (connfd == -1)
        {
            if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
            {
                printf("serverAcceptProc accept err: %d\n", errno);
                m_pLogger->err("serverAcceptProc accept err: %d", errno);
            }
            return;
        }
        printf("serverAcceptProc new conn from %s:%d\n", ip, port);
        m_pLogger->info("new client connection from %s:%d", ip, port);

        m_mapClients[connfd] = ClientInfo();
        tnet::non_block(connfd);
        m_reactor.registFileEvent(
            connfd, 
            EVENT_READABLE,
            std::bind(
                &Server::clientAuthProc,
                this,
                std::placeholders::_1, 
                std::placeholders::_2
            )
        );
    }
}

// ---------------------------------
void Server::clientSafeRecv(int cfd, std::function<void(int cfd, size_t dataSize)> callback)
{
    int ret;
    // there is not header init if data len is 0
    size_t targetSize = m_mapClients[cfd].header.ensureTargetDataSize();

    ret = recv(cfd, m_mapClients[cfd].recvBuf + m_mapClients[cfd].recvNum,
                targetSize - m_mapClients[cfd].recvNum, MSG_DONTWAIT);
    
    if (ret == -1)
    {
        if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
        {
            printf("recv client data err: %d\n", errno);
            m_pLogger->err("recv client data err: %d\n", errno);
        }
        return;
    }
    else if (ret == 0)
    {
        deleteClient(cfd);
    }
    else if (ret > 0)
    {
        m_mapClients[cfd].recvNum += ret;

        if (m_mapClients[cfd].recvNum == targetSize)
        {
            m_mapClients[cfd].recvNum = 0;

            // targetSize = header size or data size
            if (targetSize == sizeof(DataHeader))
            {
                memcpy(&m_mapClients[cfd].header, m_mapClients[cfd].recvBuf, targetSize);
            }
            else
            {
                uint32_t realDataSize = m_pCryptor->decrypt(
                    m_mapClients[cfd].header.iv, 
                    (uint8_t*)m_mapClients[cfd].recvBuf, 
                    targetSize
                );

                // if recv all done, we callback
                callback(cfd, realDataSize);

                // remember init datalen for next recv
                m_mapClients[cfd].header.dataLen = 0;
            }
        }
    }
}

// befor use this method, ensure you have filled the buf
void Server::clitneSafeSend(int cfd, std::function<void(int cfd)> callback)
{
    int ret = send(cfd, &m_mapClients[cfd].sendBuf, m_mapClients[cfd].sendSize, MSG_DONTWAIT);

    if (ret == -1)
    {
        if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
        {
            printf("clitneSafeSend err: %d\n", errno);
            m_pLogger->err("clitneSafeSend err: %d\n", errno);
            deleteClient(cfd);
        }
    }
    else if (ret > 0)
    {
        m_mapClients[cfd].sendSize -= ret;

        if (m_mapClients[cfd].sendSize == 0)
        {
            callback(cfd);
        }
        else
        {
            memmove(
                m_mapClients[cfd].sendBuf, 
                m_mapClients[cfd].sendBuf + ret, 
                m_mapClients[cfd].sendSize
            );
        }
    }
}
// -----------------------------

void Server::clientAuthProc(int cfd, int mask)
{
    if (!(mask & EVENT_READABLE))
    {
        return;
    }

    clientSafeRecv(
        cfd, 
        std::bind(
            &Server::checkClientAuthResult, 
            this, 
            std::placeholders::_1, 
            std::placeholders::_2
        )
    );
}

void Server::checkClientAuthResult(int cfd, size_t dataSize)
{
    if (dataSize != sizeof(m_serverPassword))
    {
        printf(
            "encrpt ClientAuthResult data len not good! expect: %lu, infact: %lu\n", 
            sizeof(m_serverPassword), dataSize
        );
        return;
    }

    if (strncmp(m_serverPassword, m_mapClients[cfd].recvBuf, sizeof(m_serverPassword)) == 0)
    {
        processClientAuthResult(cfd, true);
    }
    else
    {
        processClientAuthResult(cfd, false);
    }
}

void Server::processClientAuthResult(int cfd, bool isGood)
{
    if (isGood)
    {
        m_mapClients[cfd].status = CLIENT_STATUS_PW_OK;
    }
    else
    {
        m_mapClients[cfd].status = CLIENT_STATUS_PW_WRONG;
    }

    m_mapClients[cfd].sendSize = MsgUtil::packCryptedData(
        m_pCryptor, 
        (uint8_t*)m_mapClients[cfd].sendBuf, 
        (uint8_t*)AUTH_TOKEN,
        sizeof(AUTH_TOKEN)
    );

    m_reactor.registFileEvent(
        cfd, 
        EVENT_WRITABLE,
        std::bind(
            &Server::replyClientAuthProc,
            this, 
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

void Server::replyClientAuthProc(int cfd, int mask)
{
    if (!(mask & EVENT_WRITABLE))
    {
        return;
    }

    clitneSafeSend(
        cfd,
        std::bind(
            &Server::onReplyClientAuthDone, 
            this, std::placeholders::_1
        )
    );
}

void Server::onReplyClientAuthDone(int cfd)
{
    if (m_mapClients[cfd].status == CLIENT_STATUS_PW_OK)
    {
        m_reactor.removeFileEvent(cfd, EVENT_WRITABLE);
        m_reactor.registFileEvent(
            cfd, 
            EVENT_READABLE,
            std::bind(
                &Server::recvClientProxyPortsProc,
                this, 
                std::placeholders::_1, 
                std::placeholders::_2
            )
        );
    }
    else if(m_mapClients[cfd].status == CLIENT_STATUS_PW_WRONG)
    {
        printf("pw not good, delete client...\n");
        m_pLogger->info("password not good, delete client...");
        
        deleteClient(cfd);
    }
}

void Server::recvClientProxyPortsProc(int cfd, int mask)
{
    if (!(mask & EVENT_READABLE))
    {
        return;
    }

    clientSafeRecv(
        cfd, 
        std::bind(
            &Server::checkClientProxyPortsResult, 
            this, 
            std::placeholders::_1, 
            std::placeholders::_2
        )
    );
}

void Server::checkClientProxyPortsResult(int cfd, size_t dataSize)
{
    unsigned short portNum = 0;

    // first 2bytes is the port number
    memcpy(&portNum, m_mapClients[cfd].recvBuf, sizeof(portNum));
    if (portNum <= 0)
    {
        deleteClient(cfd);
        return;
    }

    size_t portDataSize = portNum * sizeof(unsigned short);
    if (dataSize != portDataSize + sizeof(portNum))
    {
        printf(
            "encrpt ClientProxyPortsResult data len not good! expect: %lu, infact: %lu\n", 
            portDataSize + sizeof(portNum), dataSize
        );
        return;
    }

    // alloc mem
    m_mapClients[cfd].remotePorts.resize(portNum);
    memcpy(
        &m_mapClients[cfd].remotePorts[0], 
        m_mapClients[cfd].recvBuf + sizeof(portNum), 
        portDataSize
    );
    initClient(cfd);
}

void Server::initClient(int fd)
{
    listenRemotePort(fd);
    updateClientHeartbeat(fd);

    m_reactor.registFileEvent(fd, EVENT_READABLE,
                              std::bind(&Server::recvClientDataProc,
                                        this, std::placeholders::_1, std::placeholders::_2));
}

int Server::listenRemotePort(int cfd)
{
    size_t len = m_mapClients[cfd].remotePorts.size();
    int num = 0;
    for (int i = 0; i < len; i++)
    {
        int fd = tnet::tcp_socket();
        if (fd == -1)
        {
            printf("listenRemotePort make socket err: %d\n", errno);
            m_pLogger->err("listenRemotePort make socket err: %d", errno);
            continue;
        }
        unsigned short port = m_mapClients[cfd].remotePorts[i];
        int ret = tnet::tcp_listen(fd, port);
        if (ret == -1)
        {
            printf("listenRemotePort listen port:%d err: %d\n", port, errno);
            m_pLogger->err("listenRemotePort listen port:%d err: %d", port, errno);
            continue;
        }
        num++;
        ListenInfo linfo;
        linfo.port = port;
        linfo.clientFd = cfd;
        m_mapListen[fd] = linfo;
        tnet::non_block(fd);
        m_reactor.registFileEvent(fd, EVENT_READABLE,
                                  std::bind(&Server::userAcceptProc,
                                            this, std::placeholders::_1, std::placeholders::_2));
        printf("listenRemotePort listening port: %d\n", port);
        m_pLogger->info("listenRemotePort listening port: %d", port);
    }
    return num;
}

void Server::userAcceptProc(int fd, int mask)
{
    if (mask & EVENT_READABLE)
    {
        char ip[INET_ADDRSTRLEN];
        int port;
        int connfd = tnet::tcp_accept(fd, ip, INET_ADDRSTRLEN, &port);
        if (connfd == -1)
        {
            if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
            {
                printf("userAcceptProc accept err: %d\n", errno);
                m_pLogger->err("userAcceptProc accept err: %d", errno);
            }
            return;
        }
        printf("userAcceptProc new conn from %s:%d\n", ip, port);
        m_pLogger->info("new user connection from %s:%d", ip, port);
        UserInfo info;
        info.port = m_mapListen[fd].port;
        m_mapUsers[connfd] = info;
        tnet::non_block(connfd);
        sendClientNewProxy(m_mapListen[fd].clientFd, connfd, m_mapListen[fd].port);
    }
}

void Server::sendClientNewProxy(int cfd, int ufd, unsigned short remotePort)
{
    MsgData msgData;
    NewProxyMsg newProxyMsg;

    newProxyMsg.UserId = ufd;
    newProxyMsg.rmeotePort = remotePort;

    msgData.type = MSGTYPE_NEW_PROXY;
    msgData.size = sizeof(newProxyMsg);
    size_t bufSize = sizeof(msgData) + sizeof(newProxyMsg);
    char buf[bufSize];

    memcpy(buf, &msgData, sizeof(msgData));
    memcpy(buf + sizeof(msgData), &newProxyMsg, sizeof(newProxyMsg));

    m_mapClients[cfd].sendSize = MsgUtil::packCryptedData(
        m_pCryptor, 
        (uint8_t*)m_mapClients[cfd].sendBuf, 
        (uint8_t*)buf,
        bufSize
    );

    m_reactor.registFileEvent(
        cfd,
        EVENT_WRITABLE,
        std::bind(
            &Server::sendClientNewProxyProc,
            this, 
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

void Server::sendClientNewProxyProc(int cfd, int mask)
{
    if (!(mask & EVENT_WRITABLE))
    {
        return;
    }

    clitneSafeSend(
        cfd,
        std::bind(
            &Server::onSendClientNewProxyDone, 
            this, std::placeholders::_1
        )
    );
}

void Server::onSendClientNewProxyDone(int cfd)
{
    m_reactor.removeFileEvent(cfd, EVENT_WRITABLE);
}

void Server::recvClientDataProc(int cfd, int mask)
{
    if (!(mask & EVENT_READABLE))
    {
        return;
    }

    clientSafeRecv(
        cfd, 
        std::bind(
            &Server::processClientBuf, 
            this, 
            std::placeholders::_1, 
            std::placeholders::_2
        )
    );
}

void Server::processClientBuf(int cfd, size_t dataSize)
{
    MsgData msgData;

    memcpy(&msgData, m_mapClients[cfd].recvBuf, sizeof(MsgData));

    if (msgData.type == MSGTYPE_HEARTBEAT)
    {
        if (memcmp(m_mapClients[cfd].recvBuf + sizeof(MsgData), 
                    HEARTBEAT_CLIENT_MSG, msgData.size) == 0)
        {
            updateClientHeartbeat(cfd);
            sendHeartbeat(cfd);
        }
    }
    else if (msgData.type == MSGTYPE_REPLY_NEW_PROXY)
    {
        ReplyNewProxyMsg rnpm;
        memcpy(&rnpm, m_mapClients[cfd].recvBuf, msgData.size);

        processNewProxy(rnpm);
    }
}

void Server::sendHeartbeat(int cfd)
{
    MsgData heartData;
    heartData.type = MSGTYPE_HEARTBEAT;
    heartData.size = strlen(HEARTBEAT_SERVER_MSG);

    size_t dataSize = sizeof(heartData) + strlen(HEARTBEAT_SERVER_MSG);
    char bufData[dataSize];

    memcpy(bufData, &heartData, sizeof(heartData));
    memcpy(bufData + sizeof(heartData), HEARTBEAT_SERVER_MSG, strlen(HEARTBEAT_SERVER_MSG));

    uint8_t buf[MsgUtil::ensureCryptedDataSize(dataSize)];
    uint32_t cryptedDataLen = MsgUtil::packCryptedData(m_pCryptor, buf, (uint8_t*)bufData, dataSize);

    int ret = send(cfd, buf, cryptedDataLen, MSG_DONTWAIT);
    if (ret == -1)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            printf("send heartbeat err: %d\n", errno);
            m_pLogger->err("send heartbeat err: %d", errno);
        }
    }
    else
    {
        if (ret == cryptedDataLen)
        {
            // printf("send to client: %d heartbeat success!\n", cfd);
        }
        else
        {
            printf("send to client: %d heartbeat not good!\n", cfd);
            m_pLogger->warn("send to client: %d heartbeat not good!", cfd);
        }
    }
}

void Server::updateClientHeartbeat(int cfd)
{
    long now_sec, now_ms;
    getTime(&now_sec, &now_ms);
    m_mapClients[cfd].lastHeartbeat = now_sec * 1000 + now_ms;
}

int Server::checkHeartbeatTimerProc(long long id)
{
    std::vector<int> timeoutClients;
    for (const auto &it : m_mapClients)
    {
        if (it.second.lastHeartbeat != -1)
        {
            long now_sec, now_ms;
            long long nowTimeStamp;
            getTime(&now_sec, &now_ms);
            nowTimeStamp = now_sec * 1000 + now_ms;
            long subTimeStamp = nowTimeStamp - it.second.lastHeartbeat;
            // printf("check timeout: %ld\n", subTimeStamp);
            if (subTimeStamp > DEFAULT_SERVER_TIMEOUT_MS)
            {
                // delete
                timeoutClients.push_back(it.first);
            }
        }
    }
    for (const auto &it : timeoutClients)
    {
        printf("client %d is timeout\n", it);
        m_pLogger->info("client %d is timeout", it);
        deleteClient(it);
    }
    return HEARTBEAT_INTERVAL_MS;
}

void Server::processNewProxy(ReplyNewProxyMsg rnpm)
{
    if (rnpm.IsSuccess)
    {
        printf("make proxy tunnel success\n");
        m_pLogger->info("make proxy tunnel success");
    }
    else
    {
        printf("make proxy tunnel fail\n");
        m_pLogger->info("make proxy tunnel fail");
        deleteUser(rnpm.UserId);
    }
}

void Server::proxyAcceptProc(int fd, int mask)
{
    if (mask & EVENT_READABLE)
    {
        char ip[INET_ADDRSTRLEN];
        int port;
        int connfd = tnet::tcp_accept(fd, ip, INET_ADDRSTRLEN, &port);
        if (connfd == -1)
        {
            if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
            {
                printf("proxyAcceptProc accept err: %d\n", errno);
                m_pLogger->err("proxyAcceptProc accept err: %d", errno);
            }
            return;
        }
        printf("proxyAcceptProc new conn from %s:%d\n", ip, port);
        m_pLogger->info("new proxy connection from %s:%d", ip, port);
        ProxyConnInfo pci;
        pci.recvNum = 0;
        pci.recvSize = sizeof(int);
        m_mapProxy[connfd] = pci;
        tnet::non_block(connfd);
        m_reactor.registFileEvent(connfd, EVENT_READABLE,
                                  std::bind(&Server::proxyReadUserInfoProc,
                                            this, std::placeholders::_1, std::placeholders::_2));
    }
}

void Server::proxySafeRecv(int fd, std::function<void(int fd, size_t dataSize)> callback)
{
    int ret;
    // there is not header init if data len is 0
    size_t targetSize = m_mapProxy[fd].header.ensureTargetDataSize();

    if (m_mapProxy[fd].recvNum >= MAX_BUF_SIZE)
    {
        printf("proxySafeRecv recv buf full!\n");
        m_pLogger->warn("proxySafeRecv recv buf full!");
        return;
    }

    ret = recv(fd, m_mapProxy[fd].recvBuf + m_mapProxy[fd].recvNum,
                targetSize - m_mapProxy[fd].recvNum, MSG_DONTWAIT);
    
    if (ret == -1)
    {
        if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
        {
            printf("proxySafeRecv err: %d\n", errno);
            m_pLogger->err("proxySafeRecv err: %d", errno);
            return;
        }
    }
    else if (ret == 0)
    {
        deleteUser(m_mapProxy[fd].userFd);
        deleteProxyConn(fd);
    }
    else if (ret > 0)
    {
        m_mapProxy[fd].recvNum += ret;

        if (m_mapProxy[fd].recvNum == targetSize)
        {
            m_mapProxy[fd].recvNum = 0;

            // targetSize = header size or data size
            if (targetSize == sizeof(DataHeader))
            {
                memcpy(&m_mapProxy[fd].header, m_mapProxy[fd].recvBuf, targetSize);
            }
            else
            {
                uint32_t realDataSize = m_pCryptor->decrypt(
                    m_mapProxy[fd].header.iv, 
                    (uint8_t*)m_mapProxy[fd].recvBuf, 
                    targetSize
                );

                // if recv all done, we callback
                callback(fd, realDataSize);

                // remember init datalen for next recv
                m_mapProxy[fd].header.dataLen = 0;
            }
        }
    }
}

void Server::proxyReadUserInfoProc(int fd, int mask)
{
    if (!(mask & EVENT_READABLE))
    {
        return;
    }

    proxySafeRecv(
        fd,
        std::bind(
            &Server::onProxyReadUserInfoDone,
            this,
            std::placeholders::_1, 
            std::placeholders::_2
        )
    );
}

void Server::onProxyReadUserInfoDone(int fd, size_t dataSize)
{
    int userFd;

    if (dataSize != sizeof(userFd))
    {
        printf(
            "encrpt onProxyReadUserInfoDone data len not good! expect: %lu, infact: %lu\n", 
            sizeof(m_serverPassword), dataSize
        );
        return;
    }

    memcpy(&userFd, m_mapProxy[fd].recvBuf, dataSize);

    if (m_mapUsers.find(userFd) != m_mapUsers.end())
    {
        m_mapProxy[fd].userFd = userFd;
        m_mapUsers[userFd].proxyFd = fd;

        m_reactor.registFileEvent(fd, EVENT_READABLE,
                                    std::bind(&Server::proxyReadDataProc,
                                            this, std::placeholders::_1, std::placeholders::_2));
        m_reactor.registFileEvent(userFd, EVENT_READABLE,
                                    std::bind(&Server::userReadDataProc,
                                            this, std::placeholders::_1, std::placeholders::_2));
        
        printf("start new proxy..., %d<--->%d\n", userFd, fd);
        m_pLogger->info("start new proxy..., %d<--->%d", userFd, fd);
    }
    else
    {
        // delete proxy
        deleteProxyConn(fd);
    }
}

void Server::deleteProxyConn(int fd)
{
    m_mapProxy.erase(fd);
    close(fd);
    m_reactor.removeFileEvent(fd, EVENT_READABLE | EVENT_WRITABLE);
    printf("deleted proxy conn: %d\n", fd);
    m_pLogger->info("deleted proxy conn: %d", fd);
}


void Server::proxyReadDataProc(int fd, int mask)
{
    if (!(mask & EVENT_READABLE))
    {
        return;
    }

    proxySafeRecv(
        fd,
        std::bind(
            &Server::onProxyReadDataDone,
            this,
            std::placeholders::_1, 
            std::placeholders::_2
        )
    );
}

void Server::onProxyReadDataDone(int fd, size_t dataSize)
{
    int userFd = m_mapProxy[fd].userFd;

    if (m_mapUsers[userFd].sendSize + dataSize >= sizeof(m_mapUsers[userFd].sendBuf))
    {
        printf("user send buf full\n");
        m_pLogger->warn("user send buf full");
        return;
    }

    memcpy(
        m_mapUsers[userFd].sendBuf + m_mapUsers[userFd].sendSize, 
        m_mapProxy[fd].recvBuf, 
        dataSize
    );
    m_mapUsers[userFd].sendSize += dataSize;

    // duplicated regist is fine
    m_reactor.registFileEvent(
        userFd,
        EVENT_WRITABLE,
        std::bind(
            &Server::userWriteDataProc,
            this, 
            std::placeholders::_1, 
            std::placeholders::_2
        )
    );
}

/*
 * 发送缓冲区的数据给user
 */
void Server::userWriteDataProc(int fd, int mask)
{
    printf("on userWriteDataProc\n");
    int numSend = send(fd, m_mapUsers[fd].sendBuf, m_mapUsers[fd].sendSize, MSG_DONTWAIT);
    if (numSend > 0)
    {
        if (numSend == m_mapUsers[fd].sendSize)
        {
            m_reactor.removeFileEvent(fd, EVENT_WRITABLE);
            // 缓冲区已经全部发送了，从开始放数据
            m_mapUsers[fd].sendSize = 0;
            printf("userWriteDataProc: send all data: %d\n", numSend);
        }
        else
        {
            // 没有全部发送完，把没发送的数据移动到前面
            size_t newSize = m_mapUsers[fd].sendSize - numSend; // 还剩多少没发送完
            m_mapUsers[fd].sendSize = newSize;
            memmove(m_mapUsers[fd].sendBuf, m_mapUsers[fd].sendBuf + numSend, newSize);
            printf("userWriteDataProc: send partial data: %d, left:%lu\n", numSend, newSize);
        }
    }
    else
    {
        if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
        {
            printf("userWriteDataProc send err:%d\n", errno);
            m_pLogger->err("userWriteDataProc send err:%d", errno);
        }
    }
}

/*
 * recv user data, and send to proxy tunnel with encrypted
*/
void Server::userReadDataProc(int fd, int mask)
{
    printf("on userReadDataProc\n");
    int proxyFd = m_mapUsers[fd].proxyFd;

    if (m_mapProxy[proxyFd].sendSize >= MAX_BUF_SIZE)
    {
        printf("proxy send buf full\n");
        return;
    }

    int numRecv = recv(fd, m_mapProxy[proxyFd].sendBuf + m_mapProxy[proxyFd].sendSize,
                       MAX_BUF_SIZE - m_mapProxy[proxyFd].sendSize, MSG_DONTWAIT);
    if (numRecv == -1)
    {
        if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
        {
            printf("userReadDataProc recv err: %d\n", errno);
            m_pLogger->err("userReadDataProc recv err: %d\n", errno);
            return;
        }
    }
    else if (numRecv == 0)
    {
        deleteUser(fd);
        deleteProxyConn(proxyFd);
    }
    else if (numRecv > 0)
    {
        m_mapProxy[proxyFd].sendSize += MsgUtil::packCryptedData(
            m_pCryptor,
            (uint8_t*)m_mapProxy[proxyFd].sendBuf + m_mapProxy[proxyFd].sendSize,
            (uint8_t*)m_mapProxy[proxyFd].sendBuf + m_mapProxy[proxyFd].sendSize,
            numRecv
        );

        m_reactor.registFileEvent(
            proxyFd, 
            EVENT_WRITABLE,
            std::bind(
                &Server::proxyWriteDataProc,
                this, 
                std::placeholders::_1, 
                std::placeholders::_2
            )
        );
        printf("userReadDataProc: recv from user: %d, proxy snedSize: %lu\n", numRecv, m_mapProxy[proxyFd].sendSize);
    }
}

/*
 * 发送缓冲区的数据给代理通道
*/
void Server::proxyWriteDataProc(int fd, int mask)
{
    printf("on proxyWriteDataProc\n");
    int numSend = send(fd, m_mapProxy[fd].sendBuf, m_mapProxy[fd].sendSize, MSG_DONTWAIT);
    if (numSend > 0)
    {
        if (numSend == m_mapProxy[fd].sendSize)
        {
            m_reactor.removeFileEvent(fd, EVENT_WRITABLE);
            m_mapProxy[fd].sendSize = 0;
            printf("proxyWriteDataProc: send all data: %d\n", numSend);
        }
        else
        {
            size_t newSize = m_mapProxy[fd].sendSize - numSend;
            m_mapProxy[fd].sendSize = newSize;
            memmove(m_mapProxy[fd].sendBuf, m_mapProxy[fd].sendBuf + numSend, newSize);
            printf("proxyWriteDataProc: send partial data: %d, left:%lu\n", numSend, newSize);
        }
    }
    else
    {
        if (errno != EAGAIN && EAGAIN != EWOULDBLOCK)
        {
            printf("proxyWriteDataProc send err:%d\n", errno);
            m_pLogger->err("proxyWriteDataProc send err:%d", errno);
        }
    }
}

void Server::deleteUser(int fd)
{
    m_mapUsers.erase(fd);
    close(fd);
    m_reactor.removeFileEvent(fd, EVENT_WRITABLE | EVENT_READABLE);
    printf("deleted user:%d\n", fd);
    m_pLogger->info("deleted user:%d", fd);
}

void Server::initCryptor()
{
    m_pCryptor = new Cryptor(CRYPT_CBC, (uint8_t*)m_serverPassword);
    if(m_pCryptor == nullptr)
    {
        m_pLogger->err("new Cryptor object error");
        exit(-1);
    }
}

void Server::setPassword(const char *password)
{
    if (password != NULL)
    {
        strncpy(m_serverPassword, MD5(password).toStr().c_str(), sizeof(m_serverPassword)); // md5加密
    }

    initCryptor();
}

void Server::deleteClient(int fd)
{
    printf("client gone!\n");
    m_pLogger->info("client gone!");
    m_reactor.removeFileEvent(fd, EVENT_READABLE | EVENT_WRITABLE);
    m_mapClients.erase(fd);
    close(fd);
    // 需要加快效率，不应每次遍历,注意删除顺序,proxy -> user -> remotelisten
    // 删除相关的proxy连接
    for (auto it = m_mapProxy.begin(); it != m_mapProxy.end();)
    {
        int ufd, cfd;
        ufd = it->second.userFd;
        cfd = findClientfdByPort(m_mapUsers[ufd].port);
        if (cfd == fd)
        {
            int pfd = it->first;
            m_reactor.removeFileEvent(pfd, EVENT_READABLE | EVENT_WRITABLE);
            close(pfd);
            it = m_mapProxy.erase(it);
            printf("delete proxy conn with this client! %d\n", pfd);
            m_pLogger->info("delete proxy conn with this client! %d", pfd);
        }
        else
        {
            it++;
        }
    }
    // 删除相关的user
    for (auto it = m_mapUsers.begin(); it != m_mapUsers.end();)
    {
        int cfd = findClientfdByPort(it->second.port);
        if (cfd == fd)
        {
            int ufd = it->first;
            m_reactor.removeFileEvent(ufd, EVENT_READABLE | EVENT_WRITABLE);
            close(ufd);
            it = m_mapUsers.erase(it);
            printf("delete user conn with this client! %d\n", ufd);
            m_pLogger->info("delete user conn with this client! %d", ufd);
        }
        else
        {
            it++;
        }
    }
    // 删除此客户端对应的公网监听的端口相关的资源
    for (auto it = m_mapListen.begin(); it != m_mapListen.end();)
    {
        if (it->second.clientFd == fd)
        {
            int remoteListenFd = it->first;
            m_reactor.removeFileEvent(remoteListenFd, EVENT_READABLE | EVENT_WRITABLE);
            close(remoteListenFd);
            it = m_mapListen.erase(it);
            printf("delete remote listen fd with this client! %d\n", remoteListenFd);
            m_pLogger->info("delete remote listen fd with this client! %d\n", remoteListenFd);
        }
        else
        {
            it++;
        }
    }
}

int Server::findClientfdByPort(unsigned short port)
{
    for (const auto &it : m_mapListen)
    {
        if (it.second.port == port)
        {
            return it.second.clientFd;
        }
    }
    return -1;
}

void Server::setLogger(Logger *logger)
{
    if (logger == nullptr)
    {
        return;
    }
    m_pLogger = logger;
}

void Server::startEventLoop()
{
    m_pLogger->info("server running...");
    m_reactor.eventLoop(EVENT_LOOP_FILE_EVENT | EVENT_LOOP_TIMER_EVENT);
}
#ifndef SDK_BUILD_PROSPERO
#include <enet/enet.h>
#endif
#include <nitro.h>
#include <simulator/sim.h>
#include <simulator/config/sim_config.h>
#include <simulator/sim_carts.h>
#include <simulator/sim_net.h>
#include <simulator/wmsp_indicate.h>

#ifdef __WIN32__
    #include <winsock2.h>
    #include <ws2tcpip.h>

    #define socket_t    SOCKET
    #define sockaddr_t  SOCKADDR
    #define sockaddr_in_t  SOCKADDR_IN
#else
    #include <unistd.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <fcntl.h>

    #define socket_t    int
    #define sockaddr_t  struct sockaddr
    #define sockaddr_in_t  struct sockaddr_in
    #define closesocket close
#endif

#ifndef INVALID_SOCKET
    #define INVALID_SOCKET  (socket_t)-1
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

#ifdef SDK_BUILD_ANDROID
#include <SDL2/SDL_system.h>
#include <jni.h>
#endif

enum {
    SIM_NET_CHANNEL_MGMT, // Control, commands, etc
    SIM_NET_CHANNEL_APPDATA, // NDS application data
};

static SIM_Net_t net = {};

// enet isn't thread-safe: the net thread services the host while the game's thread sends
// (MP data, player info). Everything that touches net.enetHost or its peers holds this
// (an SDL mutex, which may be locked again by the thread holding it).
static SDL_mutex *sEnetLock = NULL;

#define ENET_LOCK() SDL_LockMutex(sEnetLock)
#define ENET_UNLOCK() SDL_UnlockMutex(sEnetLock)

// Joining in the background (SIM_Net_StartClientAsync)
static SDL_atomic_t sConnecting;
static char sConnectAddress[64];
static BOOL sConnectFailed = FALSE;
static BOOL sHostLost = FALSE;
static BOOL sJoinRefused = FALSE;

// This device's MP links by AID: the peer that AID's data goes to. Set when a parent takes a
// child (its AID) and when a child is taken (AID 0, its parent). The player list (which
// the host rebroadcasts and may be a moment behind) only finds parents to join.
static ENetPeer * sMPPeer[16];

static void ClearMPLinks(void)
{
    memset(sMPPeer, 0, sizeof(sMPPeer));
}

// The other device went away (closed, out of reach): as a DS losing the link, the
// wireless code hears that the child left (a parent) or the parent did (a child)
static void LinkLost(ENetPeer * peer)
{
    for(int aid = 0; aid < 16; aid++) {
        if(sMPPeer[aid] != peer) {
            continue;
        }
        ENET_LOCK();
        sMPPeer[aid] = NULL;
        ENET_UNLOCK();
        if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT && aid != 0) {
            net.aidBitmask &= ~(1 << aid);
            SIM_WMSP_Indicate_t indicate = {};
            indicate.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
            indicate.aid = aid;
            WMSP_Indicate(&indicate);
        } else if(net.curPlayer.mpState == SIM_NET_MPSTATE_CHILD && aid == 0) {
            net.curPlayer.aid = 0;
            net.curPlayer.mpState = SIM_NET_MPSTATE_OFF;
            SIM_WMSP_Indicate_t indicate = {};
            indicate.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
            indicate.aid = 0;
            WMSP_Indicate(&indicate);
        }
    }
}

static void ProcessMPEvent(ENetEvent * event);

// SIM_NET_LOG=1 in the environment: every MP data packet in the log
static BOOL LogData(void)
{
    static int sLog = -1;
    if(sLog < 0) {
        const char * value = getenv("SIM_NET_LOG");
        sLog = value != NULL && value[0] == '1';
    }
    return sLog;
}
static void DiscoveryUpdate(void);

static int NetThread(void * arg);

static void InitMyPlayer();
static void SendMyPlayer();

static void InitPacketHeader(SIM_Net_PacketHeader_t * header);
static BOOL CheckPacketHeader(SIM_Net_PacketHeader_t * header);

#ifndef SDK_BUILD_PROSPERO
static void ProcessHostEvent(ENetEvent * event);
static void ProcessClientEvent(ENetEvent * event);

static void ProcessMPParentEvent(ENetEvent * event);
static void ProcessMPChildEvent(ENetEvent * event);

static void ReceiveMPData(ENetEvent * event);
#endif

static void HostUpdatePlayerList();

void SIM_Net_Init()
{
    if(net.state != SIM_NET_STATE_UNINITIALIZED) {
        SIM_assert_always_msg("Net is already initialized!");
        return;
    }

    net.playerListMutex = SDL_CreateMutex();
    net.joinReqSemaphore = SDL_CreateSemaphore(0);
    sEnetLock = SDL_CreateMutex();

    net.state = SIM_NET_STATE_DISCONNECTED;
}

void SIM_Net_Process()
{
    #ifndef SDK_BUILD_PROSPERO
    // TODO: add PS5 support for ENet
    DiscoveryUpdate();

    // The lock is only held for enet itself: handling an event may hand data to the
    // wireless code, which may wait for the game's side, which may be sending
    ENetEvent event;
    while(TRUE) {
        ENET_LOCK();
        int got = net.enetHost != NULL ? enet_host_service(net.enetHost, &event, 0) : 0;
        SIM_Net_State_t state = net.state;
        ENET_UNLOCK();
        if(got <= 0) {
            break;
        }
        #ifdef SDK_TRACY_ENABLE
        TracyCZone(ctx, 1);
        #endif
        if(state == SIM_NET_STATE_CLIENT) {
            ProcessClientEvent(&event);
        } else if(state == SIM_NET_STATE_HOST) {
            ProcessHostEvent(&event);
        }
        if(event.type == ENET_EVENT_TYPE_RECEIVE && event.packet != NULL) {
            // The wireless code reads received data after this returns (through the PXI
            // FIFO), so packets are freed a while later: when the ring comes round
            static ENetPacket * sReceived[256];
            static unsigned sReceivedNext = 0;
            ENET_LOCK();
            if(sReceived[sReceivedNext] != NULL) {
                enet_packet_destroy(sReceived[sReceivedNext]);
            }
            sReceived[sReceivedNext] = event.packet;
            sReceivedNext = (sReceivedNext + 1) % 256;
            ENET_UNLOCK();
        }
        #ifdef SDK_TRACY_ENABLE
        TracyCZoneEnd(ctx);
        #endif
    }
    #endif
}

void SIM_Net_SendBeaconIndication()
{
    // Send Beacon notification to WM
    if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT) {
        WMSP_IndicateSendBeacon();
    }
}

BOOL SIM_Net_StartHost()
{
    #ifdef SDK_BUILD_PROSPERO
    // TODO: add PS5 support for ENet
    return FALSE;
    #else
    if(net.state != SIM_NET_STATE_DISCONNECTED) {
        return FALSE;
    }

    ENetAddress myAddr;
    myAddr.host = ENET_HOST_ANY;
    myAddr.port = SIM_NET_PORT;

    ENET_LOCK();
    net.enetHost = enet_host_create(&myAddr, 16, 2, 0, 0);
    if(net.enetHost == NULL) {
        ENET_UNLOCK();
        return FALSE;
    }
    sHostLost = FALSE;

    InitMyPlayer();

    net.curPlayer.id = 0;
    net.curPlayer.status = SIM_NET_PLAYERSTATUS_HOST;

    memcpy(&net.playerList[0], &net.curPlayer, sizeof(SIM_Net_Player_t));

    net.playerCount = 1;
    net.state = SIM_NET_STATE_HOST;
    ENET_UNLOCK();
    return TRUE;
    #endif
}

BOOL SIM_Net_StartClient(char * ipAddress)
{
    #ifdef SDK_BUILD_PROSPERO
    return FALSE;
    #else
    if(net.state != SIM_NET_STATE_DISCONNECTED) {
        return FALSE;
    }

    // The handshake runs on a host of its own: net.enetHost (which the net thread
    // services) is only set once it's done
    ENetHost * client = enet_host_create(NULL, 16, 2, 0, 0);
    if(client == NULL) {
        return FALSE;
    }

    InitMyPlayer();

    ENetAddress myAddr;
    if(enet_address_set_host(&myAddr, ipAddress) != 0) {
        enet_host_destroy(client);
        return FALSE;
    }
    myAddr.port = SIM_NET_PORT;

    ENetPeer * myPeer = enet_host_connect(client, &myAddr, 2, 0);
    if(myPeer == NULL) {
        enet_host_destroy(client);
        return FALSE;
    }

    int connStatus = 0;

    u64 startTick = SDL_GetTicks64();
    const int timeoutDuration = 5000;
    ENetEvent event;
    while(TRUE) {
        u64 curTick = SDL_GetTicks64();
        if(curTick < startTick) {
            break;
        }

        if(curTick - startTick >= timeoutDuration) {
            break;
        }
        if(enet_host_service(client, &event, 10) > 0) {
            if (connStatus == 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                connStatus = 1;
            } else if(connStatus == 1 && event.type == ENET_EVENT_TYPE_RECEIVE) {
                if(event.channelID != SIM_NET_CHANNEL_MGMT) {
                    continue;
                }
                if(event.packet->dataLength < sizeof(SIM_Net_ClientInitPacket_t)) {
                    continue;
                }
                
                SIM_Net_ClientInitPacket_t *  packetData = (SIM_Net_ClientInitPacket_t *)event.packet->data;
                
                if(!CheckPacketHeader(&packetData->common.header)) {
                    continue;
                }

                if(packetData->common.cmd != SIM_NET_MGMTPACKETCMD_CLIENT_INIT) {
                    continue;
                }

                net.curPlayer.id = packetData->playerId;

                //Send player information to the host
                SIM_Net_PlayerInfoPacket_t playerInfoPacket;
                InitPacketHeader(&playerInfoPacket.common.header);
                playerInfoPacket.common.cmd = SIM_NET_MGMTPACKETCMD_PLAYER_INFO;
                memcpy(&playerInfoPacket.player, &net.curPlayer, sizeof(SIM_Net_Player_t));
                ENetPacket * pkt = enet_packet_create(&playerInfoPacket, sizeof(SIM_Net_PlayerInfoPacket_t), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(event.peer, SIM_NET_CHANNEL_MGMT, pkt);

                connStatus = 2;
                break;
            } else if(event.type == ENET_EVENT_TYPE_DISCONNECT) {
                connStatus = 0;
                break;
            }
        }
    }


    if(connStatus != 2) {
        enet_host_destroy(client);
        return FALSE;
    }

    ENET_LOCK();
    net.enetHost = client;
    net.peerList[0] = myPeer;
    myPeer->data = &net.playerList[0];
    sHostLost = FALSE;
    net.state = SIM_NET_STATE_CLIENT;
    ENET_UNLOCK();
    return TRUE;
    #endif
}

int SIM_Net_GetAID()
{
    return net.curPlayer.aid;
}

int SIM_Net_GetCurrentPlayerID()
{
    return net.curPlayer.id;
}

SIM_Net_State_t SIM_Net_GetCurrentState()
{
    return net.state;
}

int SIM_Net_GetPlayerCount()
{
    return net.playerCount;
}

const SIM_Net_Player_t * SIM_Net_GetPlayerList()
{
    return net.playerList;
}

void SIM_Net_SetGameInfo(int ggid, int tgid, u8 * userGameInfo, int userGameInfoLength)
{
    net.curPlayer.ggid = ggid;
    net.curPlayer.tgid = tgid;
    if(userGameInfo) {
        if(userGameInfoLength > 112) {
            userGameInfoLength = 112;
        }
        memcpy(net.curPlayer.userGameInfo, userGameInfo, userGameInfoLength);
        net.curPlayer.userGameInfoLength = userGameInfoLength;
    }
    SendMyPlayer();
}

void SIM_Net_SetEntry(BOOL entry)
{
    if(net.curPlayer.entryFlag == (entry ? 1 : 0)) {
        return;
    }
    net.curPlayer.entryFlag = entry ? 1 : 0;
    SendMyPlayer();
}

void SIM_Net_SetMPState(SIM_Net_MPState_t state)
{
    if(state == SIM_NET_MPSTATE_OFF || (state == SIM_NET_MPSTATE_PARENT && net.curPlayer.mpState != SIM_NET_MPSTATE_PARENT)) {
        ENET_LOCK();
        ClearMPLinks();
        net.aidBitmask = 0;
        ENET_UNLOCK();
    }
    net.curPlayer.mpState = state;
    SendMyPlayer();
}

BOOL SIM_Net_JoinMP(u8 * bssid, u8 * ssid)
{
    #ifdef SDK_BUILD_PROSPERO
    // TODO: Add PS5 support for ENet
    return FALSE;
    #else
    // Find the connected player with matching mac address (bssid)
    int idNo = -1;
    for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
        SIM_Net_Player_t * player = &net.playerList[i];
        if((player->status == SIM_NET_PLAYERSTATUS_CLIENT || player->status == SIM_NET_PLAYERSTATUS_HOST)
        && (player->mpState == SIM_NET_MPSTATE_PARENT)) {
            // Compare the mac address
            if(memcmp(player->ndsMacAddr, bssid, 6) == 0) {
                idNo = i;
            }
        }
    }

    if(idNo < 0) {
        return FALSE;
    }

    // Joining someone else's group: this device isn't a parent any more (its join
    // response must go to the child side)
    net.curPlayer.mpState = SIM_NET_MPSTATE_OFF;
    while(SDL_SemTryWait(net.joinReqSemaphore) == 0) {
        // a late response to an earlier join
    }
    sJoinRefused = FALSE;

    // Send the join request
    SIM_Net_JoinReqPacket_t myJoinRequest = {};
    InitPacketHeader(&myJoinRequest.common.header);
    myJoinRequest.common.cmd = SIM_NET_MPPACKETCMD_JOIN_REQ;
    myJoinRequest.playerId = net.curPlayer.id;
    memcpy(myJoinRequest.ssid, ssid, WM_SIZE_CHILD_SSID);

    ENET_LOCK();
    if(!net.peerList[idNo]) {
        ENET_UNLOCK();
        return FALSE;
    }
    ENetPacket * pkt = enet_packet_create(&myJoinRequest, sizeof(SIM_Net_JoinReqPacket_t), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(net.peerList[idNo], SIM_NET_CHANNEL_APPDATA, pkt);
    ENET_UNLOCK();

    //Wait for Join Response
    if(SDL_SemWaitTimeout(net.joinReqSemaphore, 5000) != 0) {
        // Timeout
        return FALSE;
    }
    if(sJoinRefused) {
        // The parent wasn't taking children (any more): as a DS whose association fails
        return FALSE;
    }



    return TRUE;
    #endif
}

BOOL SIM_Net_DisconnectMP(u16 aidBitmap, u16 * resultBitmap)
{
    #ifdef SDK_BUILD_PROSPERO
    // TODO: Add ENet support for PS5
    return FALSE;
    #else
    SIM_Net_DisconnectPacket_t disconnectPacket = {};
    InitPacketHeader(&disconnectPacket.common.header);
    disconnectPacket.common.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
    disconnectPacket.sendingAid = net.curPlayer.aid;

    BOOL result = FALSE;
    ENET_LOCK();
    for(int aid = 0; aid < 16; aid++) {
        if(!((1 << aid) & aidBitmap) || sMPPeer[aid] == NULL || aid == net.curPlayer.aid) {
            continue;
        }
        ENetPacket * pkt = enet_packet_create(&disconnectPacket, sizeof(SIM_Net_DisconnectPacket_t), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(sMPPeer[aid], SIM_NET_CHANNEL_APPDATA, pkt);
        sMPPeer[aid] = NULL;
        result = TRUE;
        if(resultBitmap) {
            *resultBitmap = *resultBitmap | (1 << aid);
        }
        if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT) {
            // Free up the AID slot on the parent
            net.aidBitmask &= ~(1 << aid);
        }
    }
    ENET_UNLOCK();

    if(result && net.curPlayer.mpState == SIM_NET_MPSTATE_CHILD) {
        ClearMPLinks();
        net.curPlayer.aid = 0;
        net.curPlayer.mpState = SIM_NET_MPSTATE_OFF;
        SendMyPlayer();
    }

    return TRUE;
    #endif
}

BOOL SIM_Net_SendMPData(u16 aidBitmap, u8 port, u8 * data, int dataLen)
{
    #ifdef SDK_BUILD_PROSPERO
    // TODO: Add ENet support for PS5
    return FALSE;
    #else
    int mpDataPacketSize = dataLen + sizeof(SIM_Net_MpDataPacket_t);
    SIM_Net_MpDataPacket_t * mpDataPacket = (SIM_Net_MpDataPacket_t *)malloc(mpDataPacketSize);
    memset(mpDataPacket, 0, mpDataPacketSize);
    InitPacketHeader(&mpDataPacket->common.header);
    mpDataPacket->common.cmd = SIM_NET_MPPACKETCMD_DATA;
    mpDataPacket->port = port;
    mpDataPacket->dataLen = dataLen;
    mpDataPacket->aid = net.curPlayer.aid;
    memcpy(mpDataPacket->data, data, dataLen);
    ENET_LOCK();
    ENetPacket * pkt = enet_packet_create(mpDataPacket, mpDataPacketSize, ENET_PACKET_FLAG_RELIABLE);

    if(LogData()) {
        printf("[SEND aid=%d bmp=%04x port=%d len=%d] ", net.curPlayer.aid, aidBitmap, port, dataLen);
        for(int i=0; i < dataLen; i++) {
            printf("%02x", data[i]);
        }
        printf("\n");
    }

    int numSent = 0;

    for(int aid = 0; aid < 16; aid++) {
        if(!((1 << aid) & aidBitmap) || sMPPeer[aid] == NULL || aid == net.curPlayer.aid) {
            continue;
        }
        enet_peer_send(sMPPeer[aid], SIM_NET_CHANNEL_APPDATA, pkt);
        numSent++;
    }
    if(numSent == 0) {
        enet_packet_destroy(pkt);
    }
    ENET_UNLOCK();

    free(mpDataPacket);
    //return (numSent > 0);
    return TRUE;
    #endif
}

int SIM_Net_Scan(WMBssDesc * bssDescOut)
{
    int numParentsFound = 0;

    for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
        if(net.playerList[i].status == SIM_NET_PLAYERSTATUS_CLIENT || net.playerList[i].status == SIM_NET_PLAYERSTATUS_HOST) {
            if(net.playerList[i].mpState == SIM_NET_MPSTATE_PARENT) {
                // Found a parent
                memcpy(bssDescOut[numParentsFound].bssid, net.playerList[i].ndsMacAddr, 6);
                bssDescOut[numParentsFound].gameInfo.magicNumber = WM_GAMEINFO_MAGIC_NUMBER;
                bssDescOut[numParentsFound].gameInfoLength = WM_GAMEINFO_LENGTH_MIN;
                bssDescOut[numParentsFound].gameInfo.gameNameCount_attribute = net.playerList[i].entryFlag ? WM_ATTR_FLAG_ENTRY : 0;
                bssDescOut[numParentsFound].gameInfo.ggid = net.playerList[i].ggid;
                bssDescOut[numParentsFound].gameInfo.tgid = net.playerList[i].tgid;
                if(net.playerList[i].userGameInfoLength > 0) {
                    bssDescOut[numParentsFound].gameInfo.userGameInfoLength = net.playerList[i].userGameInfoLength;
                    memcpy(bssDescOut[numParentsFound].gameInfo.userGameInfo, net.playerList[i].userGameInfo, net.playerList[i].userGameInfoLength);
                }

                numParentsFound++;
            }
        }
    }

    return numParentsFound;
}

void SIM_Net_StartThread() {
    SDL_CreateThread(NetThread, "NET", NULL);
}

static int NetThread(void * arg)
{
    while(1) {
        SDL_Delay(1);
        SIM_Net_Process();
    }
}

static void InitMyPlayer()
{
    memset(&net.curPlayer, 0, sizeof(SIM_Net_Player_t));
    SIM_config_type * myConfig = SIM_GetConfigPtr();

    // Get username from OS Owner Info
    OSOwnerInfo myOwnerInfo;
    OS_GetOwnerInfo(&myOwnerInfo);

    for(int i=0; i < SIM_NET_USERNAME_LEN; i++) {
        char curChar = myOwnerInfo.nickName[i];
        net.curPlayer.userName[i] = curChar;
    }

    // Copy NDS MAC address
    memcpy(net.curPlayer.ndsMacAddr, myConfig->macAddr, sizeof(net.curPlayer.ndsMacAddr));
}

// Use when updating the current player info.
// If a client, sends a playerInfo message to the host
// If a host, update in the player list and broadcast the playerlist
static void SendMyPlayer()
{
    #ifndef SDK_BUILD_PROSPERO
    //TODO: Add PS5 support for ENet
    ENET_LOCK();
    if(net.state == SIM_NET_STATE_CLIENT) {
        SIM_Net_PlayerInfoPacket_t playerInfoPacket = {0};
        InitPacketHeader(&playerInfoPacket.common.header);
        playerInfoPacket.common.cmd = SIM_NET_MGMTPACKETCMD_PLAYER_INFO;
        memcpy(&playerInfoPacket.player, &net.curPlayer, sizeof(SIM_Net_Player_t));
        ENetPacket * pkt = enet_packet_create(&playerInfoPacket, sizeof(SIM_Net_PlayerInfoPacket_t), ENET_PACKET_FLAG_RELIABLE);
        int peerNo = -1;
        for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
            if(net.playerList[i].status == SIM_NET_PLAYERSTATUS_HOST && net.peerList[i] != NULL) {
                peerNo = i;
                break;
            }
        }
        if(peerNo >= 0) {
            enet_peer_send(net.peerList[peerNo], SIM_NET_CHANNEL_MGMT, pkt);
        } else {
            enet_packet_destroy(pkt);
            printf("Error sending player info: Could not find host!\n");
        }
    } else if(net.state == SIM_NET_STATE_HOST) {
        SDL_LockMutex(net.playerListMutex);
        memcpy(&net.playerList[net.curPlayer.id], &net.curPlayer, sizeof(SIM_Net_Player_t));
        SDL_UnlockMutex(net.playerListMutex);
        HostUpdatePlayerList();
    }
    ENET_UNLOCK();
    #endif
}

static void InitPacketHeader(SIM_Net_PacketHeader_t * header)
{
    header->magicNum = htonl(SIM_NET_MAGIC_NUM);
    header->protocolVer = htonl(SIM_NET_PROTOCOL_VER);
}

static BOOL CheckPacketHeader(SIM_Net_PacketHeader_t * header)
{
    if(htonl(header->magicNum) != SIM_NET_MAGIC_NUM) {
        return FALSE;
    }
    if(htonl(header->protocolVer != 0)) {
        return FALSE;
    }
    return TRUE;
}

#ifndef SDK_BUILD_PROSPERO
static void ProcessHostEvent(ENetEvent * event)
{
    switch(event->type) {
        case ENET_EVENT_TYPE_CONNECT:
            if(net.playerCount >= SIM_NET_MAX_PLAYERS) {
                //Lobby is full, block connection
                ENET_LOCK();
                enet_peer_disconnect(event->peer, 0);
                ENET_UNLOCK();
            } else {
                // Not full, accept connection
                // Send client init

                int idNo;
                for(idNo = 0; idNo < SIM_NET_MAX_PLAYERS; idNo++) {
                    if(net.playerList[idNo].status == SIM_NET_PLAYERSTATUS_NONE || idNo >= net.playerCount) {
                        break;
                    }
                }
                if(idNo < SIM_NET_MAX_PLAYERS) {
                    SIM_Net_ClientInitPacket_t clientInitPacket;
                    InitPacketHeader(&clientInitPacket.common.header);
                    clientInitPacket.common.cmd = SIM_NET_MGMTPACKETCMD_CLIENT_INIT;
                    clientInitPacket.playerId = idNo;


                    ENET_LOCK();
                    ENetPacket * myPacket = enet_packet_create(&clientInitPacket, sizeof(SIM_Net_ClientInitPacket_t), ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(event->peer, SIM_NET_CHANNEL_MGMT, myPacket);
                    ENET_UNLOCK();

                    SDL_LockMutex(net.playerListMutex);

                    net.playerList[idNo].id = idNo;
                    net.playerList[idNo].status = SIM_NET_PLAYERSTATUS_CONNECTING;
                    net.playerList[idNo].ipAddr = event->peer->address.host;
                    event->peer->data = &net.playerList[idNo];
                    net.playerCount++;

                    SDL_UnlockMutex(net.playerListMutex);

                    net.peerList[idNo] = event->peer;
                }
            }
            break;
        
        case ENET_EVENT_TYPE_DISCONNECT:
            {
                LinkLost(event->peer);
                SIM_Net_Player_t * player = (SIM_Net_Player_t*)event->peer->data;
                if(player == NULL) {
                    break;
                }

                int idNo = player->id;
                net.peerList[idNo] = NULL;
                player->id = 0;
                player->status = SIM_NET_PLAYERSTATUS_NONE;
                net.playerCount--;

                HostUpdatePlayerList();
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            {
                if(event->packet->dataLength < sizeof(SIM_Net_PacketHeader_t)) {
                    break;
                }

                SIM_Net_PacketHeader_t * header = (SIM_Net_PacketHeader_t *)event->packet->data;

                if(!CheckPacketHeader(header)) {
                    break;
                }

                if(event->channelID == SIM_NET_CHANNEL_MGMT) {
                    // Check MGMT packet details
                    if(event->packet->dataLength < sizeof(SIM_Net_MgmtPacketCommon_t)) {
                        break;
                    }
                    SIM_Net_MgmtPacketCommon_t * packetCommon = (SIM_Net_MgmtPacketCommon_t *)event->packet->data;
                    switch(packetCommon->cmd) {
                        case SIM_NET_MGMTPACKETCMD_CLIENT_INIT:
                            // I dont think the host should receive this one.
                            SIM_assert_always();
                            break;
                        case SIM_NET_MGMTPACKETCMD_PLAYER_INFO:
                            // Client sent us player info
                            {
                                if(event->packet->dataLength < sizeof(SIM_Net_PlayerInfoPacket_t)) {
                                    break;
                                }
                                SIM_Net_PlayerInfoPacket_t * playerInfoPkt = (SIM_Net_PlayerInfoPacket_t*)event->packet->data;

                                int idNo = playerInfoPkt->player.id;

                                if(idNo > SIM_NET_MAX_PLAYERS) {
                                    SIM_assert_always();
                                }

                                SDL_LockMutex(net.playerListMutex);

                                SIM_Net_Player_t *  thePlayer = &net.playerList[idNo];
                                memcpy(thePlayer, &playerInfoPkt->player, sizeof(SIM_Net_Player_t));
                                thePlayer->status = SIM_NET_PLAYERSTATUS_CLIENT;
                                thePlayer->ipAddr = event->peer->address.host;

                                SDL_UnlockMutex(net.playerListMutex);

                                HostUpdatePlayerList();
                            }
                            break;
                        default:
                            printf("NET: Unknown MGMT packet received. ID: %d\n", packetCommon->cmd);
                            break;
                    }
                }

                if(event->channelID == SIM_NET_CHANNEL_APPDATA) {
                    ProcessMPEvent(event);
                }
            }
            break;
        case ENET_EVENT_TYPE_NONE:
        default:
            break;
    }
}

static void ProcessClientEvent(ENetEvent * event)
{
    switch(event->type) {
        case ENET_EVENT_TYPE_CONNECT:
            {
                int idNo = -1;
                
                for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
                    SIM_Net_Player_t * player = &net.playerList[i];
                    if(i == net.curPlayer.id) {
                        continue;
                    }
                    if(player->status != SIM_NET_PLAYERSTATUS_CLIENT) {
                        continue;
                    }

                    if(player->ipAddr == event->peer->address.host) {
                        idNo = i;
                        break;
                    }
                }

                if(idNo < 0) {
                    ENET_LOCK();
                    enet_peer_disconnect(event->peer, 0);
                    ENET_UNLOCK();
                    break;
                }

                net.peerList[idNo] = event->peer;
                event->peer->data = &net.playerList[idNo];
            }
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            {
                LinkLost(event->peer);
                SIM_Net_Player_t * player = (SIM_Net_Player_t*)event->peer->data;
                if(!player) {
                    break;
                }

                int idNo = player->id;
                if(event->peer == net.peerList[0] || player->status == SIM_NET_PLAYERSTATUS_HOST) {
                    sHostLost = TRUE; // the host stopped or went out of reach
                }
                net.peerList[idNo] = NULL;
                SDL_LockMutex(net.playerListMutex);
                player->status = SIM_NET_PLAYERSTATUS_DISCONNECTED;
                SDL_UnlockMutex(net.playerListMutex);


            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            {
                if(event->packet->dataLength < sizeof(SIM_Net_PacketHeader_t)) {
                    break;
                }

                SIM_Net_PacketHeader_t * header = (SIM_Net_PacketHeader_t *)event->packet->data;

                if(!CheckPacketHeader(header)) {
                    break;
                }

                if(event->channelID == SIM_NET_CHANNEL_MGMT) {
                    // Check MGMT packet details
                    if(event->packet->dataLength < sizeof(SIM_Net_MgmtPacketCommon_t)) {
                        break;
                    }
                    SIM_Net_MgmtPacketCommon_t * packetCommon = (SIM_Net_MgmtPacketCommon_t *)event->packet->data;
                    switch(packetCommon->cmd) {
                        case SIM_NET_MGMTPACKETCMD_PLAYER_LIST:
                            {
                                if(event->packet->dataLength < sizeof(SIM_Net_PlayerListPacket_t)) {
                                    break;
                                }
                                SIM_Net_PlayerListPacket_t * playerListPkt = (SIM_Net_PlayerListPacket_t*)event->packet->data;

                                net.playerCount = playerListPkt->numPlayers;
                                for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
                                    memcpy(&net.playerList[i], &playerListPkt->playerList[i], sizeof(SIM_Net_Player_t));
                                    net.playerList[i].id = htonl(playerListPkt->playerList[i].id);
                                    //memcpy(net.playerList[i].userName, playerListPkt->playerList[i].userName, 10);
                                    //memcpy(net.playerList[i].ndsMacAddr, playerListPkt->playerList[i].ndsMacAddr, 6);
                                    //net.playerList[i]
                                    //net.playerList[i].status = playerListPkt->playerList[i].status;
                                    //net.playerList[i].mpState = playerListPkt->playerList[i].mpState;
                                    //net.playerList[i].ggid = playerListPkt->playerList[i].ggid;
                                    //net.playerList[i].tgid = playerListPkt->playerList[i].tgid;
                                }
                                memcpy(&net.playerList[net.curPlayer.id], &net.curPlayer, sizeof(SIM_Net_Player_t));
                            }
                            break;
                    }
                }

                if(event->channelID == SIM_NET_CHANNEL_APPDATA) {
                    ProcessMPEvent(event);
                }
            }

            
            break;
        case ENET_EVENT_TYPE_NONE:
        default:
            break;
    }
}

// MP packets go by what they are, not by this device's MP state: a device that was a
// parent a moment ago and is joining someone else gets its join response here
static void ProcessMPEvent(ENetEvent * event)
{
    if(event->packet->dataLength < sizeof(SIM_Net_MpPacketCommon_t)) {
        return;
    }
    const SIM_Net_MpPacketCommon_t * mpPacketBase = (const SIM_Net_MpPacketCommon_t *)event->packet->data;
    switch(mpPacketBase->cmd) {
        case SIM_NET_MPPACKETCMD_JOIN_REQ:
            ProcessMPParentEvent(event);
            break;
        case SIM_NET_MPPACKETCMD_JOIN_RESP:
            ProcessMPChildEvent(event);
            break;
        case SIM_NET_MPPACKETCMD_DISCONNECT:
            if(event->packet->dataLength >= sizeof(SIM_Net_DisconnectPacket_t)
               && ((const SIM_Net_DisconnectPacket_t *)mpPacketBase)->sendingAid == 0
               && net.curPlayer.mpState != SIM_NET_MPSTATE_PARENT) {
                ProcessMPChildEvent(event); // from our parent
            } else {
                ProcessMPParentEvent(event); // a child leaving
            }
            break;
        case SIM_NET_MPPACKETCMD_DATA:
            if(event->packet->dataLength >= sizeof(SIM_Net_MpDataPacket_t)) {
                ReceiveMPData(event);
            }
            break;
        default:
            printf("NET: Unknown MP packet %d\n", mpPacketBase->cmd);
            break;
    }
}

static void ProcessMPParentEvent(ENetEvent * event)
{
    if(event->packet->dataLength < sizeof(SIM_Net_MpPacketCommon_t)) {
        return;
    }

    SIM_Net_MpPacketCommon_t * mpPacketBase = (SIM_Net_MpPacketCommon_t *)event->packet->data;

    switch(mpPacketBase->cmd) {
        case SIM_NET_MPPACKETCMD_JOIN_REQ:
            {
                if(net.curPlayer.mpState != SIM_NET_MPSTATE_PARENT || !net.curPlayer.entryFlag) {
                    // Not a parent taking children right now (the child's scan saw an older
                    // state): refuse, as a DS's association would fail
                    SIM_Net_JoinRespPacket_t refusal = {};
                    InitPacketHeader(&refusal.common.header);
                    refusal.common.cmd = SIM_NET_MPPACKETCMD_JOIN_RESP;
                    refusal.aid = 0;
                    printf("NET: Refused a join request (not taking children now).\n");
                    ENET_LOCK();
                    ENetPacket * refusalPkt = enet_packet_create(&refusal, sizeof(refusal), ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(event->peer, SIM_NET_CHANNEL_APPDATA, refusalPkt);
                    ENET_UNLOCK();
                    break;
                }

                // Determine the AID
                int aid = -1;
                for(int i=1; i < 16; i++) {
                    if(((net.aidBitmask >> i) & 1) == 0) {
                        aid = i;
                        net.aidBitmask = net.aidBitmask | (1 << aid);
                        break;
                    }
                }

                if(aid <= 0) {
                    break;
                }

                // Send a response with the AID
                SIM_Net_JoinRespPacket_t joinResponse = {};
                InitPacketHeader(&joinResponse.common.header);
                joinResponse.common.cmd = SIM_NET_MPPACKETCMD_JOIN_RESP;

                joinResponse.aid = aid;
                printf("NET: Received join request. Assigning AID %d.\n", aid);

                ENET_LOCK();
                ENetPacket * pkt = enet_packet_create(&joinResponse, sizeof(SIM_Net_JoinRespPacket_t), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(event->peer, SIM_NET_CHANNEL_APPDATA, pkt);
                sMPPeer[aid] = event->peer;
                ENET_UNLOCK();

                if(event->peer->data != NULL) {
                    SDL_LockMutex(net.playerListMutex);
                    // Provisionally set the player's MP status and AID
                    SIM_Net_Player_t * player = (SIM_Net_Player_t*)event->peer->data;
                    player->aid = aid;
                    player->mpState = SIM_NET_MPSTATE_CHILD;

                    SDL_UnlockMutex(net.playerListMutex);
                }
                
                SIM_WMSP_Indicate_t indicate = {};
                indicate.cmd = SIM_NET_MPPACKETCMD_JOIN_REQ;
                indicate.aid = aid;
                indicate.dataLen = 0;
                SIM_Net_JoinReqPacket_t * joinReq = (SIM_Net_JoinReqPacket_t*)mpPacketBase;
                indicate.data = joinReq->ssid;
                
                WMSP_Indicate(&indicate);
            }
            break;
        case SIM_NET_MPPACKETCMD_JOIN_RESP:
            SIM_assert_always_msg("Parent device should not receive a join response");
            break;
        case SIM_NET_MPPACKETCMD_DISCONNECT:
            // Child device is disconnecting
            {
                SIM_Net_DisconnectPacket_t * disconnectPacket = (SIM_Net_DisconnectPacket_t*)mpPacketBase;
                net.aidBitmask &= ~(1 << disconnectPacket->sendingAid);
                if(disconnectPacket->sendingAid < 16) {
                    ENET_LOCK();
                    sMPPeer[disconnectPacket->sendingAid] = NULL;
                    ENET_UNLOCK();
                }

                printf("NET: Parent received disconnect from AID %d", disconnectPacket->sendingAid);

                SIM_WMSP_Indicate_t indicate = {};
                indicate.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
                indicate.aid = disconnectPacket->sendingAid;

                WMSP_Indicate(&indicate);
            }
            break;
        case SIM_NET_MPPACKETCMD_DATA:
            ReceiveMPData(event);
            break;
        default:
            SIM_assert_always_msg("Unknown MP Packet received.");
            break;
    }
}

static void ProcessMPChildEvent(ENetEvent * event)
{
    if(event->packet->dataLength < sizeof(SIM_Net_MpPacketCommon_t)) {
        return;
    }

    SIM_Net_MpPacketCommon_t * mpPacketBase = (SIM_Net_MpPacketCommon_t *)event->packet->data;

    switch(mpPacketBase->cmd) {
        case SIM_NET_MPPACKETCMD_JOIN_REQ:
            printf("NET Warning: Child Device received MP Join request.\n");
            break;
        case SIM_NET_MPPACKETCMD_JOIN_RESP:
            {
                SIM_Net_JoinRespPacket_t * joinRespPacket = (SIM_Net_JoinRespPacket_t*)event->packet->data;
                if(joinRespPacket->aid == 0) {
                    printf("NET: Join refused.\n");
                    sJoinRefused = TRUE;
                    SDL_SemPost(net.joinReqSemaphore);
                    break;
                }
                printf("NET: Received join response. AID %d.\n", joinRespPacket->aid);
                ENET_LOCK();
                ClearMPLinks();
                sMPPeer[0] = event->peer;
                ENET_UNLOCK();
                net.curPlayer.aid = joinRespPacket->aid;
                net.curPlayer.mpState = SIM_NET_MPSTATE_CHILD;
                SendMyPlayer();
                SDL_SemPost(net.joinReqSemaphore);
                break;
            }
        case SIM_NET_MPPACKETCMD_DISCONNECT:
            // Parent is kicking this child out of the group
            {
                SIM_Net_DisconnectPacket_t * disconnectPacket = (SIM_Net_DisconnectPacket_t*)mpPacketBase;

                SIM_assert(disconnectPacket->sendingAid == 0);
                printf("NET: Child received disconnect. Setting MP state to OFF.\n");
                ENET_LOCK();
                ClearMPLinks();
                ENET_UNLOCK();

                net.curPlayer.aid = 0;
                net.curPlayer.mpState = SIM_NET_MPSTATE_OFF;
                SendMyPlayer();

                SIM_WMSP_Indicate_t indicate = {};
                indicate.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
                indicate.aid = 0;

                WMSP_Indicate(&indicate);
            }
            break;

        case SIM_NET_MPPACKETCMD_DATA:
            ReceiveMPData(event);
            break;
        
        default:
            SIM_assert_always_msg("Unknown MP Packet received");
            break;
    }
}

static void ReceiveMPData(ENetEvent * event)
{
    SIM_Net_MpDataPacket_t * dataPacket = (SIM_Net_MpDataPacket_t *)event->packet->data;

    SIM_WMSP_Indicate_t indicate = {};
    indicate.cmd = SIM_NET_MPPACKETCMD_DATA;
    indicate.aid = dataPacket->aid;
    indicate.port = dataPacket->port;
    indicate.dataLen = dataPacket->dataLen;
    indicate.data = dataPacket->data;
    if(LogData()) {
        printf("[RECV aid=%d port=%d len=%d] ", dataPacket->aid, dataPacket->port, dataPacket->dataLen);
        for(int i=0; i < dataPacket->dataLen; i++) {
            printf("%02x", dataPacket->data[i]);
        }
        printf("\n");
    }

    WMSP_Indicate(&indicate);
}
#endif


// --- stopping, joining in the background ------------------------------------------------------

void SIM_Net_Stop()
{
    #ifndef SDK_BUILD_PROSPERO
    ENET_LOCK();
    if(net.enetHost != NULL) {
        for(size_t i = 0; i < net.enetHost->peerCount; i++) {
            ENetPeer * peer = &net.enetHost->peers[i];
            if(peer->state != ENET_PEER_STATE_DISCONNECTED) {
                enet_peer_disconnect_now(peer, 0);
            }
        }
        enet_host_flush(net.enetHost);
        enet_host_destroy(net.enetHost);
        net.enetHost = NULL;
    }
    SDL_LockMutex(net.playerListMutex);
    memset(net.playerList, 0, sizeof(net.playerList));
    memset(net.peerList, 0, sizeof(net.peerList));
    memset(&net.curPlayer, 0, sizeof(net.curPlayer));
    net.playerCount = 0;
    net.aidBitmask = 0;
    SDL_UnlockMutex(net.playerListMutex);
    ClearMPLinks();
    sHostLost = FALSE;
    if(net.state != SIM_NET_STATE_UNINITIALIZED) {
        net.state = SIM_NET_STATE_DISCONNECTED;
    }
    ENET_UNLOCK();
    #endif
}

static int ConnectThread(void * arg)
{
    (void)arg;
    BOOL ok = SIM_Net_StartClient(sConnectAddress);
    sConnectFailed = !ok;
    SDL_AtomicSet(&sConnecting, 0);
    return 0;
}

BOOL SIM_Net_StartClientAsync(const char * ipAddress)
{
    if(net.state != SIM_NET_STATE_DISCONNECTED || SDL_AtomicGet(&sConnecting) != 0) {
        return FALSE;
    }
    snprintf(sConnectAddress, sizeof(sConnectAddress), "%s", ipAddress);
    sConnectFailed = FALSE;
    SDL_AtomicSet(&sConnecting, 1);
    SDL_Thread * thread = SDL_CreateThread(ConnectThread, "NETJOIN", NULL);
    if(thread == NULL) {
        SDL_AtomicSet(&sConnecting, 0);
        sConnectFailed = TRUE;
        return FALSE;
    }
    SDL_DetachThread(thread);
    return TRUE;
}

BOOL SIM_Net_IsConnecting()
{
    return SDL_AtomicGet(&sConnecting) != 0;
}

BOOL SIM_Net_TakeConnectFailed()
{
    BOOL failed = sConnectFailed;
    sConnectFailed = FALSE;
    return failed;
}

BOOL SIM_Net_HostLost()
{
    return sHostLost;
}

#ifndef SDK_BUILD_PROSPERO
// --- finding each other on the network ---------------------------------------------------------
//
// A host says so on the local network every second (a UDP broadcast on SIM_NET_DISCOVERY_PORT:
// "NTRLINK1", the game code and a name); while searching, the other devices listen for it.

#define DISCOVERY_MAGIC "NTRLINK1"

typedef struct {
    char magic[8];
    char gameCode[4];
    char name[48];
} DiscoveryPacket;

static socket_t sDiscoverySocket = INVALID_SOCKET;   // sending (host)
static socket_t sListenSocket = INVALID_SOCKET;      // receiving (searching)
static SDL_atomic_t sSearching;
static u64 sLastBeaconMs = 0;
static SIM_Net_FoundHost sFound[SIM_NET_MAX_FOUND];
static int sFoundCount = 0;
static char sDeviceName[48] = "";

static void SetNonBlocking(socket_t sock)
{
    #ifdef __WIN32__
    u_long on = 1;
    ioctlsocket(sock, FIONBIO, &on);
    #else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    #endif
}

static void GameCode(char out[4])
{
    const u8 * header = (const u8 *)HW_ROM_HEADER_BUF;
    memcpy(out, header + 0x0C, 4);
}

void SIM_Net_SetDeviceName(const char * name)
{
    snprintf(sDeviceName, sizeof(sDeviceName), "%s", name != NULL ? name : "");
}

void SIM_Net_SetSearching(BOOL searching)
{
    const int was = SDL_AtomicSet(&sSearching, searching ? 1 : 0);
    #ifdef SDK_BUILD_ANDROID
    // Many devices drop broadcasts on Wi-Fi unless an app holds a multicast lock
    if((was != 0) != (searching != FALSE)) {
        JNIEnv * env = (JNIEnv *)SDL_AndroidGetJNIEnv();
        jobject activity = (jobject)SDL_AndroidGetActivity();
        if(env != NULL && activity != NULL) {
            jclass activityClass = env->GetObjectClass(activity);
            jmethodID method = env->GetMethodID(activityClass, "setLanSearch", "(Z)V");
            if(method != NULL) {
                env->CallVoidMethod(activity, method, searching ? JNI_TRUE : JNI_FALSE);
            }
            env->ExceptionClear();
            env->DeleteLocalRef(activityClass);
            env->DeleteLocalRef(activity);
        }
    }
    #else
    (void)was;
    #endif
}

int SIM_Net_GetFoundHosts(SIM_Net_FoundHost * out, int max)
{
    SDL_LockMutex(net.playerListMutex);
    int count = sFoundCount < max ? sFoundCount : max;
    memcpy(out, sFound, sizeof(SIM_Net_FoundHost) * count);
    SDL_UnlockMutex(net.playerListMutex);
    return count;
}

static void DiscoveryUpdate(void)
{
    const u64 now = SDL_GetTicks64();

    // Hosting: say so every second
    if(net.state == SIM_NET_STATE_HOST && now - sLastBeaconMs >= 1000) {
        sLastBeaconMs = now;
        if(sDiscoverySocket == INVALID_SOCKET) {
            sDiscoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if(sDiscoverySocket != INVALID_SOCKET) {
                int on = 1;
                setsockopt(sDiscoverySocket, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
                SetNonBlocking(sDiscoverySocket);
            }
        }
        if(sDiscoverySocket != INVALID_SOCKET) {
            DiscoveryPacket packet;
            memset(&packet, 0, sizeof(packet));
            memcpy(packet.magic, DISCOVERY_MAGIC, 8);
            GameCode(packet.gameCode);
            snprintf(packet.name, sizeof(packet.name), "%s", sDeviceName[0] != '\0' ? sDeviceName : net.curPlayer.userName);
            sockaddr_in_t to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(SIM_NET_DISCOVERY_PORT);
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(sDiscoverySocket, (const char *)&packet, sizeof(packet), 0, (const sockaddr_t *)&to, sizeof(to));
        }
    } else if(net.state != SIM_NET_STATE_HOST && sDiscoverySocket != INVALID_SOCKET) {
        closesocket(sDiscoverySocket);
        sDiscoverySocket = INVALID_SOCKET;
    }

    // Searching: listen
    const BOOL searching = SDL_AtomicGet(&sSearching) != 0 && net.state == SIM_NET_STATE_DISCONNECTED;
    if(searching && sListenSocket == INVALID_SOCKET) {
        sListenSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if(sListenSocket != INVALID_SOCKET) {
            int on = 1;
            setsockopt(sListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
            #ifdef SO_REUSEPORT
            setsockopt(sListenSocket, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof(on));
            #endif
            setsockopt(sListenSocket, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
            sockaddr_in_t addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(SIM_NET_DISCOVERY_PORT);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if(bind(sListenSocket, (const sockaddr_t *)&addr, sizeof(addr)) != 0) {
                closesocket(sListenSocket);
                sListenSocket = INVALID_SOCKET;
            } else {
                SetNonBlocking(sListenSocket);
            }
        }
    } else if(!searching && sListenSocket != INVALID_SOCKET) {
        closesocket(sListenSocket);
        sListenSocket = INVALID_SOCKET;
        SDL_LockMutex(net.playerListMutex);
        sFoundCount = 0;
        SDL_UnlockMutex(net.playerListMutex);
    }
    if(sListenSocket == INVALID_SOCKET) {
        return;
    }

    SDL_LockMutex(net.playerListMutex);
    while(TRUE) {
        DiscoveryPacket packet;
        sockaddr_in_t from;
        #ifdef __WIN32__
        int fromLength = sizeof(from);
        #else
        socklen_t fromLength = sizeof(from);
        #endif
        int got = recvfrom(sListenSocket, (char *)&packet, sizeof(packet), 0, (sockaddr_t *)&from, &fromLength);
        if(got < (int)sizeof(packet)) {
            break;
        }
        // Hosts running a game this build runs too (HeartGold finds SoulSilver)
        if(memcmp(packet.magic, DISCOVERY_MAGIC, 8) != 0 || !SIM_Carts_RunsGameCode(packet.gameCode)) {
            continue;
        }
        packet.name[sizeof(packet.name) - 1] = '\0';
        const u32 address = ntohl(from.sin_addr.s_addr);
        int slot = -1;
        for(int i = 0; i < sFoundCount; i++) {
            if(sFound[i].address == address) {
                slot = i;
            }
        }
        if(slot < 0 && sFoundCount < SIM_NET_MAX_FOUND) {
            slot = sFoundCount++;
        }
        if(slot >= 0) {
            sFound[slot].address = address;
            snprintf(sFound[slot].addressText, sizeof(sFound[slot].addressText), "%u.%u.%u.%u", (address >> 24) & 0xFF,
                     (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
            snprintf(sFound[slot].name, sizeof(sFound[slot].name), "%s", packet.name);
            sFound[slot].lastSeenMs = now;
        }
    }
    // Gone quiet: gone
    for(int i = 0; i < sFoundCount;) {
        if(now - sFound[i].lastSeenMs > 4000) {
            sFound[i] = sFound[--sFoundCount];
        } else {
            i++;
        }
    }
    SDL_UnlockMutex(net.playerListMutex);
}

BOOL SIM_Net_LocalAddress(char * out, int outSize)
{
    // The address the system would send from to the internet: a UDP "connection" sends
    // nothing, it only picks the interface
    out[0] = '\0';
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock == INVALID_SOCKET) {
        return FALSE;
    }
    sockaddr_in_t to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);
    BOOL ok = FALSE;
    if(connect(sock, (const sockaddr_t *)&to, sizeof(to)) == 0) {
        sockaddr_in_t mine;
        #ifdef __WIN32__
        int length = sizeof(mine);
        #else
        socklen_t length = sizeof(mine);
        #endif
        if(getsockname(sock, (sockaddr_t *)&mine, &length) == 0) {
            const u32 address = ntohl(mine.sin_addr.s_addr);
            if(address != 0) {
                snprintf(out, outSize, "%u.%u.%u.%u", (address >> 24) & 0xFF, (address >> 16) & 0xFF,
                         (address >> 8) & 0xFF, address & 0xFF);
                ok = TRUE;
            }
        }
    }
    closesocket(sock);
    return ok;
}
#else // SDK_BUILD_PROSPERO
static void DiscoveryUpdate(void) {}
void SIM_Net_SetDeviceName(const char * name) { (void)name; }
void SIM_Net_SetSearching(BOOL searching) { (void)searching; }
int SIM_Net_GetFoundHosts(SIM_Net_FoundHost * out, int max) { (void)out; (void)max; return 0; }
BOOL SIM_Net_LocalAddress(char * out, int outSize) { if(outSize > 0) { out[0] = '\0'; } return FALSE; }
#endif

static void HostUpdatePlayerList()
{
    #ifndef SDK_BUILD_PROSPERO
    SIM_Net_PlayerListPacket_t playerListPkt = {};

    InitPacketHeader(&playerListPkt.common.header);
    playerListPkt.common.cmd = SIM_NET_MGMTPACKETCMD_PLAYER_LIST;
    playerListPkt.numPlayers = net.playerCount;
    memcpy(playerListPkt.playerList, net.playerList, sizeof(playerListPkt.playerList));
    ENET_LOCK();
    if(net.enetHost != NULL) {
        ENetPacket * pkt = enet_packet_create(&playerListPkt, sizeof(SIM_Net_PlayerListPacket_t), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(net.enetHost, SIM_NET_CHANNEL_MGMT, pkt);
    }
    ENET_UNLOCK();
    #endif
}
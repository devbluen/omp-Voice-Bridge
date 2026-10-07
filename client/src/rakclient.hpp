/*
 *  Voice Bridge client
 *
 *  SA-MP keeps its RakClient in CNetGame.  The pointer is replaced by a proxy
 *  with the same interface, which works on every samp.dll build because the
 *  RakNet interface never changed.  The virtual methods must stay in this
 *  exact order.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vbc::rak
{
#pragma pack(push, 1)
struct PlayerID
{
	unsigned int binaryAddress;
	unsigned short port;
};

struct Packet
{
	unsigned short playerIndex;
	PlayerID playerId;
	unsigned int length;
	unsigned int bitSize;
	unsigned char* data;
	bool deleteData;
};

struct NetworkID
{
	PlayerID playerId;
	unsigned short localSystemId;
};
#pragma pack(pop)

// Same memory layout as RakNet::BitStream in samp.dll.
struct BitStream
{
	int numberOfBitsUsed;
	int numberOfBitsAllocated;
	int readOffset;
	unsigned char* data;
	bool copyData;
	unsigned char stackData[256];
};

enum PacketPriority
{
	SYSTEM_PRIORITY,
	HIGH_PRIORITY,
	MEDIUM_PRIORITY,
	LOW_PRIORITY,
};

enum PacketReliability
{
	UNRELIABLE = 6,
	UNRELIABLE_SEQUENCED,
	RELIABLE,
	RELIABLE_ORDERED,
	RELIABLE_SEQUENCED,
};

struct RPCParameters;
struct RakNetStatisticsStruct;
using RPCFunction = void (*)(RPCParameters*);

class RakClientInterface
{
public:
	virtual ~RakClientInterface() { }
	virtual bool Connect(const char* host, unsigned short serverPort, unsigned short clientPort, unsigned int depreciated, int threadSleepTimer) = 0;
	virtual void Disconnect(unsigned int blockDuration, unsigned char orderingChannel = 0) = 0;
	virtual void InitializeSecurity(const char* privKeyP, const char* privKeyQ) = 0;
	virtual void SetPassword(const char* password) = 0;
	virtual bool HasPassword() = 0;
	virtual bool Send(const char* data, const int length, PacketPriority priority, PacketReliability reliability, char orderingChannel) = 0;
	virtual bool Send(BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel) = 0;
	virtual Packet* Receive() = 0;
	virtual void DeallocatePacket(Packet* packet) = 0;
	virtual void PingServer() = 0;
	virtual void PingServer(const char* host, unsigned short serverPort, unsigned short clientPort, bool onlyReplyOnAcceptingConnections) = 0;
	virtual int GetAveragePing() = 0;
	virtual int GetLastPing() = 0;
	virtual int GetLowestPing() = 0;
	virtual int GetPlayerPing(const PlayerID playerId) = 0;
	virtual void StartOccasionalPing() = 0;
	virtual void StopOccasionalPing() = 0;
	virtual bool IsConnected() = 0;
	virtual unsigned int GetSynchronizedRandomInteger() = 0;
	virtual bool GenerateCompressionLayer(unsigned int inputFrequencyTable[256], bool inputLayer) = 0;
	virtual bool DeleteCompressionLayer(bool inputLayer) = 0;
	virtual void RegisterAsRemoteProcedureCall(int* uniqueID, RPCFunction functionPointer) = 0;
	virtual void RegisterClassMemberRPC(int* uniqueID, void* functionPointer) = 0;
	virtual void UnregisterAsRemoteProcedureCall(int* uniqueID) = 0;
	virtual bool RPC(int* uniqueID, const char* data, unsigned int bitLength, PacketPriority priority, PacketReliability reliability, char orderingChannel, bool shiftTimestamp) = 0;
	virtual bool RPC(int* uniqueID, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, bool shiftTimestamp) = 0;
	virtual bool RPC_(int* uniqueID, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, bool shiftTimestamp, NetworkID networkID) = 0;
	virtual void SetTrackFrequencyTable(bool b) = 0;
	virtual bool GetSendFrequencyTable(unsigned int outputFrequencyTable[256]) = 0;
	virtual float GetCompressionRatio() = 0;
	virtual float GetDecompressionRatio() = 0;
	virtual void AttachPlugin(void* messageHandler) = 0;
	virtual void DetachPlugin(void* messageHandler) = 0;
	virtual BitStream* GetStaticServerData() = 0;
	virtual void SetStaticServerData(const char* data, const int length) = 0;
	virtual BitStream* GetStaticClientData(const PlayerID playerId) = 0;
	virtual void SetStaticClientData(const PlayerID playerId, const char* data, const int length) = 0;
	virtual void SendStaticClientDataToServer() = 0;
	virtual PlayerID GetServerID() = 0;
	virtual PlayerID GetPlayerID() = 0;
	virtual PlayerID GetInternalID() = 0;
	virtual const char* PlayerIDToDottedIP(const PlayerID playerId) = 0;
	virtual void PushBackPacket(Packet* packet, bool pushAtHead) = 0;
	virtual void SetRouterInterface(void* routerInterface) = 0;
	virtual void RemoveRouterInterface(void* routerInterface) = 0;
	virtual void SetTimeoutTime(unsigned int timeMS) = 0;
	virtual bool SetMTUSize(int size) = 0;
	virtual int GetMTUSize() = 0;
	virtual void AllowConnectionResponseIPMigration(bool allow) = 0;
	virtual void AdvertiseSystem(const char* host, unsigned short remotePort, const char* data, int dataLength) = 0;
	virtual RakNetStatisticsStruct* GetStatistics() = 0;
	virtual void ApplyNetworkSimulator(double maxSendBPS, unsigned short minExtraPing, unsigned short extraPingVariance) = 0;
	virtual bool IsNetworkSimulatorActive() = 0;
	virtual unsigned short GetPlayerIndex() = 0;
};

// Callbacks run on the game thread, inside SA-MP's network processing.
class Events
{
public:
	virtual ~Events() = default;
	virtual void onConnect(const char* host, unsigned short port) = 0;
	virtual void onConnectionAccepted(uint32_t serverIp, uint16_t serverPort) = 0;
	virtual void onDisconnect() = 0;
	// Bytes appended to the client join RPC.
	virtual std::vector<uint8_t> joinPayload() = 0;
	virtual void onPacket(const uint8_t* data, std::size_t size) = 0;
};

// Replaces *slot with the proxy.  Safe to call every frame.
bool Install(void** slot, Events* events);
bool Installed();
const void* ProxyVtable();
bool Connected();
bool Send(const uint8_t* data, std::size_t size, bool reliable);
int Ping();
}

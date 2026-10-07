/*
 *  Voice Bridge client
 */

#include "rakclient.hpp"
#include "log.hpp"
#include <vb-protocol.hpp>
#include <cstring>
#include <windows.h>

namespace vbc::rak
{
namespace
{
Events* g_events = nullptr;
RakClientInterface* g_original = nullptr;
RakClientInterface* g_proxy = nullptr;
bool g_connected = false;
// CNetGame's RakClient pointer the proxy was written to.
void** g_slot = nullptr;
// When the proxy handed the slot back to SA-MP (see Proxy::Disconnect).
uint64_t g_retiredAt = 0;
constexpr uint64_t kReinstallAfterMs = 10000;

BitStream makeStream(uint8_t* data, std::size_t size)
{
	BitStream stream {};
	stream.numberOfBitsUsed = static_cast<int>(size * 8);
	stream.numberOfBitsAllocated = static_cast<int>(size * 8);
	stream.readOffset = 0;
	stream.data = data;
	stream.copyData = false;
	return stream;
}


class Proxy final : public RakClientInterface
{
public:
	explicit Proxy(RakClientInterface* original)
		: o_(original)
	{
	}

	~Proxy() override
	{
		// SA-MP deletes its RakClient through this pointer on exit.
		if (g_proxy == this)
		{
			g_original = nullptr;
			g_proxy = nullptr;
			g_connected = false;
		}
		delete o_;
	}

	bool Connect(const char* host, unsigned short serverPort, unsigned short clientPort, unsigned int depreciated, int threadSleepTimer) override
	{
		if (g_events && host)
		{
			g_events->onConnect(host, serverPort);
		}
		return o_->Connect(host, serverPort, clientPort, depreciated, threadSleepTimer);
	}

	void Disconnect(unsigned int blockDuration, unsigned char orderingChannel) override
	{
		if (g_connected && g_events)
		{
			g_events->onDisconnect();
		}
		g_connected = false;
		o_->Disconnect(blockDuration, orderingChannel);

		// SA-MP disconnects right before destroying the RakClient (/q, game
		// exit), and it destroys it as its own concrete class: the pointer
		// is adjusted to the start of that class, which on this proxy lands
		// on unrelated memory (crash in CNetGame::~CNetGame).  Hand its own
		// object back first; the proxy is simply left alive.
		if (g_slot && *g_slot == this)
		{
			*g_slot = o_;
			g_proxy = nullptr;
			g_retiredAt = GetTickCount64();
			Log("RakClient handed back to SA-MP");
		}
	}

	void InitializeSecurity(const char* privKeyP, const char* privKeyQ) override { o_->InitializeSecurity(privKeyP, privKeyQ); }
	void SetPassword(const char* password) override { o_->SetPassword(password); }
	bool HasPassword() override { return o_->HasPassword(); }

	bool Send(const char* data, const int length, PacketPriority priority, PacketReliability reliability, char orderingChannel) override
	{
		return o_->Send(data, length, priority, reliability, orderingChannel);
	}

	bool Send(BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel) override
	{
		return o_->Send(bitStream, priority, reliability, orderingChannel);
	}

	Packet* Receive() override
	{
		for (;;)
		{
			Packet* packet = o_->Receive();
			if (!packet || !packet->data || packet->length == 0)
			{
				return packet;
			}
			switch (packet->data[0])
			{
			case vb::kRakPacketId:
				if (g_events && packet->length > 1)
				{
					g_events->onPacket(packet->data + 1, packet->length - 1);
				}
				o_->DeallocatePacket(packet);
				continue;
			case vb::kIdConnectionRequestAccepted:
				g_connected = true;
				if (g_events)
				{
					g_events->onConnectionAccepted(packet->playerId.binaryAddress, packet->playerId.port);
				}
				break;
			case vb::kIdDisconnectionNotification:
			case vb::kIdConnectionLost:
				if (g_connected && g_events)
				{
					g_events->onDisconnect();
				}
				g_connected = false;
				break;
			default:
				break;
			}
			return packet;
		}
	}

	void DeallocatePacket(Packet* packet) override { o_->DeallocatePacket(packet); }
	void PingServer() override { o_->PingServer(); }
	void PingServer(const char* host, unsigned short serverPort, unsigned short clientPort, bool onlyReplyOnAcceptingConnections) override
	{
		o_->PingServer(host, serverPort, clientPort, onlyReplyOnAcceptingConnections);
	}
	int GetAveragePing() override { return o_->GetAveragePing(); }
	int GetLastPing() override { return o_->GetLastPing(); }
	int GetLowestPing() override { return o_->GetLowestPing(); }
	int GetPlayerPing(const PlayerID playerId) override { return o_->GetPlayerPing(playerId); }
	void StartOccasionalPing() override { o_->StartOccasionalPing(); }
	void StopOccasionalPing() override { o_->StopOccasionalPing(); }
	bool IsConnected() override { return o_->IsConnected(); }
	unsigned int GetSynchronizedRandomInteger() override { return o_->GetSynchronizedRandomInteger(); }
	bool GenerateCompressionLayer(unsigned int inputFrequencyTable[256], bool inputLayer) override
	{
		return o_->GenerateCompressionLayer(inputFrequencyTable, inputLayer);
	}
	bool DeleteCompressionLayer(bool inputLayer) override { return o_->DeleteCompressionLayer(inputLayer); }
	void RegisterAsRemoteProcedureCall(int* uniqueID, RPCFunction functionPointer) override { o_->RegisterAsRemoteProcedureCall(uniqueID, functionPointer); }
	void RegisterClassMemberRPC(int* uniqueID, void* functionPointer) override { o_->RegisterClassMemberRPC(uniqueID, functionPointer); }
	void UnregisterAsRemoteProcedureCall(int* uniqueID) override { o_->UnregisterAsRemoteProcedureCall(uniqueID); }

	bool RPC(int* uniqueID, const char* data, unsigned int bitLength, PacketPriority priority, PacketReliability reliability, char orderingChannel,
		bool shiftTimestamp) override
	{
		if (uniqueID && *uniqueID == vb::kRpcClientJoin)
		{
			std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + (bitLength + 7) / 8);
			BitStream stream = makeStream(bytes.data(), bytes.size());
			stream.numberOfBitsUsed = static_cast<int>(bitLength);
			return RPC(uniqueID, &stream, priority, reliability, orderingChannel, shiftTimestamp);
		}
		return o_->RPC(uniqueID, data, bitLength, priority, reliability, orderingChannel, shiftTimestamp);
	}

	bool RPC(int* uniqueID, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, bool shiftTimestamp) override
	{
		if (uniqueID && bitStream && g_events)
		{
			if (*uniqueID == vb::kRpcClientJoin)
			{
				return sendJoin(uniqueID, bitStream, priority, reliability, orderingChannel, shiftTimestamp);
			}
		}
		return o_->RPC(uniqueID, bitStream, priority, reliability, orderingChannel, shiftTimestamp);
	}

	bool RPC_(int* uniqueID, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, bool shiftTimestamp,
		NetworkID networkID) override
	{
		return o_->RPC_(uniqueID, bitStream, priority, reliability, orderingChannel, shiftTimestamp, networkID);
	}

	void SetTrackFrequencyTable(bool b) override { o_->SetTrackFrequencyTable(b); }
	bool GetSendFrequencyTable(unsigned int outputFrequencyTable[256]) override { return o_->GetSendFrequencyTable(outputFrequencyTable); }
	float GetCompressionRatio() override { return o_->GetCompressionRatio(); }
	float GetDecompressionRatio() override { return o_->GetDecompressionRatio(); }
	void AttachPlugin(void* messageHandler) override { o_->AttachPlugin(messageHandler); }
	void DetachPlugin(void* messageHandler) override { o_->DetachPlugin(messageHandler); }
	BitStream* GetStaticServerData() override { return o_->GetStaticServerData(); }
	void SetStaticServerData(const char* data, const int length) override { o_->SetStaticServerData(data, length); }
	BitStream* GetStaticClientData(const PlayerID playerId) override { return o_->GetStaticClientData(playerId); }
	void SetStaticClientData(const PlayerID playerId, const char* data, const int length) override { o_->SetStaticClientData(playerId, data, length); }
	void SendStaticClientDataToServer() override { o_->SendStaticClientDataToServer(); }
	PlayerID GetServerID() override { return o_->GetServerID(); }
	PlayerID GetPlayerID() override { return o_->GetPlayerID(); }
	PlayerID GetInternalID() override { return o_->GetInternalID(); }
	const char* PlayerIDToDottedIP(const PlayerID playerId) override { return o_->PlayerIDToDottedIP(playerId); }
	void PushBackPacket(Packet* packet, bool pushAtHead) override { o_->PushBackPacket(packet, pushAtHead); }
	void SetRouterInterface(void* routerInterface) override { o_->SetRouterInterface(routerInterface); }
	void RemoveRouterInterface(void* routerInterface) override { o_->RemoveRouterInterface(routerInterface); }
	void SetTimeoutTime(unsigned int timeMS) override { o_->SetTimeoutTime(timeMS); }
	bool SetMTUSize(int size) override { return o_->SetMTUSize(size); }
	int GetMTUSize() override { return o_->GetMTUSize(); }
	void AllowConnectionResponseIPMigration(bool allow) override { o_->AllowConnectionResponseIPMigration(allow); }
	void AdvertiseSystem(const char* host, unsigned short remotePort, const char* data, int dataLength) override
	{
		o_->AdvertiseSystem(host, remotePort, data, dataLength);
	}
	RakNetStatisticsStruct* GetStatistics() override { return o_->GetStatistics(); }
	void ApplyNetworkSimulator(double maxSendBPS, unsigned short minExtraPing, unsigned short extraPingVariance) override
	{
		o_->ApplyNetworkSimulator(maxSendBPS, minExtraPing, extraPingVariance);
	}
	bool IsNetworkSimulatorActive() override { return o_->IsNetworkSimulatorActive(); }
	unsigned short GetPlayerIndex() override { return o_->GetPlayerIndex(); }

	RakClientInterface* original() const { return o_; }

private:
	bool sendJoin(int* id, BitStream* original, PacketPriority priority, PacketReliability reliability, char channel, bool shiftTimestamp)
	{
		const std::size_t bytes = static_cast<std::size_t>((original->numberOfBitsUsed + 7) / 8);
		std::vector<uint8_t> data(original->data, original->data + bytes);
		const std::vector<uint8_t> extra = g_events->joinPayload();
		data.insert(data.end(), extra.begin(), extra.end());
		BitStream stream = makeStream(data.data(), data.size());
		Log("join RPC sent with the voice hello (%u + %u bytes)", static_cast<unsigned>(bytes), static_cast<unsigned>(extra.size()));
		return o_->RPC(id, &stream, priority, reliability, channel, shiftTimestamp);
	}


	RakClientInterface* o_;
};
}

bool Install(void** slot, Events* events)
{
	if (!slot || !*slot)
	{
		return false;
	}
	auto* current = static_cast<RakClientInterface*>(*slot);
	if (current == g_proxy && g_proxy)
	{
		return true;
	}
	// Not again right after a disconnect: SA-MP may be destroying CNetGame.
	// If the game keeps running (a disconnect that was not an exit), voice
	// comes back after a few seconds.
	if (g_retiredAt && GetTickCount64() - g_retiredAt < kReinstallAfterMs)
	{
		return false;
	}
	g_events = events;
	g_slot = slot;
	auto* proxy = new Proxy(current);
	g_original = current;
	g_proxy = proxy;
	*slot = proxy;
	Log("RakClient proxy installed");
	return true;
}

bool Installed()
{
	return g_proxy != nullptr;
}

const void* ProxyVtable()
{
	// Every Proxy shares one vtable; read it from a throwaway instance.
	static const void* vtable = []
	{
		Proxy probe(nullptr);
		const void* table = *reinterpret_cast<void**>(&probe);
		return table;
	}();
	return vtable;
}

bool Connected()
{
	return g_proxy && g_connected;
}

bool Send(const uint8_t* data, std::size_t size, bool reliable)
{
	if (!g_original || !g_connected || !data || !size)
	{
		return false;
	}
	std::vector<uint8_t> copy(data, data + size);
	BitStream stream = makeStream(copy.data(), copy.size());
	return g_original->Send(&stream, HIGH_PRIORITY, reliable ? RELIABLE_ORDERED : UNRELIABLE_SEQUENCED, 0);
}

int Ping()
{
	return g_original && g_connected ? g_original->GetLastPing() : -1;
}
}

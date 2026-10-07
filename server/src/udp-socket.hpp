/*
 *  Voice Bridge for open.mp and SA-MP
 */

#pragma once

#include <cstdint>
#include <string>

namespace vbs
{
// Packs an IPv4 address (network byte order) and a port (host byte order)
// into one integer so it can live in a std::atomic.  Zero means "unknown".
inline uint64_t PackAddress(uint32_t ip, uint16_t port) noexcept
{
	return (uint64_t(1) << 48) | (uint64_t(ip) << 16) | port;
}

inline uint32_t AddressIp(uint64_t packed) noexcept
{
	return static_cast<uint32_t>(packed >> 16);
}

inline uint16_t AddressPort(uint64_t packed) noexcept
{
	return static_cast<uint16_t>(packed & 0xFFFF);
}

std::string FormatIp(uint32_t ip);
// Resolves a dotted address.  Returns 0 on failure.
uint32_t ParseIp(const std::string& text);

class UdpSocket
{
public:
	UdpSocket() = default;
	~UdpSocket();
	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	bool open(const std::string& bindIp, uint16_t port, std::string& error);
	void close();
	bool isOpen() const noexcept { return handle_ != kInvalid; }
	uint16_t localPort() const noexcept { return port_; }

	// Blocks for at most the configured timeout.  Returns the datagram size,
	// 0 on timeout and -1 when the socket was closed.
	int receive(uint8_t* buffer, int size, uint32_t& ip, uint16_t& port);
	bool sendTo(uint64_t address, const void* data, int size);

private:
#ifdef _WIN32
	using Handle = uintptr_t;
	static constexpr Handle kInvalid = ~Handle(0);
#else
	using Handle = int;
	static constexpr Handle kInvalid = -1;
#endif
	Handle handle_ = kInvalid;
	uint16_t port_ = 0;
	bool wsa_ = false;
};
}

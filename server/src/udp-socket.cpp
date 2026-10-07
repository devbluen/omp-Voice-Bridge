/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "udp-socket.hpp"
#include <cstring>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <mstcpip.h>
	#pragma comment(lib, "ws2_32.lib")
	#ifndef SIO_UDP_CONNRESET
		#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
	#endif
#else
	#include <arpa/inet.h>
	#include <cerrno>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <unistd.h>
#endif

namespace vbs
{
std::string FormatIp(uint32_t ip)
{
	const auto* bytes = reinterpret_cast<const uint8_t*>(&ip);
	return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." + std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

uint32_t ParseIp(const std::string& text)
{
	in_addr address {};
	if (inet_pton(AF_INET, text.c_str(), &address) != 1)
	{
		return 0;
	}
	return address.s_addr;
}

UdpSocket::~UdpSocket()
{
	close();
}

bool UdpSocket::open(const std::string& bindIp, uint16_t port, std::string& error)
{
	close();
#ifdef _WIN32
	WSADATA data {};
	if (const int result = WSAStartup(MAKEWORD(2, 2), &data))
	{
		error = "WSAStartup failed (" + std::to_string(result) + ")";
		return false;
	}
	wsa_ = true;
#endif

	const auto fail = [this, &error](const char* what)
	{
#ifdef _WIN32
		error = std::string(what) + " failed (" + std::to_string(WSAGetLastError()) + ")";
#else
		error = std::string(what) + " failed (" + std::strerror(errno) + ")";
#endif
		close();
		return false;
	};

	handle_ = static_cast<Handle>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
	if (handle_ == kInvalid)
	{
		return fail("socket");
	}

	const int sendBuffer = 4 * 1024 * 1024;
	const int receiveBuffer = 8 * 1024 * 1024;
	::setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));
	::setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));

#ifdef _WIN32
	// An ICMP "port unreachable" from one client must not make the next
	// recvfrom fail: that is what killed voice for everyone in SampVoice.
	BOOL reportReset = FALSE;
	DWORD returned = 0;
	WSAIoctl(handle_, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
	const DWORD timeout = 250;
	::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
	timeval timeout {};
	timeout.tv_usec = 250 * 1000;
	::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
	if (!bindIp.empty() && bindIp != "0.0.0.0")
	{
		const uint32_t ip = ParseIp(bindIp);
		if (!ip)
		{
			error = "invalid bind address '" + bindIp + "'";
			close();
			return false;
		}
		address.sin_addr.s_addr = ip;
	}

	if (::bind(handle_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		return fail("bind");
	}

	sockaddr_in local {};
	socklen_t localSize = sizeof(local);
	if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&local), &localSize) != 0)
	{
		return fail("getsockname");
	}
	port_ = ntohs(local.sin_port);
	return true;
}

void UdpSocket::close()
{
	if (handle_ != kInvalid)
	{
#ifdef _WIN32
		::closesocket(handle_);
#else
		::shutdown(handle_, SHUT_RDWR);
		::close(handle_);
#endif
		handle_ = kInvalid;
	}
	port_ = 0;
#ifdef _WIN32
	if (wsa_)
	{
		WSACleanup();
		wsa_ = false;
	}
#endif
}

int UdpSocket::receive(uint8_t* buffer, int size, uint32_t& ip, uint16_t& port)
{
	const Handle handle = handle_;
	if (handle == kInvalid)
	{
		return -1;
	}
	sockaddr_in from {};
	socklen_t fromSize = sizeof(from);
	const auto received = ::recvfrom(handle, reinterpret_cast<char*>(buffer), size, 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
	if (received < 0)
	{
#ifdef _WIN32
		const int code = WSAGetLastError();
		if (code == WSAENOTSOCK || code == WSAEINTR || code == WSANOTINITIALISED)
		{
			return handle_ == kInvalid ? -1 : 0;
		}
#else
		if (errno == EBADF)
		{
			return -1;
		}
#endif
		// Timeouts, resets and transient errors keep the loop running.
		return 0;
	}
	ip = from.sin_addr.s_addr;
	port = ntohs(from.sin_port);
	return static_cast<int>(received);
}

bool UdpSocket::sendTo(uint64_t packed, const void* data, int size)
{
	const Handle handle = handle_;
	if (handle == kInvalid || !packed)
	{
		return false;
	}
	sockaddr_in to {};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = AddressIp(packed);
	to.sin_port = htons(AddressPort(packed));
	return ::sendto(handle, reinterpret_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == size;
}
}

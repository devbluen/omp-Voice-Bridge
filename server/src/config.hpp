/*
 *  Voice Bridge for open.mp and SA-MP
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace vbs
{
struct Config
{
	// UDP port of the voice server.  0 selects the game port + 1, and only
	// when that port is busy does the plugin fall back to a random port.
	uint16_t port = 0;
	// Local address the voice socket binds to.  Empty uses the game server's
	// bind address, or every interface when the game server has none.
	std::string bind;
	// Host name or IP announced to Voice Bridge clients.  Only needed when
	// players reach the game through a proxy that does not forward UDP to
	// the same address.
	std::string publicHost;
	// Reject voice packets whose source IP differs from the game connection.
	bool strictIp = false;
	uint32_t bitrate = 24000;
	// Opus frame length requested from Voice Bridge clients.  SampVoice
	// clients can only decode 100 ms frames, so lower values make Voice
	// Bridge speakers inaudible to them.
	uint16_t frameMs = 100;
	// Relay voice through the game connection when UDP is blocked.
	bool tunnel = true;
	bool forceTunnel = false;
	uint32_t streamTickMs = 100;
	uint32_t positionRateMs = 100;
	uint32_t keepAliveMs = 5000;
	uint32_t maxPacketsPerSecond = 80;
	bool debug = false;
	// Voice log file.  Empty picks logs/voice-bridge.log (when a logs folder
	// exists, as on open.mp) or voice-bridge.log; "off" disables it.
	std::string logFile;

	// Client presentation (Voice Bridge clients only).
	bool allowVoiceActivation = false;
	bool showSpeakerList = true;
	bool showMicIcon = true;

	// Which voice clients may use voice chat.
	bool allowSampVoice = true;
	bool allowVoiceBridge = true;

	// Game server values used to derive defaults.
	uint16_t gamePort = 7777;
	std::string gameBind;
};

// Reads configuration values from the environment, an optional loader-specific
// source and finally server.cfg.
class ConfigSource
{
public:
	virtual ~ConfigSource() = default;
	virtual bool getString(const std::string& key, std::string& out) = 0;
	virtual bool getInt(const std::string& key, int& out) = 0;
	virtual bool getBool(const std::string& key, bool& out) = 0;
};

// server.cfg (or any "key value" per line file).
class ServerCfgSource final : public ConfigSource
{
public:
	explicit ServerCfgSource(const char* path = "server.cfg");
	bool getString(const std::string& key, std::string& out) override;
	bool getInt(const std::string& key, int& out) override;
	bool getBool(const std::string& key, bool& out) override;

private:
	std::map<std::string, std::string> values_;
};

Config LoadConfig(ConfigSource& source);
bool ParseBool(const std::string& value, bool fallback);
}

/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "config.hpp"
#include "log.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace vbs
{
namespace
{
std::string trim(const std::string& value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
	{
		return {};
	}
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool parseInt(const std::string& text, int& out)
{
	try
	{
		std::size_t used = 0;
		const int value = std::stoi(text, &used, 0);
		if (used == 0)
		{
			return false;
		}
		out = value;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

// One logical setting, looked up under every name it has been known by.
struct Key
{
	const char* env; // VOICE_BRIDGE_*
	const char* json; // open.mp config.json path (dotted)
	const char* cfg; // server.cfg key
	const char* legacyJson; // older sampvoice forks
	const char* legacyCfg;
};

bool lookupString(ConfigSource& source, const Key& key, std::string& out)
{
	if (key.env)
	{
		if (const char* value = std::getenv(key.env); value && *value)
		{
			out = value;
			return true;
		}
	}
	for (const char* name : { key.json, key.cfg, key.legacyJson, key.legacyCfg })
	{
		if (name && source.getString(name, out) && !out.empty())
		{
			return true;
		}
	}
	return false;
}

bool lookupInt(ConfigSource& source, const Key& key, int& out)
{
	if (key.env)
	{
		if (const char* value = std::getenv(key.env); value && *value)
		{
			if (parseInt(value, out))
			{
				return true;
			}
		}
	}
	for (const char* name : { key.json, key.cfg, key.legacyJson, key.legacyCfg })
	{
		if (name && source.getInt(name, out))
		{
			return true;
		}
	}
	return false;
}

bool lookupBool(ConfigSource& source, const Key& key, bool& out)
{
	if (key.env)
	{
		if (const char* value = std::getenv(key.env); value && *value)
		{
			out = ParseBool(value, out);
			return true;
		}
	}
	for (const char* name : { key.json, key.cfg, key.legacyJson, key.legacyCfg })
	{
		if (name && source.getBool(name, out))
		{
			return true;
		}
	}
	return false;
}

template <typename T>
void readRanged(ConfigSource& source, const Key& key, T& target, int minimum, int maximum, const char* label)
{
	int value = 0;
	if (!lookupInt(source, key, value))
	{
		return;
	}
	if (value < minimum || value > maximum)
	{
		LogWarning("%s must be between %d and %d (got %d); keeping %d", label, minimum, maximum, value, static_cast<int>(target));
		return;
	}
	target = static_cast<T>(value);
}
}

bool ParseBool(const std::string& value, bool fallback)
{
	const std::string text = lower(trim(value));
	if (text == "1" || text == "true" || text == "yes" || text == "on")
	{
		return true;
	}
	if (text == "0" || text == "false" || text == "no" || text == "off")
	{
		return false;
	}
	return fallback;
}

ServerCfgSource::ServerCfgSource(const char* path)
{
	std::ifstream file(path);
	std::string line;
	while (std::getline(file, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#' || (line.size() > 1 && line[0] == '/' && line[1] == '/'))
		{
			continue;
		}
		const auto separator = line.find_first_of(" \t");
		const std::string key = separator == std::string::npos ? line : line.substr(0, separator);
		std::string value = separator == std::string::npos ? std::string() : trim(line.substr(separator + 1));
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		{
			value = value.substr(1, value.size() - 2);
		}
		values_[key] = value;
	}
}

bool ServerCfgSource::getString(const std::string& key, std::string& out)
{
	const auto it = values_.find(key);
	if (it == values_.end())
	{
		return false;
	}
	out = it->second;
	return true;
}

bool ServerCfgSource::getInt(const std::string& key, int& out)
{
	const auto it = values_.find(key);
	return it != values_.end() && parseInt(it->second, out);
}

bool ServerCfgSource::getBool(const std::string& key, bool& out)
{
	const auto it = values_.find(key);
	if (it == values_.end())
	{
		return false;
	}
	out = ParseBool(it->second, out);
	return true;
}

Config LoadConfig(ConfigSource& source)
{
	Config config;

	const Key port { "VOICE_BRIDGE_PORT", "voice_bridge.port", "voice_port", "sampvoice.port", "sv_voice_port" };
	const Key bind { "VOICE_BRIDGE_BIND", "voice_bridge.bind", "voice_bind", "sampvoice.ip", nullptr };
	const Key publicHost { "VOICE_BRIDGE_PUBLIC_HOST", "voice_bridge.public_host", "voice_public_host", nullptr, nullptr };
	const Key strictIp { "VOICE_BRIDGE_STRICT_IP", "voice_bridge.strict_ip", "voice_strict_ip", nullptr, nullptr };
	const Key bitrate { "VOICE_BRIDGE_BITRATE", "voice_bridge.bitrate", "voice_bitrate", nullptr, nullptr };
	const Key gain { "VOICE_BRIDGE_GAIN", "voice_bridge.gain", "voice_gain", nullptr, nullptr };
	const Key frameMs { "VOICE_BRIDGE_FRAME_MS", "voice_bridge.frame_ms", "voice_frame_ms", nullptr, nullptr };
	const Key tunnel { "VOICE_BRIDGE_TUNNEL", "voice_bridge.tunnel", "voice_tunnel", nullptr, nullptr };
	const Key forceTunnel { "VOICE_BRIDGE_FORCE_TUNNEL", "voice_bridge.force_tunnel", "voice_force_tunnel", nullptr, nullptr };
	const Key streamTick { "VOICE_BRIDGE_STREAM_TICK_MS", "voice_bridge.stream_tick_ms", "voice_stream_tick_ms", nullptr, nullptr };
	const Key positionRate { "VOICE_BRIDGE_POSITION_RATE_MS", "voice_bridge.position_rate_ms", "voice_position_rate_ms", nullptr, nullptr };
	const Key keepAlive { "VOICE_BRIDGE_KEEPALIVE_MS", "voice_bridge.keepalive_ms", "voice_keepalive_ms", nullptr, nullptr };
	const Key maxPackets { "VOICE_BRIDGE_MAX_PACKETS", "voice_bridge.max_packets_per_second", "voice_max_packets_per_second", nullptr, nullptr };
	const Key debug { "VOICE_BRIDGE_DEBUG", "voice_bridge.debug", "voice_debug", nullptr, nullptr };
	const Key logFile { "VOICE_BRIDGE_LOG_FILE", "voice_bridge.log_file", "voice_log_file", nullptr, nullptr };
	const Key voiceActivation { "VOICE_BRIDGE_ALLOW_VOICE_ACTIVATION", "voice_bridge.allow_voice_activation", "voice_allow_voice_activation", nullptr, nullptr };
	const Key speakerList { "VOICE_BRIDGE_SHOW_SPEAKER_LIST", "voice_bridge.show_speaker_list", "voice_show_speaker_list", nullptr, nullptr };
	const Key micIcon { "VOICE_BRIDGE_SHOW_MIC_ICON", "voice_bridge.show_mic_icon", "voice_show_mic_icon", nullptr, nullptr };
	const Key headIcons { "VOICE_BRIDGE_SHOW_HEAD_ICONS", "voice_bridge.show_head_icons", "voice_show_head_icons", nullptr, nullptr };
	const Key allowSampVoice { "VOICE_BRIDGE_ALLOW_SAMPVOICE", "voice_bridge.allow_sampvoice", "voice_allow_sampvoice", nullptr, nullptr };
	const Key allowVoiceBridge { "VOICE_BRIDGE_ALLOW_VOICEBRIDGE", "voice_bridge.allow_voicebridge", "voice_allow_voicebridge", nullptr, nullptr };
	const Key gamePort { nullptr, "network.port", "port", nullptr, nullptr };
	const Key gameBind { nullptr, "network.bind", "bind", nullptr, nullptr };

	readRanged(source, port, config.port, 0, 65535, "voice_port");
	lookupString(source, bind, config.bind);
	lookupString(source, publicHost, config.publicHost);
	lookupBool(source, strictIp, config.strictIp);
	readRanged(source, bitrate, config.bitrate, 6000, 128000, "voice_bitrate");
	readRanged(source, gain, config.gainPercent, 0, 2000, "voice_gain");

	int frame = config.frameMs;
	if (lookupInt(source, frameMs, frame))
	{
		if (frame == 20 || frame == 40 || frame == 60 || frame == 100)
		{
			config.frameMs = static_cast<uint16_t>(frame);
		}
		else
		{
			LogWarning("voice_frame_ms must be 20, 40, 60 or 100 (got %d); keeping %u", frame, config.frameMs);
		}
	}

	lookupBool(source, tunnel, config.tunnel);
	lookupBool(source, forceTunnel, config.forceTunnel);
	readRanged(source, streamTick, config.streamTickMs, 20, 2000, "voice_stream_tick_ms");
	readRanged(source, positionRate, config.positionRateMs, 30, 2000, "voice_position_rate_ms");
	readRanged(source, keepAlive, config.keepAliveMs, 1000, 60000, "voice_keepalive_ms");
	readRanged(source, maxPackets, config.maxPacketsPerSecond, 10, 1000, "voice_max_packets_per_second");
	lookupBool(source, debug, config.debug);
	lookupString(source, logFile, config.logFile);
	lookupBool(source, voiceActivation, config.allowVoiceActivation);
	lookupBool(source, speakerList, config.showSpeakerList);
	lookupBool(source, micIcon, config.showMicIcon);
	lookupBool(source, headIcons, config.showHeadIcons);
	lookupBool(source, allowSampVoice, config.allowSampVoice);
	lookupBool(source, allowVoiceBridge, config.allowVoiceBridge);
	readRanged(source, gamePort, config.gamePort, 1, 65535, "port");
	lookupString(source, gameBind, config.gameBind);

	if (config.forceTunnel && !config.tunnel)
	{
		LogWarning("voice_force_tunnel requires voice_tunnel; enabling the tunnel");
		config.tunnel = true;
	}
	if (config.frameMs != 100)
	{
		LogWarning("voice_frame_ms is %u: players using the original SampVoice client will not hear Voice Bridge speakers", config.frameMs);
	}
	return config;
}
}

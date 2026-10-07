/*
 *  Voice Bridge client
 */

#include "overlay.hpp"
#include "audio.hpp"
#include "rakclient.hpp"
#include "log.hpp"
#include "samp.hpp"
#include "settings.hpp"
#include "version.hpp"
#include "voice.hpp"
#include <vb-protocol.hpp>
#include <d3d9.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <cmath>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace vbc::overlay
{
namespace
{
constexpr uintptr_t kGameDevice = 0xC97C28;
constexpr std::size_t kResetIndex = 16;
constexpr std::size_t kPresentIndex = 17;

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

PresentFn g_present = nullptr;
ResetFn g_reset = nullptr;
void** g_vtable = nullptr;
HWND g_window = nullptr;
WNDPROC g_windowProc = nullptr;
bool g_imgui = false;
IDirect3DDevice9* g_device = nullptr;
std::atomic<uint64_t> g_lastPresent { 0 };
bool g_audioReady = false;
std::atomic<bool> g_menu { false };
bool g_cursorByUs = false;
bool g_portuguese = false;
bool g_systemPortuguese = false;

// Settings::language: 0 = follow Windows, 1 = English, 2 = Portuguese.
void applyLanguage()
{
	const int language = GetSettings().language;
	g_portuguese = language == 2 || (language == 0 && g_systemPortuguese);
}
int g_menuKeyWasDown = 0;

// --- Text -----------------------------------------------------------------

const char* tr(const char* english, const char* portuguese)
{
	return g_portuguese ? portuguese : english;
}

// SA-MP strings are Windows-1252; ImGui wants UTF-8.
std::string toUtf8(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (unsigned char c : text)
	{
		if (c < 0x80)
		{
			out.push_back(static_cast<char>(c));
		}
		else
		{
			out.push_back(static_cast<char>(0xC0 | (c >> 6)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

std::string keyName(uint8_t key)
{
	switch (key)
	{
	case VK_LBUTTON:
		return tr("Left mouse", "Mouse esquerdo");
	case VK_RBUTTON:
		return tr("Right mouse", "Mouse direito");
	case VK_MBUTTON:
		return tr("Middle mouse", "Mouse meio");
	case VK_XBUTTON1:
		return "Mouse 4";
	case VK_XBUTTON2:
		return "Mouse 5";
	default:
		break;
	}
	char name[64] {};
	const UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
	if (scan && GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)) > 0)
	{
		return toUtf8(name);
	}
	return "0x" + std::to_string(key);
}

ImU32 streamColor(uint32_t argb)
{
	if (!argb)
	{
		return IM_COL32(230, 230, 230, 255);
	}
	const uint8_t a = static_cast<uint8_t>(argb >> 24);
	return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, a ? a : 255);
}

ImU32 rgbaColor(uint32_t rgba)
{
	const uint8_t a = static_cast<uint8_t>(rgba & 0xFF);
	return IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, a ? a : 255);
}

// --- Drawing --------------------------------------------------------------

void drawMicrophone(ImDrawList* draw, ImVec2 center, float size, ImU32 color, bool crossed)
{
	const float w = size * 0.32f;
	const float h = size * 0.5f;
	draw->AddRectFilled(ImVec2(center.x - w / 2, center.y - h), ImVec2(center.x + w / 2, center.y + h * 0.15f), color, w / 2);
	draw->AddBezierQuadratic(ImVec2(center.x - w, center.y - h * 0.2f), ImVec2(center.x, center.y + h * 0.9f),
		ImVec2(center.x + w, center.y - h * 0.2f), color, size * 0.06f);
	draw->AddLine(ImVec2(center.x, center.y + h * 0.35f), ImVec2(center.x, center.y + h * 0.7f), color, size * 0.06f);
	draw->AddLine(ImVec2(center.x - w * 0.7f, center.y + h * 0.7f), ImVec2(center.x + w * 0.7f, center.y + h * 0.7f), color, size * 0.06f);
	if (crossed)
	{
		draw->AddLine(ImVec2(center.x - size * 0.4f, center.y - size * 0.45f), ImVec2(center.x + size * 0.4f, center.y + size * 0.45f),
			IM_COL32(230, 60, 60, 255), size * 0.08f);
	}
}

bool g_draggingMic = false;

ImVec2 micIconCenter(const ImVec2& display, float scale)
{
	const Settings& settings = GetSettings();
	const float margin = 24.f * scale;
	return ImVec2(std::clamp(display.x * static_cast<float>(settings.micIconX) / 1000.f, margin, display.x - margin),
		std::clamp(display.y * static_cast<float>(settings.micIconY) / 1000.f, margin, display.y - margin));
}

// With the menu open the microphone icon can be dragged to any place.
void dragMicIcon(const ImVec2& center, float size, const ImVec2& display)
{
	ImGuiIO& io = ImGui::GetIO();
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	const float dx = io.MousePos.x - center.x;
	const float dy = io.MousePos.y - center.y;
	const bool hovered = dx * dx + dy * dy <= size * size * 0.5f && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		g_draggingMic = true;
	}
	if (g_draggingMic)
	{
		Settings& settings = GetSettings();
		settings.micIconX = std::clamp(static_cast<int>(io.MousePos.x / display.x * 1000.f), 0, 1000);
		settings.micIconY = std::clamp(static_cast<int>(io.MousePos.y / display.y * 1000.f), 0, 1000);
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			g_draggingMic = false;
			SaveSettings();
		}
	}
	const bool active = hovered || g_draggingMic;
	draw->AddCircle(center, size * 0.72f, active ? IM_COL32(90, 170, 255, 255) : IM_COL32(90, 170, 255, 140), 0, 2.f);
	if (active)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	}
}

void drawHud(const VoiceStatus& status)
{
	const Settings& settings = GetSettings();
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const float scale = display.y / 720.f * static_cast<float>(settings.uiScale) / 100.f;
	Audio& audio = Audio::Get();

	if (status.server != ServerKind::None && status.showMicIcon && settings.showMicIcon)
	{
		const ImVec2 center = micIconCenter(display, scale);
		const float size = 34.f * scale;
		ImU32 color = IM_COL32(200, 200, 200, 170);
		bool crossed = false;
		if (status.muted || !settings.micEnabled || !audio.hasMicrophone())
		{
			color = IM_COL32(200, 200, 200, 120);
			crossed = true;
		}
		else if (status.transmitting)
		{
			const float level = std::clamp((audio.micLevel() + 60.f) / 50.f, 0.f, 1.f);
			color = IM_COL32(60, 220, 90, 255);
			draw->AddCircleFilled(center, size * (0.62f + 0.25f * level), IM_COL32(60, 220, 90, 60));
		}
		draw->AddCircleFilled(center, size * 0.6f, IM_COL32(15, 15, 15, 140));
		drawMicrophone(draw, center, size * 0.8f, color, crossed);
		if (g_menu)
		{
			dragMicIcon(center, size, display);
		}
		if (status.transport == vb::transport::none)
		{
			draw->AddText(ImVec2(center.x + size * 0.45f, center.y + size * 0.15f), IM_COL32(255, 80, 80, 255), "!");
		}
	}

	if (status.showSpeakerList && settings.showSpeakerList)
	{
		const auto speakers = audio.speakers();
		float y = display.y * 0.30f;
		const float x = display.x * 0.015f;
		const float line = ImGui::GetFontSize() * 1.35f;
		for (const SpeakerInfo& speaker : speakers)
		{
			const std::string name = toUtf8(VoiceClient::Get().playerName(speaker.player));
			const std::string text = speaker.stream.empty() ? name : name + "  [" + toUtf8(speaker.stream) + "]";
			const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
			draw->AddRectFilled(ImVec2(x, y), ImVec2(x + textSize.x + line * 1.4f, y + line), IM_COL32(10, 10, 10, 150), line * 0.25f);
			const float level = std::clamp((speaker.level + 50.f) / 40.f, 0.15f, 1.f);
			draw->AddCircleFilled(ImVec2(x + line * 0.55f, y + line * 0.5f), line * 0.22f * (0.7f + 0.5f * level), streamColor(speaker.color));
			draw->AddText(ImVec2(x + line * 1.1f, y + (line - textSize.y) * 0.5f), IM_COL32(245, 245, 245, 255), text.c_str());
			y += line + 3.f;
		}
	}

	float notificationY = display.y * 0.12f;
	for (const Notification& notification : VoiceClient::Get().notifications())
	{
		const std::string text = notification.utf8 ? notification.text : toUtf8(notification.text);
		const ImVec2 size = ImGui::CalcTextSize(text.c_str());
		const ImVec2 position((display.x - size.x) * 0.5f, notificationY);
		draw->AddRectFilled(ImVec2(position.x - 12.f, position.y - 6.f), ImVec2(position.x + size.x + 12.f, position.y + size.y + 6.f),
			IM_COL32(10, 10, 10, 190), 6.f);
		draw->AddText(position, rgbaColor(notification.color), text.c_str());
		notificationY += size.y + 18.f;
	}
}

void setMenu(bool open)
{
	if (open == g_menu)
	{
		return;
	}
	g_menu = open;
	VoiceClient::Get().setMenuOpen(open);
	ImGui::GetIO().MouseDrawCursor = open;
	if (open)
	{
		g_cursorByUs = samp::SetCursor(true);
	}
	else
	{
		if (g_cursorByUs)
		{
			samp::SetCursor(false);
		}
		g_cursorByUs = false;
		Audio::Get().setMicTest(false);
		SaveSettings();
	}
}

// --- Menu -----------------------------------------------------------------

enum class Page
{
	Status,
	Sound,
	Microphone,
	Players,
	Interface,
	About,
};

struct RecentSpeaker
{
	uint64_t lastHeard = 0;
	uint32_t color = 0;
	std::string stream;
};

Page g_page = Page::Status;
bool g_previewMode = false;
bool g_waitingKey = false;
uint64_t g_linkCopiedAt = 0;
ImFont* g_titleFont = nullptr;
std::map<uint16_t, RecentSpeaker> g_recent;

const ImVec4 kAccent(0.16f, 0.78f, 0.56f, 1.f);
const ImVec4 kMuted(0.62f, 0.65f, 0.70f, 1.f);
const ImVec4 kGood(0.25f, 0.85f, 0.45f, 1.f);
const ImVec4 kWarn(1.f, 0.72f, 0.25f, 1.f);
const ImVec4 kBad(1.f, 0.38f, 0.38f, 1.f);

void trackSpeakers(const VoiceStatus& status)
{
	if (status.server == ServerKind::None)
	{
		g_recent.clear();
		return;
	}
	const uint64_t now = GetTickCount64();
	for (const SpeakerInfo& speaker : Audio::Get().speakers())
	{
		RecentSpeaker& recent = g_recent[speaker.player];
		recent.lastHeard = now;
		recent.color = speaker.color;
		recent.stream = speaker.stream;
	}
	for (auto it = g_recent.begin(); it != g_recent.end();)
	{
		it = now - it->second.lastHeard > 15 * 60 * 1000 ? g_recent.erase(it) : std::next(it);
	}
}

// Switch drawn like a phone toggle.
bool toggle(const char* label, bool* value)
{
	ImGui::PushID(label);
	const float height = ImGui::GetFrameHeight() * 0.8f;
	const float width = height * 1.8f;
	const ImVec2 position = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::InvisibleButton("##switch", ImVec2(width, height));
	if (clicked)
	{
		*value = !*value;
	}
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 track = ImGui::GetColorU32(*value ? kAccent : ImVec4(0.30f, 0.32f, 0.36f, 1.f));
	draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height), track, height * 0.5f);
	const float radius = height * 0.5f - 2.f;
	const float knob = *value ? width - radius - 2.f : radius + 2.f;
	draw->AddCircleFilled(ImVec2(position.x + knob, position.y + height * 0.5f), radius, IM_COL32(245, 245, 245, 255));
	if (!(label[0] == '#' && label[1] == '#'))
	{
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
	}
	ImGui::PopID();
	return clicked;
}

void sectionTitle(const char* text)
{
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::Separator();
	ImGui::Spacing();
}

void infoRow(const char* label, const std::string& value, const ImVec4& color = ImVec4(0.92f, 0.93f, 0.95f, 1.f))
{
	ImGui::TextColored(kMuted, "%s", label);
	ImGui::SameLine(190.f);
	ImGui::TextColored(color, "%s", value.c_str());
}

// Microphone level with the noise gate / activation threshold drawn on top.
void levelMeter(float level, int threshold, bool showThreshold)
{
	const ImVec2 position = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float height = 14.f;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const auto x = [&](float db) { return position.x + width * std::clamp((db + 90.f) / 90.f, 0.f, 1.f); };
	draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height), IM_COL32(35, 38, 44, 255), 4.f);
	draw->AddRectFilledMultiColor(position, ImVec2(x(level), position.y + height), IM_COL32(40, 200, 120, 255), IM_COL32(230, 200, 60, 255),
		IM_COL32(230, 200, 60, 255), IM_COL32(40, 200, 120, 255));
	if (showThreshold)
	{
		const float marker = x(static_cast<float>(threshold));
		draw->AddLine(ImVec2(marker, position.y - 3.f), ImVec2(marker, position.y + height + 3.f), IM_COL32(255, 255, 255, 230), 2.f);
	}
	ImGui::Dummy(ImVec2(width, height + 4.f));
}

void statusCard(const VoiceStatus& status)
{
	const char* title = tr("No voice server", "Servidor sem chat de voz");
	const char* detail = tr("This server does not use Voice Bridge or SampVoice.", "Este servidor não usa Voice Bridge nem SampVoice.");
	ImVec4 color = kMuted;
	if (!g_previewMode && !rak::Installed())
	{
		title = tr("Waiting for the game", "Aguardando o jogo");
		detail = tr("Voice starts when you join a server.", "A voz inicia quando você entra em um servidor.");
	}
	else if (!g_previewMode && !rak::Connected())
	{
		title = tr("Not connected", "Fora de um servidor");
		detail = tr("Join a server to use voice chat.", "Entre em um servidor para usar o chat de voz.");
	}
	else if (status.server != ServerKind::None)
	{
		if (status.transport != vb::transport::none)
		{
			title = tr("Connected", "Conectado");
			detail = tr("Voice chat is working.", "O chat de voz está funcionando.");
			color = kGood;
		}
		else
		{
			title = tr("Connecting...", "Conectando...");
			detail = tr("Connecting to the voice chat.", "Conectando ao chat de voz.");
			color = kWarn;
		}
	}

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.155f, 1.f));
	ImGui::BeginChild("card", ImVec2(0.f, 84.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetWindowPos();
	const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(GetTickCount64() % 2000) / 2000.f * 6.2831f);
	draw->AddCircleFilled(ImVec2(origin.x + 32.f, origin.y + 42.f), 16.f * pulse, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.25f)));
	draw->AddCircleFilled(ImVec2(origin.x + 32.f, origin.y + 42.f), 9.f, ImGui::GetColorU32(color));
	ImGui::SetCursorPos(ImVec2(62.f, 14.f));
	if (g_titleFont)
	{
		ImGui::PushFont(g_titleFont);
	}
	ImGui::TextColored(color, "%s", title);
	if (g_titleFont)
	{
		ImGui::PopFont();
	}
	ImGui::SetCursorPos(ImVec2(62.f, 50.f));
	ImGui::TextColored(kMuted, "%s", detail);
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void pageStatus(const VoiceStatus& status)
{
	statusCard(status);
	sectionTitle(tr("Connection", "Conexão"));
	const char* server = status.server == ServerKind::VoiceBridge ? "Voice Bridge" : status.server == ServerKind::SampVoice ? "SampVoice" : "-";
	infoRow(tr("Voice server", "Servidor de voz"), server);
	infoRow(tr("Voice chat", "Chat de voz"), status.transport != vb::transport::none ? tr("connected", "conectado") : tr("connecting...", "conectando..."),
		status.transport != vb::transport::none ? kGood : kWarn);
	const int ping = rak::Ping();
	infoRow("Ping", ping >= 0 ? std::to_string(ping) + " ms" : "-");
	infoRow(tr("SA-MP version", "Versão do SA-MP"), samp::VersionName());

	sectionTitle(tr("Talking", "Falar"));
	Audio& audio = Audio::Get();
	if (!audio.hasMicrophone())
	{
		infoRow(tr("Microphone", "Microfone"), tr("not found", "não encontrado"), kBad);
	}
	else
	{
		infoRow(tr("Microphone", "Microfone"), toUtf8(audio.microphoneName()));
	}
	if (status.muted)
	{
		infoRow(tr("State", "Estado"), tr("muted by the server", "silenciado pelo servidor"), kBad);
	}
	else if (status.transmitting)
	{
		infoRow(tr("State", "Estado"), tr("talking", "falando"), kGood);
	}
	else if (status.recordingForced)
	{
		infoRow(tr("State", "Estado"), tr("open microphone (server)", "microfone aberto (servidor)"), kWarn);
	}
	else
	{
		infoRow(tr("State", "Estado"), tr("quiet", "em silêncio"));
	}
	std::string keys;
	for (uint8_t key : status.keys)
	{
		keys += (keys.empty() ? "" : ", ") + keyName(key);
	}
	infoRow(tr("Push to talk", "Tecla para falar"), keys.empty() ? tr("defined by the server", "definida pelo servidor") : keys);
}

void pageAbout()
{
	sectionTitle("Voice Bridge");
	infoRow(tr("Version", "Versão"), "v" VOICE_BRIDGE_VERSION);
	infoRow(tr("Works with", "Funciona com"), tr("Voice Bridge and SampVoice servers", "servidores Voice Bridge e SampVoice"));
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "%s", VOICE_BRIDGE_URL);
	ImGui::SameLine();
	if (ImGui::SmallButton(tr("Copy link", "Copiar link")))
	{
		ImGui::SetClipboardText(VOICE_BRIDGE_URL);
		g_linkCopiedAt = GetTickCount64();
	}
	if (g_linkCopiedAt && GetTickCount64() - g_linkCopiedAt < 2000)
	{
		ImGui::SameLine();
		ImGui::TextColored(kGood, "%s", tr("copied", "copiado"));
	}

	sectionTitle(tr("Credits", "Créditos"));
	infoRow(VOICE_BRIDGE_AUTHOR, tr("creator", "criador"));
	infoRow("MMV (Ramon)", tr("testing and ideas", "testes e ideias"));
	infoRow("MOR (CyberMor)", tr("original SampVoice protocol", "protocolo original do SampVoice"));
}

bool pageSound()
{
	Settings& settings = GetSettings();
	bool changed = false;
	sectionTitle(tr("Voice chat", "Chat de voz"));
	changed |= toggle(tr("Hear other players", "Ouvir os outros jogadores"), &settings.soundEnabled);
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "%s", tr("Volume", "Volume"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##volume", &settings.volume, 0, 200, "%d%%");
	ImGui::TextColored(kMuted, "%s", tr("Above 100% the voices are amplified.", "Acima de 100% as vozes são amplificadas."));

	sectionTitle(tr("3D sound", "Som 3D"));
	const char* modes[] = {
		tr("Realistic (direction, distance and room)", "Realista (direção, distância e ambiente)"),
		tr("Simple stereo", "Estéreo simples"),
		tr("Off (same in both ears)", "Desligado (igual nos dois ouvidos)"),
	};
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::Combo("##spatial", &settings.spatialMode, modes, IM_ARRAYSIZE(modes));
	if (settings.spatialMode == 0)
	{
		ImGui::Spacing();
		ImGui::TextColored(kMuted, "%s", tr("Room echo (makes far voices sound far)", "Eco do ambiente (faz vozes distantes soarem longe)"));
		ImGui::SetNextItemWidth(-1.f);
		changed |= ImGui::SliderInt("##room", &settings.roomAmount, 0, 100, "%d%%");
	}
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "%s", tr("Distance fade", "Queda com a distância"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##distance", &settings.distanceStrength, 50, 200, "%d%%");
	ImGui::Spacing();
	changed |= toggle(tr("Directions follow my character (not the camera)", "Direção segue o personagem (não a câmera)"), &settings.orientationFromCharacter);
	changed |= toggle(tr("Swap left and right", "Inverter esquerda e direita"), &settings.swapChannels);

	sectionTitle(tr("Sounds", "Sons"));
	changed |= toggle(tr("Beep when the talk key is pressed", "Bipe ao apertar a tecla de falar"), &settings.pushToTalkSound);
	return changed;
}

bool pageMicrophone(const VoiceStatus& status)
{
	Settings& settings = GetSettings();
	Audio& audio = Audio::Get();
	bool changed = false;

	sectionTitle(tr("Device", "Dispositivo"));
	changed |= toggle(tr("Use the microphone", "Usar o microfone"), &settings.micEnabled);
	ImGui::Spacing();
	const std::string current = audio.microphoneName();
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::BeginCombo("##device", current.empty() ? tr("No microphone", "Nenhum microfone") : toUtf8(current).c_str()))
	{
		for (const std::string& device : audio.microphones())
		{
			if (ImGui::Selectable(toUtf8(device).c_str(), device == current))
			{
				settings.micDevice = device;
				audio.openMicrophone(device);
				changed = true;
			}
		}
		ImGui::EndCombo();
	}

	sectionTitle(tr("Level", "Nível"));
	levelMeter(audio.micLevel(), settings.noiseGate, settings.noiseGate > -90);
	ImGui::TextColored(kMuted, "%s", tr("The white line is the noise gate: quieter sounds are not sent.", "A linha branca é o filtro de ruído: sons mais baixos não são enviados."));
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "%s", tr("Gain", "Ganho"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##gain", &settings.micGain, 0, 400, "%d%%");
	ImGui::TextColored(kMuted, "%s", tr("Noise gate", "Filtro de ruído"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##gate", &settings.noiseGate, -90, -20, settings.noiseGate <= -90 ? tr("off", "desligado") : "%d dB");
	ImGui::Spacing();
	bool test = audio.micTest();
	if (toggle(tr("Hear my own voice (test)", "Ouvir minha própria voz (teste)"), &test))
	{
		audio.setMicTest(test);
	}

	if (status.allowVoiceActivation)
	{
		sectionTitle(tr("Voice activation", "Ativação por voz"));
		changed |= toggle(tr("Talk without holding the key", "Falar sem segurar a tecla"), &settings.voiceActivation);
		if (settings.voiceActivation)
		{
			levelMeter(audio.micLevel(), settings.voiceActivationLevel, true);
			ImGui::SetNextItemWidth(-1.f);
			changed |= ImGui::SliderInt("##activation", &settings.voiceActivationLevel, -80, -5, tr("starts at %d dB", "ativa em %d dB"));
		}
	}
	return changed;
}

void pagePlayers(const VoiceStatus& status)
{
	Audio& audio = Audio::Get();
	sectionTitle(tr("Players heard recently", "Jogadores ouvidos recentemente"));
	if (!status.showSpeakerList)
	{
		ImGui::TextColored(kMuted, "%s", tr("This server hides who is talking.", "Este servidor oculta quem está falando."));
		return;
	}
	if (g_recent.empty())
	{
		ImGui::TextColored(kMuted, "%s", tr("Nobody has talked yet.", "Ninguém falou ainda."));
		return;
	}
	const uint64_t now = GetTickCount64();
	if (ImGui::BeginTable("players", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn(tr("Player", "Jogador"), ImGuiTableColumnFlags_WidthStretch, 2.f);
		ImGui::TableSetupColumn(tr("Channel", "Canal"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn(tr("Volume", "Volume"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn(tr("Mute", "Silenciar"), ImGuiTableColumnFlags_WidthFixed, 80.f);
		ImGui::TableHeadersRow();
		for (const auto& entry : g_recent)
		{
			const uint16_t player = entry.first;
			ImGui::PushID(player);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const bool talking = now - entry.second.lastHeard < 400;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImVec2 dot = ImGui::GetCursorScreenPos();
			draw->AddCircleFilled(ImVec2(dot.x + 6.f, dot.y + ImGui::GetFrameHeight() * 0.5f), talking ? 6.f : 4.f,
				talking ? ImGui::GetColorU32(kGood) : streamColor(entry.second.color));
			ImGui::Dummy(ImVec2(16.f, 0.f));
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s (%u)", toUtf8(VoiceClient::Get().playerName(player)).c_str(), player);
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(kMuted, "%s", entry.second.stream.empty() ? "-" : toUtf8(entry.second.stream).c_str());
			ImGui::TableNextColumn();
			float volume = audio.playerVolume(player) * 100.f;
			ImGui::SetNextItemWidth(-1.f);
			if (ImGui::SliderFloat("##volume", &volume, 0.f, 300.f, "%.0f%%"))
			{
				audio.setPlayerVolume(player, volume / 100.f);
			}
			ImGui::TableNextColumn();
			bool muted = audio.playerMuted(player);
			if (toggle("##mute", &muted))
			{
				audio.setPlayerMuted(player, muted);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

bool pageInterface()
{
	Settings& settings = GetSettings();
	bool changed = false;
	sectionTitle(tr("Language", "Idioma"));
	const char* languages[] = { tr("Automatic (system)", "Automático (sistema)"), "English", "Português (Brasil)" };
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::Combo("##language", &settings.language, languages, IM_ARRAYSIZE(languages)))
	{
		applyLanguage();
		changed = true;
	}

	sectionTitle(tr("On screen", "Na tela"));
	changed |= toggle(tr("Show who is talking", "Mostrar quem está falando"), &settings.showSpeakerList);
	changed |= toggle(tr("Show the microphone icon", "Mostrar o ícone do microfone"), &settings.showMicIcon);
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "%s", tr("Size", "Tamanho"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##scale", &settings.uiScale, 50, 200, "%d%%");

	sectionTitle(tr("Microphone icon position", "Posição do ícone do microfone"));
	ImGui::TextColored(kMuted, "%s", tr("Drag the icon on the screen while this menu is open, or use the sliders.",
		"Arraste o ícone na tela enquanto este menu está aberto, ou use os controles."));
	ImGui::TextColored(kMuted, "%s", tr("Horizontal", "Horizontal"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##micx", &settings.micIconX, 0, 1000, "%d");
	ImGui::TextColored(kMuted, "%s", tr("Vertical", "Vertical"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##micy", &settings.micIconY, 0, 1000, "%d");

	sectionTitle(tr("Menu key", "Tecla do menu"));
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(keyName(static_cast<uint8_t>(settings.menuKey)).c_str());
	ImGui::SameLine(190.f);
	if (g_waitingKey)
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(kWarn, "%s", tr("Press a key... (Esc cancels)", "Pressione uma tecla... (Esc cancela)"));
	}
	else if (ImGui::Button(tr("Change", "Alterar")))
	{
		g_waitingKey = true;
	}

	sectionTitle(tr("Settings", "Configurações"));
	if (ImGui::Button(tr("Restore defaults", "Restaurar padrões")))
	{
		const std::string device = settings.micDevice;
		settings = Settings {};
		settings.micDevice = device;
		applyLanguage();
		changed = true;
	}
	return changed;
}

void navButton(const char* label, Page page)
{
	const bool selected = g_page == page;
	ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f) : ImVec4(0.f, 0.f, 0.f, 0.f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.06f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
	ImGui::PushStyleColor(ImGuiCol_Text, selected ? kAccent : ImVec4(0.85f, 0.87f, 0.90f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));
	if (ImGui::Button(label, ImVec2(-1.f, 36.f)))
	{
		g_page = page;
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
}

void waitForMenuKey()
{
	if (!g_waitingKey)
	{
		return;
	}
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
	{
		g_waitingKey = false;
		return;
	}
	for (int key = 0x08; key <= 0xFE; ++key)
	{
		if (key == VK_ESCAPE || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU)
		{
			continue;
		}
		if (GetAsyncKeyState(key) & 0x8000)
		{
			GetSettings().menuKey = key;
			g_menuKeyWasDown = 1; // the new key must not toggle the menu right away
			g_waitingKey = false;
			SaveSettings();
			return;
		}
	}
}

void drawMenu(const VoiceStatus& status)
{
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2(720.f, 520.f), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	const bool visible = ImGui::Begin("##voicebridge", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
	ImGui::PopStyleVar();
	if (!visible)
	{
		ImGui::End();
		return;
	}
	waitForMenuKey();
	bool changed = false;

	// Sidebar
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.08f, 0.095f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 18.f));
	ImGui::BeginChild("sidebar", ImVec2(190.f, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding);
	if (g_titleFont)
	{
		ImGui::PushFont(g_titleFont);
	}
	ImGui::TextColored(kAccent, "Voice");
	ImGui::SameLine(0.f, 5.f);
	ImGui::TextUnformatted("Bridge");
	if (g_titleFont)
	{
		ImGui::PopFont();
	}
	ImGui::TextColored(kMuted, "v" VOICE_BRIDGE_VERSION);
	ImGui::Dummy(ImVec2(0.f, 14.f));
	navButton(tr("   Status", "   Status"), Page::Status);
	navButton(tr("   Sound", "   Som"), Page::Sound);
	navButton(tr("   Microphone", "   Microfone"), Page::Microphone);
	navButton(tr("   Players", "   Jogadores"), Page::Players);
	navButton(tr("   Interface", "   Interface"), Page::Interface);
	navButton(tr("   About", "   Sobre"), Page::About);
	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 44.f);
	ImGui::TextColored(kMuted, "%s / Esc %s", keyName(static_cast<uint8_t>(GetSettings().menuKey)).c_str(), tr("closes", "fecha"));
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	// Content
	ImGui::SameLine(0.f, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.f, 18.f));
	ImGui::BeginChild("content", ImVec2(0.f, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding);
	static const char* const titles[][2] = {
		{ "Status", "Status" },
		{ "Sound", "Som" },
		{ "Microphone", "Microfone" },
		{ "Players", "Jogadores" },
		{ "Interface", "Interface" },
		{ "About", "Sobre" },
	};
	const auto& title = titles[static_cast<int>(g_page)];
	if (g_titleFont)
	{
		ImGui::PushFont(g_titleFont);
	}
	ImGui::TextUnformatted(tr(title[0], title[1]));
	if (g_titleFont)
	{
		ImGui::PopFont();
	}
	ImGui::SameLine(ImGui::GetWindowWidth() - 50.f);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
	if (ImGui::Button("X", ImVec2(28.f, 28.f)))
	{
		setMenu(false);
	}
	ImGui::PopStyleColor();
	ImGui::Spacing();
	switch (g_page)
	{
	case Page::Status:
		pageStatus(status);
		break;
	case Page::Sound:
		changed |= pageSound();
		break;
	case Page::Microphone:
		changed |= pageMicrophone(status);
		break;
	case Page::Players:
		pagePlayers(status);
		break;
	case Page::Interface:
		changed |= pageInterface();
		break;
	case Page::About:
		pageAbout();
		break;
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::End();

	if (changed)
	{
		VoiceClient::Get().settingsChanged();
	}
}

void applyStyle()
{
	ImGuiStyle& style = ImGui::GetStyle();
	ImGui::StyleColorsDark(&style);
	style.WindowRounding = 12.f;
	style.ChildRounding = 10.f;
	style.FrameRounding = 7.f;
	style.GrabRounding = 7.f;
	style.PopupRounding = 8.f;
	style.ScrollbarRounding = 8.f;
	style.WindowBorderSize = 0.f;
	style.FramePadding = ImVec2(10.f, 7.f);
	style.ItemSpacing = ImVec2(10.f, 9.f);
	style.GrabMinSize = 14.f;
	ImVec4* colors = style.Colors;
	colors[ImGuiCol_WindowBg] = ImVec4(0.095f, 0.10f, 0.12f, 0.98f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.14f, 0.98f);
	colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.25f, 1.f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.24f, 0.28f, 1.f);
	colors[ImGuiCol_SliderGrab] = kAccent;
	colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.90f, 0.66f, 1.f);
	colors[ImGuiCol_CheckMark] = kAccent;
	colors[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.23f, 1.f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.26f, 0.30f, 1.f);
	colors[ImGuiCol_ButtonActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.6f);
	colors[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
	colors[ImGuiCol_HeaderActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f);
	colors[ImGuiCol_Separator] = ImVec4(1.f, 1.f, 1.f, 0.07f);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.f, 1.f, 1.f, 0.025f);
	colors[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.f);
	colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1.f);
}

// --- Hooks ----------------------------------------------------------------

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (g_imgui && g_menu)
	{
		ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
		if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
		{
			setMenu(false);
			return 0;
		}
		switch (message)
		{
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_MOUSEWHEEL:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			return 0;
		default:
			break;
		}
	}
	return CallWindowProcA(g_windowProc, window, message, wParam, lParam);
}

void initImGui(IDirect3DDevice9* device)
{
	D3DDEVICE_CREATION_PARAMETERS parameters {};
	device->GetCreationParameters(&parameters);
	g_window = parameters.hFocusWindow;
	VoiceClient::Get().setWindow(g_window);

	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	char fonts[MAX_PATH] {};
	GetWindowsDirectoryA(fonts, MAX_PATH);
	const std::string font = std::string(fonts) + "\\Fonts\\segoeui.ttf";
	if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		io.Fonts->AddFontFromFileTTF(font.c_str(), 17.f, nullptr, io.Fonts->GetGlyphRangesDefault());
		const std::string bold = std::string(fonts) + "\\Fonts\\segoeuib.ttf";
		g_titleFont = io.Fonts->AddFontFromFileTTF(GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES ? bold.c_str() : font.c_str(), 24.f,
			nullptr, io.Fonts->GetGlyphRangesDefault());
	}
	applyStyle();
	ImGui_ImplWin32_Init(g_window);
	ImGui_ImplDX9_Init(device);
	g_windowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
	g_device = device;
	g_imgui = true;
	Log("interface ready");
}

HRESULT WINAPI presentHook(IDirect3DDevice9* device, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty)
{
	g_lastPresent = GetTickCount64();
	if (!g_imgui)
	{
		initImGui(device);
	}
	else if (device != g_device)
	{
		// SA-MP or another mod replaced the device object.
		ImGui_ImplDX9_Shutdown();
		ImGui_ImplDX9_Init(device);
		g_device = device;
		Log("Direct3D device changed; interface moved to the new device");
	}
	static unsigned frames = 0;
	static bool netGameSeen = false;
	if (!netGameSeen && samp::NetGame())
	{
		netGameSeen = true;
		Log("network game started");
	}
	else if (!netGameSeen && frames == 3600)
	{
		Log("network game not detected after 3600 frames (samp.dll %s); voice cannot start", samp::VersionName());
	}
	static uint64_t lastAudioAttempt = 0;
	++frames;
	// Unknown samp.dll builds have no CNetGame address: start after ~10 s.
	// A failed attempt is retried every 5 seconds.
	if (!g_audioReady && (samp::NetGame() || (samp::GetVersion() == samp::Version::Unknown && frames > 600))
		&& GetTickCount64() - lastAudioAttempt > 5000)
	{
		lastAudioAttempt = GetTickCount64();
		// SA-MP has initialised BASS by the time the network game exists.
		g_audioReady = Audio::Get().init(g_window);
		if (g_audioReady)
		{
			Audio::Get().configureEncoder(vb::kDefaultBitrate, vb::kLegacyFrameMs);
			Audio::Get().openMicrophone(GetSettings().micDevice);
		}
	}

	VoiceClient& client = VoiceClient::Get();
	client.frame();
	const int menuKey = GetSettings().menuKey;
	if (menuKey && !g_waitingKey)
	{
		const int down = (GetAsyncKeyState(menuKey) & 0x8000) != 0;
		if (down && !g_menuKeyWasDown && !samp::IsChatInputActive() && !samp::IsDialogActive())
		{
			setMenu(!g_menu);
		}
		g_menuKeyWasDown = down;
	}

	if (device->TestCooperativeLevel() == D3D_OK)
	{
		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		const VoiceStatus status = client.status();
		trackSpeakers(status);
		drawHud(status);
		if (g_menu)
		{
			drawMenu(status);
		}
		ImGui::EndFrame();
		if (device->BeginScene() == D3D_OK)
		{
			ImGui::Render();
			ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			device->EndScene();
		}
	}
	return g_present(device, source, destination, window, dirty);
}

HRESULT WINAPI resetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
	if (g_imgui)
	{
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}
	const HRESULT result = g_reset(device, parameters);
	if (g_imgui && SUCCEEDED(result))
	{
		ImGui_ImplDX9_CreateDeviceObjects();
	}
	return result;
}

bool hookVtable(void** vtable)
{
	DWORD previous = 0;
	if (!VirtualProtect(&vtable[kResetIndex], sizeof(void*) * 2, PAGE_EXECUTE_READWRITE, &previous))
	{
		return false;
	}
	if (vtable[kPresentIndex] != reinterpret_cast<void*>(&presentHook))
	{
		g_present = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);
	}
	if (vtable[kResetIndex] != reinterpret_cast<void*>(&resetHook))
	{
		g_reset = reinterpret_cast<ResetFn>(vtable[kResetIndex]);
	}
	// The game thread may call the new entries as soon as they are written.
	std::atomic_thread_fence(std::memory_order_seq_cst);
	vtable[kResetIndex] = reinterpret_cast<void*>(&resetHook);
	vtable[kPresentIndex] = reinterpret_cast<void*>(&presentHook);
	VirtualProtect(&vtable[kResetIndex], sizeof(void*) * 2, previous, &previous);
	g_vtable = vtable;
	return true;
}

std::string moduleName(const void* address)
{
	HMODULE module = nullptr;
	char name[MAX_PATH] {};
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module)
		&& GetModuleFileNameA(module, name, MAX_PATH))
	{
		const char* slash = std::strrchr(name, '\\');
		return slash ? slash + 1 : name;
	}
	return "?";
}
}

bool Install()
{
	g_systemPortuguese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
	applyLanguage();
	IDirect3DDevice9* device = nullptr;
	for (int i = 0; i < 6000 && !device; ++i)
	{
		samp::Read(kGameDevice, device);
		if (!device)
		{
			Sleep(50);
		}
	}
	if (!device || !hookVtable(*reinterpret_cast<void***>(device)))
	{
		Log("could not hook Direct3D");
		return false;
	}
	Log("Direct3D hooked");
	g_lastPresent = GetTickCount64();

	// Watchdog: SA-MP (R5/DL) and overlays such as SAMPFUNCS may replace the
	// device or its Present entry after us.  If our hook stops running while
	// another Present is installed, hook the current device again.
	for (;;)
	{
		Sleep(500);
		IDirect3DDevice9* current = nullptr;
		if (!samp::Read(kGameDevice, current) || !current)
		{
			continue;
		}
		void** vtable = *reinterpret_cast<void***>(current);
		if (vtable[kPresentIndex] == reinterpret_cast<void*>(&presentHook) || GetTickCount64() - g_lastPresent < 2000)
		{
			continue;
		}
		Log("Direct3D hook replaced by %s; hooking again", moduleName(vtable[kPresentIndex]).c_str());
		hookVtable(vtable);
		g_lastPresent = GetTickCount64();
	}
}

void Shutdown()
{
	if (g_vtable && g_present)
	{
		DWORD previous = 0;
		if (VirtualProtect(&g_vtable[kResetIndex], sizeof(void*) * 2, PAGE_EXECUTE_READWRITE, &previous))
		{
			g_vtable[kResetIndex] = reinterpret_cast<void*>(g_reset);
			g_vtable[kPresentIndex] = reinterpret_cast<void*>(g_present);
			VirtualProtect(&g_vtable[kResetIndex], sizeof(void*) * 2, previous, &previous);
		}
	}
	if (g_window && g_windowProc)
	{
		SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_windowProc));
	}
}

bool MenuOpen()
{
	return g_menu;
}

#ifdef VB_MENU_PREVIEW
// Renders the menu outside the game (tools/menu-preview.cpp).
void PreviewFrame(IDirect3DDevice9* device, int page, bool portuguese)
{
	if (!g_imgui)
	{
		g_portuguese = portuguese;
		g_previewMode = true;
		initImGui(device);
		g_menu = true;
		ImGui::GetIO().MouseDrawCursor = false;
		g_recent[3] = { GetTickCount64(), 0xFF66FF66, "Local" };
		g_recent[12] = { GetTickCount64() - 5000, 0xFFFFAA00, "Global" };
		g_recent[27] = { GetTickCount64() - 60000, 0xFF55AAFF, "Radio 7" };
	}
	g_page = static_cast<Page>(page);
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	VoiceStatus status;
	status.server = ServerKind::VoiceBridge;
	status.transport = vb::transport::udp;
	status.keys = { 0x42, 0x5A };
	drawHud(status);
	drawMenu(status);
	ImGui::EndFrame();
	if (device->BeginScene() == D3D_OK)
	{
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		device->EndScene();
	}
}
#endif
}

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
#include <cfloat>
#include <cstdio>
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
// The functions found the first time we hooked (Direct3D itself, or a hook
// installed before us).  Used when our hook is re-entered: SA-MP hooks after
// us and calls us as its "original"; if we then hook again on top of it,
// each would call the other forever (crash after alt+tab).
PresentFn g_basePresent = nullptr;
ResetFn g_baseReset = nullptr;
thread_local bool g_inPresent = false;
thread_local bool g_inReset = false;
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

// Microphone above the head of every player being heard (like SampVoice).
void drawHeadIcons(ImDrawList* draw, float scale)
{
	const uint64_t t = GetTickCount64();
	for (const SpeakerInfo& speaker : Audio::Get().speakers())
	{
		float position[3];
		if (!samp::PlayerHeadPosition(speaker.player, position))
		{
			continue; // not streamed in
		}
		const game::Vec3 head { position[0], position[1], position[2] };
		float x = 0.f;
		float y = 0.f;
		float depth = 0.f;
		if (!game::WorldToScreen(head, x, y, depth) || depth <= 0.5f || depth > 60.f)
		{
			continue;
		}
		// Smaller with distance, a little pulse with the voice level.
		const float level = std::clamp((speaker.level + 50.f) / 40.f, 0.f, 1.f);
		const float size = std::clamp(46.f * scale * 5.f / depth, 12.f * scale, 40.f * scale);
		const ImVec2 center(x, y - size * 0.6f);
		const float pulse = 0.85f + 0.15f * std::sin(static_cast<float>(t % 1000) / 1000.f * 6.2831f);
		draw->AddCircleFilled(center, size * (0.62f + 0.22f * level) * pulse, IM_COL32(60, 220, 90, 70));
		draw->AddCircleFilled(center, size * 0.55f, IM_COL32(15, 15, 15, 170));
		drawMicrophone(draw, center, size * 0.75f, IM_COL32(60, 220, 90, 255), false);
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

	if (status.server != ServerKind::None && status.showHeadIcons && settings.showHeadIcons)
	{
		drawHeadIcons(draw, scale);
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
const VoiceStatus* g_pageStatus = nullptr;
bool g_previewMode = false;
bool g_waitingKey = false;
uint64_t g_linkCopiedAt = 0;
ImFont* g_titleFont = nullptr;
std::map<uint16_t, RecentSpeaker> g_recent;

const ImVec4 kGood(0.25f, 0.85f, 0.45f, 1.f);
const ImVec4 kWarn(1.f, 0.72f, 0.25f, 1.f);
const ImVec4 kBad(1.f, 0.38f, 0.38f, 1.f);

// --- Themes ---------------------------------------------------------------
// Each theme has its own menu layout, not only colours.

enum class Layout
{
	Classic, // sidebar with text buttons
	Lounge, // chat app: server rail, channel list, user panel
	Neon, // pill tabs on top, floating card
	Terminal, // monospace, square, amber
};

struct Theme
{
	const char* name[2];
	Layout layout;
	ImVec4 accent; // selection, titles, sliders
	ImVec4 accent2; // second accent (gradients, hover)
	ImVec4 toggleOn;
	ImVec4 text;
	ImVec4 muted;
	ImVec4 windowBg; // also the content area
	ImVec4 sidebarBg;
	ImVec4 railBg;
	ImVec4 cardBg;
	ImVec4 frameBg;
	ImVec4 frameHover;
	ImVec4 button;
	ImVec4 buttonHover;
	ImVec4 buttonText;
	ImVec4 toggleOff;
	ImVec4 separator;
	float windowRounding;
	float frameRounding;
};

ImVec4 alpha(const ImVec4& color, float a)
{
	return ImVec4(color.x, color.y, color.z, a);
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t)
{
	return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

Theme classicTheme()
{
	Theme t {};
	t.name[0] = "Classic";
	t.name[1] = "Clássico";
	t.layout = Layout::Classic;
	t.accent = ImVec4(0.16f, 0.78f, 0.56f, 1.f);
	t.accent2 = ImVec4(0.30f, 0.90f, 0.66f, 1.f);
	t.toggleOn = t.accent;
	t.text = ImVec4(0.92f, 0.93f, 0.95f, 1.f);
	t.muted = ImVec4(0.62f, 0.65f, 0.70f, 1.f);
	t.windowBg = ImVec4(0.095f, 0.10f, 0.12f, 0.98f);
	t.sidebarBg = ImVec4(0.075f, 0.08f, 0.095f, 1.f);
	t.railBg = t.sidebarBg;
	t.cardBg = ImVec4(0.12f, 0.13f, 0.155f, 1.f);
	t.frameBg = ImVec4(0.16f, 0.17f, 0.20f, 1.f);
	t.frameHover = ImVec4(0.20f, 0.21f, 0.25f, 1.f);
	t.button = ImVec4(0.18f, 0.19f, 0.23f, 1.f);
	t.buttonHover = ImVec4(0.24f, 0.26f, 0.30f, 1.f);
	t.buttonText = t.text;
	t.toggleOff = ImVec4(0.30f, 0.32f, 0.36f, 1.f);
	t.separator = ImVec4(1.f, 1.f, 1.f, 0.07f);
	t.windowRounding = 12.f;
	t.frameRounding = 7.f;
	return t;
}

Theme loungeTheme()
{
	Theme t {};
	t.name[0] = "Lounge";
	t.name[1] = "Lounge";
	t.layout = Layout::Lounge;
	t.accent = ImVec4(0.345f, 0.396f, 0.949f, 1.f);
	t.accent2 = ImVec4(0.278f, 0.322f, 0.769f, 1.f);
	t.toggleOn = ImVec4(0.137f, 0.647f, 0.353f, 1.f);
	t.text = ImVec4(0.86f, 0.87f, 0.88f, 1.f);
	t.muted = ImVec4(0.58f, 0.61f, 0.64f, 1.f);
	t.windowBg = ImVec4(0.192f, 0.200f, 0.220f, 1.f);
	t.sidebarBg = ImVec4(0.169f, 0.176f, 0.192f, 1.f);
	t.railBg = ImVec4(0.118f, 0.122f, 0.133f, 1.f);
	t.cardBg = t.sidebarBg;
	t.frameBg = t.railBg;
	t.frameHover = ImVec4(0.15f, 0.155f, 0.17f, 1.f);
	t.button = t.accent;
	t.buttonHover = t.accent2;
	t.buttonText = ImVec4(1.f, 1.f, 1.f, 1.f);
	t.toggleOff = ImVec4(0.502f, 0.518f, 0.557f, 1.f);
	t.separator = ImVec4(1.f, 1.f, 1.f, 0.06f);
	t.windowRounding = 8.f;
	t.frameRounding = 4.f;
	return t;
}

Theme neonTheme()
{
	Theme t {};
	t.name[0] = "Neon";
	t.name[1] = "Neon";
	t.layout = Layout::Neon;
	t.accent = ImVec4(1.f, 0.24f, 0.80f, 1.f);
	t.accent2 = ImVec4(0.16f, 0.90f, 1.f, 1.f);
	t.toggleOn = t.accent;
	t.text = ImVec4(0.95f, 0.93f, 1.f, 1.f);
	t.muted = ImVec4(0.64f, 0.60f, 0.80f, 1.f);
	t.windowBg = ImVec4(0.055f, 0.043f, 0.10f, 0.98f);
	t.sidebarBg = ImVec4(0.075f, 0.06f, 0.14f, 1.f);
	t.railBg = t.sidebarBg;
	t.cardBg = ImVec4(0.09f, 0.07f, 0.19f, 1.f);
	t.frameBg = ImVec4(0.14f, 0.11f, 0.26f, 1.f);
	t.frameHover = ImVec4(0.18f, 0.14f, 0.33f, 1.f);
	t.button = ImVec4(1.f, 0.24f, 0.80f, 0.25f);
	t.buttonHover = ImVec4(1.f, 0.24f, 0.80f, 0.45f);
	t.buttonText = t.text;
	t.toggleOff = ImVec4(0.25f, 0.20f, 0.38f, 1.f);
	t.separator = ImVec4(0.16f, 0.90f, 1.f, 0.12f);
	t.windowRounding = 16.f;
	t.frameRounding = 10.f;
	return t;
}

Theme terminalTheme()
{
	Theme t {};
	t.name[0] = "Terminal";
	t.name[1] = "Terminal";
	t.layout = Layout::Terminal;
	t.accent = ImVec4(1.f, 0.69f, 0.f, 1.f);
	t.accent2 = ImVec4(1.f, 0.80f, 0.35f, 1.f);
	t.toggleOn = t.accent;
	t.text = ImVec4(1.f, 0.84f, 0.55f, 1.f);
	t.muted = ImVec4(0.66f, 0.52f, 0.28f, 1.f);
	t.windowBg = ImVec4(0.035f, 0.04f, 0.03f, 0.98f);
	t.sidebarBg = ImVec4(0.05f, 0.055f, 0.04f, 1.f);
	t.railBg = t.sidebarBg;
	t.cardBg = ImVec4(0.07f, 0.07f, 0.05f, 1.f);
	t.frameBg = ImVec4(0.11f, 0.10f, 0.06f, 1.f);
	t.frameHover = ImVec4(0.16f, 0.14f, 0.08f, 1.f);
	t.button = t.frameBg;
	t.buttonHover = t.frameHover;
	t.buttonText = t.accent;
	t.toggleOff = ImVec4(0.30f, 0.24f, 0.10f, 1.f);
	t.separator = ImVec4(1.f, 0.69f, 0.f, 0.25f);
	t.windowRounding = 0.f;
	t.frameRounding = 0.f;
	return t;
}

const Theme kThemes[] = { classicTheme(), loungeTheme(), neonTheme(), terminalTheme() };
constexpr int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));
const Theme* g_theme = &kThemes[0];
int g_styledTheme = -1;
ImFont* g_monoFont = nullptr;
ImFont* g_monoTitleFont = nullptr;

ImFont* titleFont()
{
	return g_theme->layout == Layout::Terminal && g_monoTitleFont ? g_monoTitleFont : g_titleFont;
}

void titleText(const char* text, const ImVec4& color)
{
	ImFont* font = titleFont();
	if (font)
	{
		ImGui::PushFont(font);
	}
	ImGui::TextColored(color, "%s", text);
	if (font)
	{
		ImGui::PopFont();
	}
}

// Upper case for titles: ASCII and the accented Latin-1 letters (ã -> Ã).
std::string upper(const char* text)
{
	std::string out(text);
	for (std::size_t i = 0; i < out.size(); ++i)
	{
		const auto c = static_cast<unsigned char>(out[i]);
		if (c >= 'a' && c <= 'z')
		{
			out[i] = static_cast<char>(c - 32);
		}
		else if (c == 0xC3 && i + 1 < out.size())
		{
			const auto next = static_cast<unsigned char>(out[i + 1]);
			if (next >= 0xA0 && next <= 0xBE && next != 0xB7)
			{
				out[i + 1] = static_cast<char>(next - 0x20);
			}
			++i;
		}
	}
	return out;
}

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
	const ImU32 track = ImGui::GetColorU32(*value ? g_theme->toggleOn : g_theme->toggleOff);
	const float radius = height * 0.5f - 2.f;
	const float knob = *value ? width - radius - 2.f : radius + 2.f;
	if (g_theme->layout == Layout::Terminal)
	{
		draw->AddRect(position, ImVec2(position.x + width, position.y + height), ImGui::GetColorU32(g_theme->accent));
		const ImVec2 center(position.x + knob, position.y + height * 0.5f);
		draw->AddRectFilled(ImVec2(center.x - radius + 1.f, center.y - radius + 1.f), ImVec2(center.x + radius - 1.f, center.y + radius - 1.f),
			*value ? track : ImGui::GetColorU32(g_theme->muted));
	}
	else
	{
		draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height), track, height * 0.5f);
		draw->AddCircleFilled(ImVec2(position.x + knob, position.y + height * 0.5f), radius, IM_COL32(245, 245, 245, 255));
	}
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
	switch (g_theme->layout)
	{
	case Layout::Lounge:
		// Small upper case heading, like the settings of chat apps.
		ImGui::Dummy(ImVec2(0.f, 2.f));
		ImGui::SetWindowFontScale(0.8f);
		ImGui::TextColored(g_theme->muted, "%s", upper(text).c_str());
		ImGui::SetWindowFontScale(1.f);
		break;
	case Layout::Neon:
	{
		ImGui::TextColored(g_theme->accent2, "%s", text);
		const ImVec2 position = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilledMultiColor(position, ImVec2(position.x + 90.f, position.y + 2.f), ImGui::GetColorU32(g_theme->accent),
			ImGui::GetColorU32(alpha(g_theme->accent2, 0.f)), ImGui::GetColorU32(alpha(g_theme->accent2, 0.f)), ImGui::GetColorU32(g_theme->accent));
		ImGui::Dummy(ImVec2(0.f, 4.f));
		break;
	}
	case Layout::Terminal:
		ImGui::TextColored(g_theme->accent, "-- %s --", upper(text).c_str());
		break;
	default:
		ImGui::PushStyleColor(ImGuiCol_Text, g_theme->accent);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
		ImGui::Separator();
		break;
	}
	ImGui::Spacing();
}

void infoRow(const char* label, const std::string& value, const ImVec4& color = g_theme->text)
{
	ImGui::TextColored(g_theme->muted, "%s", label);
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
	draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height), ImGui::GetColorU32(g_theme->frameBg), g_theme->frameRounding * 0.6f);
	draw->AddRectFilledMultiColor(position, ImVec2(x(level), position.y + height), IM_COL32(40, 200, 120, 255), IM_COL32(230, 200, 60, 255),
		IM_COL32(230, 200, 60, 255), IM_COL32(40, 200, 120, 255));
	if (showThreshold)
	{
		const float marker = x(static_cast<float>(threshold));
		draw->AddLine(ImVec2(marker, position.y - 3.f), ImVec2(marker, position.y + height + 3.f), IM_COL32(255, 255, 255, 230), 2.f);
	}
	ImGui::Dummy(ImVec2(width, height + 4.f));
}

void connectionState(const VoiceStatus& status, const char*& title, const char*& detail, ImVec4& color)
{
	title = tr("No voice server", "Servidor sem chat de voz");
	detail = tr("This server does not use Voice Bridge or SampVoice.", "Este servidor não usa Voice Bridge nem SampVoice.");
	color = g_theme->muted;
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
}

void statusCard(const VoiceStatus& status)
{
	const char* title = nullptr;
	const char* detail = nullptr;
	ImVec4 color;
	connectionState(status, title, detail, color);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, g_theme->cardBg);
	ImGui::BeginChild("card", ImVec2(0.f, 84.f), g_theme->layout == Layout::Terminal ? ImGuiChildFlags_Borders : ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetWindowPos();
	const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(GetTickCount64() % 2000) / 2000.f * 6.2831f);
	draw->AddCircleFilled(ImVec2(origin.x + 32.f, origin.y + 42.f), 16.f * pulse, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.25f)));
	draw->AddCircleFilled(ImVec2(origin.x + 32.f, origin.y + 42.f), 9.f, ImGui::GetColorU32(color));
	if (g_theme->layout == Layout::Neon)
	{
		const ImVec2 size = ImGui::GetWindowSize();
		draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(alpha(color, 0.45f)), ImGui::GetStyle().ChildRounding, 0, 1.5f);
	}
	ImGui::SetCursorPos(ImVec2(62.f, 14.f));
	titleText(title, color);
	ImGui::SetCursorPos(ImVec2(62.f, 50.f));
	ImGui::TextColored(g_theme->muted, "%s", detail);
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
	ImGui::TextColored(g_theme->muted, "%s", VOICE_BRIDGE_URL);
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
	infoRow("Claude (Anthropic)", tr("development assistance", "auxílio no desenvolvimento"));
	infoRow("MOR (CyberMor)", tr("original SampVoice protocol", "protocolo original do SampVoice"));
}

bool pageSound()
{
	Settings& settings = GetSettings();
	bool changed = false;
	sectionTitle(tr("Voice chat", "Chat de voz"));
	changed |= toggle(tr("Hear other players", "Ouvir os outros jogadores"), &settings.soundEnabled);
	ImGui::Spacing();
	ImGui::TextColored(g_theme->muted, "%s", tr("Volume", "Volume"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##volume", &settings.volume, 0, 200, "%d%%");
	ImGui::TextColored(g_theme->muted, "%s", tr("Above 100% the voices are amplified.", "Acima de 100% as vozes são amplificadas."));
	ImGui::Spacing();
	changed |= toggle(tr("Hear while the game is paused", "Ouvir com o jogo pausado"), &settings.hearWhilePaused);
	changed |= toggle(tr("Noise suppression on the voices I hear", "Anti-ruído nas vozes que eu escuto"), &settings.incomingNoiseSuppression);
	ImGui::TextColored(g_theme->muted, "%s",
		tr("Pause menu (Esc), minimised or in the background (alt+tab).", "Menu de pausa (Esc), minimizado ou em segundo plano (alt+tab)."));

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
		ImGui::TextColored(g_theme->muted, "%s", tr("Room echo (makes far voices sound far)", "Eco do ambiente (faz vozes distantes soarem longe)"));
		ImGui::SetNextItemWidth(-1.f);
		changed |= ImGui::SliderInt("##room", &settings.roomAmount, 0, 100, "%d%%");
	}
	ImGui::Spacing();
	ImGui::TextColored(g_theme->muted, "%s", tr("Distance fade", "Queda com a distância"));
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
	ImGui::TextColored(g_theme->muted, "%s", tr("The white line is the noise gate: quieter sounds are not sent.", "A linha branca é o filtro de ruído: sons mais baixos não são enviados."));
	ImGui::Spacing();
	ImGui::TextColored(g_theme->muted, "%s", tr("Gain", "Ganho"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##gain", &settings.micGain, 0, 400, "%d%%");
	ImGui::TextColored(g_theme->muted, "%s", tr("Noise gate", "Filtro de ruído"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##gate", &settings.noiseGate, -90, -20, settings.noiseGate <= -90 ? tr("off", "desligado") : "%d dB");
	ImGui::Spacing();
	changed |= toggle(tr("Noise suppression (removes fans, keyboard and background noise)", "Anti-ruído (remove ventilador, teclado e barulho de fundo)"),
		&settings.noiseSuppression);
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
		ImGui::TextColored(g_theme->muted, "%s", tr("This server hides who is talking.", "Este servidor oculta quem está falando."));
		return;
	}
	if (g_recent.empty())
	{
		ImGui::TextColored(g_theme->muted, "%s", tr("Nobody has talked yet.", "Ninguém falou ainda."));
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
			ImGui::TextColored(g_theme->muted, "%s", entry.second.stream.empty() ? "-" : toUtf8(entry.second.stream).c_str());
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

// Small drawing of a theme's layout for the theme picker.
void themeThumbnail(ImDrawList* draw, const Theme& t, ImVec2 a, ImVec2 b)
{
	const float w = b.x - a.x;
	const float h = b.y - a.y;
	const float r = std::min(t.windowRounding * 0.5f, 6.f);
	const auto col = [](const ImVec4& c) { return ImGui::GetColorU32(c); };
	draw->AddRectFilled(a, b, col(alpha(t.windowBg, 1.f)), r);
	const auto line = [&](float x, float y, float length, const ImVec4& c) {
		draw->AddRectFilled(ImVec2(x, y), ImVec2(x + length, y + 3.f), col(c), t.frameRounding > 0.f ? 1.5f : 0.f);
	};
	switch (t.layout)
	{
	case Layout::Classic:
		draw->AddRectFilled(a, ImVec2(a.x + w * 0.3f, b.y), col(t.sidebarBg), r, ImDrawFlags_RoundCornersLeft);
		draw->AddRectFilled(ImVec2(a.x + 5.f, a.y + 16.f), ImVec2(a.x + w * 0.3f - 5.f, a.y + 24.f), col(alpha(t.accent, 0.35f)), 2.f);
		line(a.x + 7.f, a.y + 30.f, w * 0.18f, t.muted);
		line(a.x + 7.f, a.y + 38.f, w * 0.15f, t.muted);
		line(a.x + w * 0.36f, a.y + 10.f, w * 0.3f, t.text);
		line(a.x + w * 0.36f, a.y + 22.f, w * 0.5f, t.accent);
		line(a.x + w * 0.36f, a.y + 32.f, w * 0.45f, t.muted);
		break;
	case Layout::Lounge:
		draw->AddRectFilled(a, ImVec2(a.x + w * 0.14f, b.y), col(t.railBg), r, ImDrawFlags_RoundCornersLeft);
		draw->AddRectFilled(ImVec2(a.x + w * 0.025f, a.y + 6.f), ImVec2(a.x + w * 0.115f, a.y + 6.f + w * 0.09f), col(t.accent), 4.f);
		draw->AddRectFilled(ImVec2(a.x + w * 0.14f, a.y), ImVec2(a.x + w * 0.42f, b.y), col(t.sidebarBg));
		draw->AddRectFilled(ImVec2(a.x + w * 0.14f, b.y - 12.f), ImVec2(a.x + w * 0.42f, b.y), col(mix(t.railBg, t.sidebarBg, 0.5f)));
		line(a.x + w * 0.17f, a.y + 12.f, w * 0.2f, t.muted);
		draw->AddRectFilled(ImVec2(a.x + w * 0.16f, a.y + 19.f), ImVec2(a.x + w * 0.40f, a.y + 27.f), col(mix(t.sidebarBg, t.text, 0.15f)), 2.f);
		line(a.x + w * 0.17f, a.y + 32.f, w * 0.17f, t.muted);
		line(a.x + w * 0.47f, a.y + 10.f, w * 0.25f, t.text);
		line(a.x + w * 0.47f, a.y + 22.f, w * 0.45f, t.toggleOn);
		line(a.x + w * 0.47f, a.y + 32.f, w * 0.40f, t.muted);
		break;
	case Layout::Neon:
	{
		const float pill = w * 0.2f;
		draw->AddRectFilled(ImVec2(a.x + 6.f, a.y + 6.f), ImVec2(a.x + 6.f + pill, a.y + 13.f), col(t.accent), 4.f);
		draw->AddRectFilled(ImVec2(a.x + 10.f + pill, a.y + 6.f), ImVec2(a.x + 10.f + pill * 2.f, a.y + 13.f), col(t.frameBg), 4.f);
		draw->AddRectFilledMultiColor(ImVec2(a.x + 6.f, a.y + 17.f), ImVec2(b.x - 6.f, a.y + 18.5f), col(t.accent), col(t.accent2), col(t.accent2),
			col(t.accent));
		draw->AddRectFilled(ImVec2(a.x + 6.f, a.y + 22.f), ImVec2(b.x - 6.f, b.y - 6.f), col(t.cardBg), 5.f);
		line(a.x + 11.f, a.y + 28.f, w * 0.3f, t.accent2);
		line(a.x + 11.f, a.y + 37.f, w * 0.5f, t.muted);
		break;
	}
	case Layout::Terminal:
		draw->AddRect(a, b, col(t.accent));
		line(a.x + 5.f, a.y + 6.f, w * 0.6f, t.accent);
		draw->AddLine(ImVec2(a.x + w * 0.3f, a.y + 14.f), ImVec2(a.x + w * 0.3f, b.y - 4.f), col(alpha(t.accent, 0.4f)));
		draw->AddRectFilled(ImVec2(a.x + 5.f, a.y + 17.f), ImVec2(a.x + w * 0.3f - 4.f, a.y + 24.f), col(t.accent));
		line(a.x + 5.f, a.y + 29.f, w * 0.18f, t.muted);
		line(a.x + w * 0.34f, a.y + 18.f, w * 0.4f, t.text);
		line(a.x + w * 0.34f, a.y + 28.f, w * 0.5f, t.muted);
		break;
	}
}

bool themePicker(Settings& settings)
{
	bool changed = false;
	const float spacing = 10.f;
	const float width = (ImGui::GetContentRegionAvail().x - spacing * (kThemeCount - 1)) / kThemeCount;
	const float thumb = width * 0.55f;
	for (int i = 0; i < kThemeCount; ++i)
	{
		const Theme& theme = kThemes[i];
		ImGui::PushID(i);
		if (i > 0)
		{
			ImGui::SameLine(0.f, spacing);
		}
		const ImVec2 position = ImGui::GetCursorScreenPos();
		const float height = thumb + ImGui::GetTextLineHeight() + 14.f;
		if (ImGui::InvisibleButton("##theme", ImVec2(width, height)))
		{
			settings.menuTheme = i;
			changed = true;
		}
		const bool selected = settings.menuTheme == i;
		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (selected || ImGui::IsItemHovered())
		{
			draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height),
				ImGui::GetColorU32(alpha(g_theme->accent, selected ? 0.16f : 0.07f)), g_theme->frameRounding);
		}
		if (selected)
		{
			draw->AddRect(position, ImVec2(position.x + width, position.y + height), ImGui::GetColorU32(g_theme->accent), g_theme->frameRounding, 0, 2.f);
		}
		themeThumbnail(draw, theme, ImVec2(position.x + 6.f, position.y + 6.f), ImVec2(position.x + width - 6.f, position.y + 6.f + thumb));
		const char* name = tr(theme.name[0], theme.name[1]);
		const float textWidth = ImGui::CalcTextSize(name).x;
		draw->AddText(ImVec2(position.x + (width - textWidth) * 0.5f, position.y + thumb + 10.f), ImGui::GetColorU32(selected ? g_theme->accent : g_theme->text),
			name);
		ImGui::PopID();
	}
	if (settings.menuTheme == 1)
	{
		ImGui::TextColored(g_theme->muted, "%s", tr("Lounge: a layout like Discord.", "Lounge: um layout parecido com o Discord."));
	}
	return changed;
}

bool pageInterface()
{
	Settings& settings = GetSettings();
	bool changed = false;
	sectionTitle(tr("Appearance", "Aparência"));
	changed |= themePicker(settings);

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
	changed |= toggle(tr("Microphone above the head of who is talking", "Microfone na cabeça de quem está falando"), &settings.showHeadIcons);
	if (!g_previewMode && !VoiceClient::Get().status().showHeadIcons)
	{
		ImGui::TextColored(g_theme->muted, "%s", tr("This server disabled the icons above heads.", "Este servidor desativou os ícones na cabeça."));
	}
	ImGui::Spacing();
	ImGui::TextColored(g_theme->muted, "%s", tr("Size", "Tamanho"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##scale", &settings.uiScale, 50, 200, "%d%%");

	sectionTitle(tr("Microphone icon position", "Posição do ícone do microfone"));
	ImGui::TextColored(g_theme->muted, "%s", tr("Drag the icon on the screen while this menu is open, or use the sliders.",
		"Arraste o ícone na tela enquanto este menu está aberto, ou use os controles."));
	ImGui::TextColored(g_theme->muted, "%s", tr("Horizontal", "Horizontal"));
	ImGui::SetNextItemWidth(-1.f);
	changed |= ImGui::SliderInt("##micx", &settings.micIconX, 0, 1000, "%d");
	ImGui::TextColored(g_theme->muted, "%s", tr("Vertical", "Vertical"));
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

void applyStyle();

struct PageInfo
{
	Page page;
	const char* name[2];
};

const PageInfo kPages[] = {
	{ Page::Status, { "Status", "Status" } },
	{ Page::Sound, { "Sound", "Som" } },
	{ Page::Microphone, { "Microphone", "Microfone" } },
	{ Page::Players, { "Players", "Jogadores" } },
	{ Page::Interface, { "Interface", "Interface" } },
	{ Page::About, { "About", "Sobre" } },
};

const char* pageName(Page page)
{
	const PageInfo& info = kPages[static_cast<int>(page)];
	return tr(info.name[0], info.name[1]);
}

bool drawPage(const VoiceStatus& status)
{
	switch (g_page)
	{
	case Page::Status:
		pageStatus(status);
		return false;
	case Page::Sound:
		return pageSound();
	case Page::Microphone:
		return pageMicrophone(status);
	case Page::Players:
		pagePlayers(status);
		return false;
	case Page::Interface:
		return pageInterface();
	case Page::About:
		pageAbout();
		return false;
	}
	return false;
}

bool beginMenuWindow(const ImVec2& size)
{
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	const bool visible = ImGui::Begin("##voicebridge", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
	ImGui::PopStyleVar();
	if (!visible)
	{
		ImGui::End();
	}
	return visible;
}

// Scrolling area for the pages, at the cursor.
bool pageArea(const ImVec2& size, const ImVec2& padding)
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::BeginChild("content", size, ImGuiChildFlags_AlwaysUseWindowPadding);
	const bool changed = drawPage(*g_pageStatus);
	ImGui::EndChild();
	ImGui::PopStyleVar();
	return changed;
}

bool closeButton(const char* label, const ImVec2& size)
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
	ImGui::PushStyleColor(ImGuiCol_Text, g_theme->text);
	const bool clicked = ImGui::Button(label, size);
	ImGui::PopStyleColor(2);
	if (clicked)
	{
		setMenu(false);
	}
	return clicked;
}

// --- Classic: sidebar with text buttons ---

void navButton(const char* label, Page page)
{
	const bool selected = g_page == page;
	ImGui::PushStyleColor(ImGuiCol_Button, selected ? alpha(g_theme->accent, 0.22f) : ImVec4(0.f, 0.f, 0.f, 0.f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.06f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, alpha(g_theme->accent, 0.35f));
	ImGui::PushStyleColor(ImGuiCol_Text, selected ? g_theme->accent : ImVec4(0.85f, 0.87f, 0.90f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));
	if (ImGui::Button((std::string("   ") + label).c_str(), ImVec2(-1.f, 36.f)))
	{
		g_page = page;
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
}

bool drawClassic()
{
	if (!beginMenuWindow(ImVec2(720.f, 520.f)))
	{
		return false;
	}
	ImGui::PushStyleColor(ImGuiCol_ChildBg, g_theme->sidebarBg);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 18.f));
	ImGui::BeginChild("sidebar", ImVec2(190.f, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding);
	if (g_titleFont)
	{
		ImGui::PushFont(g_titleFont);
	}
	ImGui::TextColored(g_theme->accent, "Voice");
	ImGui::SameLine(0.f, 5.f);
	ImGui::TextUnformatted("Bridge");
	if (g_titleFont)
	{
		ImGui::PopFont();
	}
	ImGui::TextColored(g_theme->muted, "v" VOICE_BRIDGE_VERSION);
	ImGui::Dummy(ImVec2(0.f, 14.f));
	for (const PageInfo& page : kPages)
	{
		navButton(tr(page.name[0], page.name[1]), page.page);
	}
	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 44.f);
	ImGui::TextColored(g_theme->muted, "%s / Esc %s", keyName(static_cast<uint8_t>(GetSettings().menuKey)).c_str(), tr("closes", "fecha"));
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	ImGui::SameLine(0.f, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.f, 18.f));
	ImGui::BeginChild("main", ImVec2(0.f, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding);
	titleText(pageName(g_page), g_theme->text);
	ImGui::SameLine(ImGui::GetWindowWidth() - 50.f);
	closeButton("X", ImVec2(28.f, 28.f));
	ImGui::Spacing();
	const bool changed = drawPage(*g_pageStatus);
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::End();
	return changed;
}

// --- Lounge: like a chat app (server rail, channel list, user panel) ---

void drawHeadset(ImDrawList* draw, ImVec2 center, float size, ImU32 color, bool crossed)
{
	const float r = size * 0.36f;
	draw->PathArcTo(ImVec2(center.x, center.y + size * 0.05f), r, 3.14159f, 6.28318f, 16);
	draw->PathStroke(color, 0, size * 0.09f);
	const float cupW = size * 0.16f;
	const float cupH = size * 0.30f;
	draw->AddRectFilled(ImVec2(center.x - r - cupW * 0.5f, center.y), ImVec2(center.x - r + cupW * 0.5f, center.y + cupH), color, size * 0.05f);
	draw->AddRectFilled(ImVec2(center.x + r - cupW * 0.5f, center.y), ImVec2(center.x + r + cupW * 0.5f, center.y + cupH), color, size * 0.05f);
	if (crossed)
	{
		draw->AddLine(ImVec2(center.x - size * 0.45f, center.y - size * 0.4f), ImVec2(center.x + size * 0.45f, center.y + size * 0.45f),
			IM_COL32(242, 63, 67, 255), size * 0.09f);
	}
}

bool loungeChannel(const char* label, Page page)
{
	const Theme& t = *g_theme;
	const bool selected = g_page == page;
	const ImVec2 position = ImGui::GetCursorScreenPos();
	const ImVec2 size(ImGui::GetContentRegionAvail().x, 32.f);
	ImGui::PushID(label);
	const bool clicked = ImGui::InvisibleButton("##channel", size);
	const bool hovered = ImGui::IsItemHovered();
	ImGui::PopID();
	if (clicked)
	{
		g_page = page;
	}
	ImDrawList* draw = ImGui::GetWindowDrawList();
	if (selected || hovered)
	{
		draw->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y),
			ImGui::GetColorU32(mix(t.sidebarBg, t.text, selected ? 0.13f : 0.06f)), 4.f);
	}
	const float textY = position.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
	draw->AddText(ImVec2(position.x + 10.f, textY), ImGui::GetColorU32(t.muted), "#");
	draw->AddText(ImVec2(position.x + 28.f, textY), ImGui::GetColorU32(selected ? ImVec4(1.f, 1.f, 1.f, 1.f) : hovered ? t.text : t.muted), label);
	return clicked;
}

void loungeCategory(const char* label)
{
	ImGui::Dummy(ImVec2(0.f, 8.f));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.f);
	ImGui::SetWindowFontScale(0.78f);
	ImGui::TextColored(g_theme->muted, "%s", upper(label).c_str());
	ImGui::SetWindowFontScale(1.f);
}

// Square icon button of the user panel; returns true when clicked.
bool panelButton(const char* id, const ImVec2& position, float size)
{
	ImGui::SetCursorScreenPos(position);
	const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
	if (ImGui::IsItemHovered())
	{
		ImGui::GetWindowDrawList()->AddRectFilled(position, ImVec2(position.x + size, position.y + size), ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.08f)), 4.f);
	}
	return clicked;
}

bool drawLounge()
{
	const Theme& t = *g_theme;
	if (!beginMenuWindow(ImVec2(800.f, 540.f)))
	{
		return false;
	}
	bool changed = false;
	const VoiceStatus& status = *g_pageStatus;
	Settings& settings = GetSettings();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetWindowPos();
	const ImVec2 size = ImGui::GetWindowSize();
	const float rail = 72.f;
	const float channels = 232.f;
	const float panel = 56.f;
	const float header = 48.f;
	const auto col = [](const ImVec4& c) { return ImGui::GetColorU32(c); };

	// Server rail: the Voice Bridge "server" and the microphone state.
	draw->AddRectFilled(origin, ImVec2(origin.x + rail, origin.y + size.y), col(t.railBg), t.windowRounding, ImDrawFlags_RoundCornersLeft);
	const ImVec2 logo(origin.x + rail * 0.5f, origin.y + 12.f + 24.f);
	draw->AddRectFilled(ImVec2(logo.x - 24.f, logo.y - 24.f), ImVec2(logo.x + 24.f, logo.y + 24.f), col(t.accent), 16.f);
	draw->AddRectFilled(ImVec2(origin.x, logo.y - 20.f), ImVec2(origin.x + 4.f, logo.y + 20.f), IM_COL32(255, 255, 255, 255), 2.f,
		ImDrawFlags_RoundCornersRight);
	ImFont* font = g_titleFont ? g_titleFont : ImGui::GetFont();
	const float logoSize = font->FontSize * 0.8f;
	const ImVec2 logoText = font->CalcTextSizeA(logoSize, FLT_MAX, 0.f, "VB");
	draw->AddText(font, logoSize, ImVec2(logo.x - logoText.x * 0.5f, logo.y - logoText.y * 0.5f), IM_COL32(255, 255, 255, 255), "VB");
	draw->AddLine(ImVec2(logo.x - 16.f, logo.y + 36.f), ImVec2(logo.x + 16.f, logo.y + 36.f), col(alpha(t.muted, 0.3f)), 2.f);
	const ImVec2 micCircle(logo.x, logo.y + 72.f);
	const bool micOff = status.muted || !settings.micEnabled;
	draw->AddCircleFilled(micCircle, 24.f, col(status.transmitting ? t.toggleOn : t.windowBg));
	drawMicrophone(draw, micCircle, 24.f, micOff ? IM_COL32(242, 63, 67, 255) : IM_COL32(220, 222, 225, 255), micOff);

	// Channel list.
	draw->AddRectFilled(ImVec2(origin.x + rail, origin.y), ImVec2(origin.x + rail + channels, origin.y + size.y), col(t.sidebarBg));
	draw->AddText(ImVec2(origin.x + rail + 16.f, origin.y + (header - ImGui::GetTextLineHeight()) * 0.5f), col(ImVec4(1.f, 1.f, 1.f, 1.f)), "Voice Bridge");
	draw->AddText(ImVec2(origin.x + rail + channels - 56.f, origin.y + (header - ImGui::GetTextLineHeight()) * 0.5f), col(t.muted),
		"v" VOICE_BRIDGE_VERSION);
	draw->AddLine(ImVec2(origin.x + rail, origin.y + header), ImVec2(origin.x + rail + channels, origin.y + header), IM_COL32(0, 0, 0, 70), 1.f);
	ImGui::SetCursorPos(ImVec2(rail + 8.f, header + 4.f));
	ImGui::BeginChild("channels", ImVec2(channels - 16.f, size.y - header - panel - 8.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 2.f));
	loungeCategory(tr("Voice chat", "Chat de voz"));
	for (Page page : { Page::Status, Page::Sound, Page::Microphone })
	{
		loungeChannel(pageName(page), page);
	}
	loungeCategory(tr("People", "Pessoas"));
	loungeChannel(pageName(Page::Players), Page::Players);
	loungeCategory(tr("Settings", "Configurações"));
	for (Page page : { Page::Interface, Page::About })
	{
		loungeChannel(pageName(page), page);
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();

	// User panel: who you are, microphone and sound switches.
	const ImVec2 panelTop(origin.x + rail, origin.y + size.y - panel);
	draw->AddRectFilled(panelTop, ImVec2(panelTop.x + channels, origin.y + size.y), col(mix(t.railBg, t.sidebarBg, 0.45f)));
	const char* title = nullptr;
	const char* detail = nullptr;
	ImVec4 stateColor;
	connectionState(status, title, detail, stateColor);
	const ImVec2 avatar(panelTop.x + 26.f, panelTop.y + panel * 0.5f);
	draw->AddCircleFilled(avatar, 16.f, col(t.accent));
	const char* you = tr("You", "Você");
	const char initial[2] = { you[0], 0 };
	const ImVec2 initialSize = ImGui::CalcTextSize(initial);
	draw->AddText(ImVec2(avatar.x - initialSize.x * 0.5f, avatar.y - initialSize.y * 0.5f), IM_COL32(255, 255, 255, 255), initial);
	draw->AddCircleFilled(ImVec2(avatar.x + 11.f, avatar.y + 11.f), 6.f, col(mix(t.railBg, t.sidebarBg, 0.45f)));
	draw->AddCircleFilled(ImVec2(avatar.x + 11.f, avatar.y + 11.f), 4.f, col(stateColor));
	draw->AddText(ImVec2(avatar.x + 24.f, panelTop.y + 9.f), IM_COL32(255, 255, 255, 255), you);
	draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f, ImVec2(avatar.x + 24.f, panelTop.y + 30.f), col(t.muted), title);
	const float button = 32.f;
	const ImVec2 micButton(panelTop.x + channels - button * 2.f - 12.f, panelTop.y + (panel - button) * 0.5f);
	const ImVec2 soundButton(panelTop.x + channels - button - 8.f, micButton.y);
	if (panelButton("##mic", micButton, button))
	{
		settings.micEnabled = !settings.micEnabled;
		changed = true;
	}
	if (panelButton("##sound", soundButton, button))
	{
		settings.soundEnabled = !settings.soundEnabled;
		changed = true;
	}
	drawMicrophone(draw, ImVec2(micButton.x + button * 0.5f, micButton.y + button * 0.5f), 22.f,
		settings.micEnabled ? IM_COL32(181, 186, 193, 255) : IM_COL32(242, 63, 67, 255), !settings.micEnabled);
	drawHeadset(draw, ImVec2(soundButton.x + button * 0.5f, soundButton.y + button * 0.5f), 22.f,
		settings.soundEnabled ? IM_COL32(181, 186, 193, 255) : IM_COL32(242, 63, 67, 255), !settings.soundEnabled);

	// Content with the channel header.
	const float left = rail + channels;
	draw->AddText(ImVec2(origin.x + left + 18.f, origin.y + (header - ImGui::GetTextLineHeight()) * 0.5f), col(t.muted), "#");
	draw->AddText(ImVec2(origin.x + left + 36.f, origin.y + (header - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(255, 255, 255, 255), pageName(g_page));
	draw->AddLine(ImVec2(origin.x + left, origin.y + header), ImVec2(origin.x + size.x, origin.y + header), IM_COL32(0, 0, 0, 70), 1.f);
	ImGui::SetCursorPos(ImVec2(size.x - 44.f, 10.f));
	closeButton("X", ImVec2(28.f, 28.f));
	ImGui::SetCursorPos(ImVec2(left, header + 1.f));
	changed |= pageArea(ImVec2(0.f, 0.f), ImVec2(24.f, 14.f));
	ImGui::End();
	return changed;
}

// --- Neon: pill tabs on top, floating card ---

bool drawNeon()
{
	const Theme& t = *g_theme;
	if (!beginMenuWindow(ImVec2(800.f, 540.f)))
	{
		return false;
	}
	const VoiceStatus& status = *g_pageStatus;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetWindowPos();
	const ImVec2 size = ImGui::GetWindowSize();
	const auto col = [](const ImVec4& c) { return ImGui::GetColorU32(c); };

	// Soft glows in the corners.
	for (int i = 6; i > 0; --i)
	{
		const float f = static_cast<float>(i) / 6.f;
		draw->AddCircleFilled(ImVec2(origin.x + 40.f, origin.y + 20.f), 150.f * f, col(alpha(t.accent, 0.018f)), 48);
		draw->AddCircleFilled(ImVec2(origin.x + size.x - 40.f, origin.y + size.y - 20.f), 170.f * f, col(alpha(t.accent2, 0.016f)), 48);
	}

	// Logo and close.
	ImGui::SetCursorPos(ImVec2(26.f, 18.f));
	titleText("VOICE", t.accent);
	ImGui::SameLine(0.f, 4.f);
	titleText("BRIDGE", t.accent2);
	ImGui::SameLine(0.f, 10.f);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7.f);
	ImGui::TextColored(t.muted, "v" VOICE_BRIDGE_VERSION);
	ImGui::SetCursorPos(ImVec2(size.x - 50.f, 16.f));
	closeButton("X", ImVec2(28.f, 28.f));

	// Pill tabs.
	ImGui::SetCursorPos(ImVec2(24.f, 66.f));
	for (const PageInfo& info : kPages)
	{
		const char* label = tr(info.name[0], info.name[1]);
		const ImVec2 text = ImGui::CalcTextSize(label);
		const ImVec2 pill(text.x + 30.f, text.y + 14.f);
		const ImVec2 position = ImGui::GetCursorScreenPos();
		ImGui::PushID(label);
		if (ImGui::InvisibleButton("##tab", pill))
		{
			g_page = info.page;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool selected = g_page == info.page;
		const ImVec2 end(position.x + pill.x, position.y + pill.y);
		if (selected)
		{
			draw->AddRect(ImVec2(position.x - 2.f, position.y - 2.f), ImVec2(end.x + 2.f, end.y + 2.f), col(alpha(t.accent, 0.35f)), pill.y, 0, 3.f);
			draw->AddRectFilled(position, end, col(t.accent), pill.y);
		}
		else if (hovered)
		{
			draw->AddRect(position, end, col(alpha(t.accent2, 0.6f)), pill.y, 0, 1.5f);
		}
		draw->AddText(ImVec2(position.x + 15.f, position.y + 7.f), col(selected ? t.windowBg : hovered ? t.text : t.muted), label);
		ImGui::SameLine(0.f, 8.f);
	}
	ImGui::NewLine();
	const float lineY = origin.y + 112.f;
	draw->AddRectFilledMultiColor(ImVec2(origin.x + 24.f, lineY), ImVec2(origin.x + size.x - 24.f, lineY + 2.f), col(t.accent), col(t.accent2), col(t.accent2),
		col(t.accent));

	// Floating card with the page.
	ImGui::SetCursorPos(ImVec2(24.f, 126.f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, alpha(t.cardBg, 0.92f));
	const bool changed = pageArea(ImVec2(size.x - 48.f, size.y - 126.f - 44.f), ImVec2(22.f, 16.f));
	ImGui::PopStyleColor();

	// Status strip.
	const char* title = nullptr;
	const char* detail = nullptr;
	ImVec4 stateColor;
	connectionState(status, title, detail, stateColor);
	const float stripY = origin.y + size.y - 30.f;
	draw->AddCircleFilled(ImVec2(origin.x + 32.f, stripY + 8.f), 5.f, col(stateColor));
	draw->AddCircleFilled(ImVec2(origin.x + 32.f, stripY + 8.f), 9.f, col(alpha(stateColor, 0.25f)));
	draw->AddText(ImVec2(origin.x + 46.f, stripY), col(t.text), title);
	char hint[64];
	std::snprintf(hint, sizeof(hint), "%s / Esc %s", keyName(static_cast<uint8_t>(GetSettings().menuKey)).c_str(), tr("closes", "fecha"));
	draw->AddText(ImVec2(origin.x + size.x - 28.f - ImGui::CalcTextSize(hint).x, stripY), col(t.muted), hint);
	ImGui::End();
	return changed;
}

// --- Terminal: monospace, square, amber ---

bool drawTerminal()
{
	const Theme& t = *g_theme;
	if (g_monoFont)
	{
		ImGui::PushFont(g_monoFont);
	}
	if (!beginMenuWindow(ImVec2(780.f, 520.f)))
	{
		if (g_monoFont)
		{
			ImGui::PopFont();
		}
		return false;
	}
	const VoiceStatus& status = *g_pageStatus;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetWindowPos();
	const ImVec2 size = ImGui::GetWindowSize();
	const auto col = [](const ImVec4& c) { return ImGui::GetColorU32(c); };
	const bool cursor = GetTickCount64() % 1000 < 550;

	// Header: program, page and a blinking cursor.
	ImGui::SetCursorPos(ImVec2(16.f, 12.f));
	ImGui::TextColored(t.accent, "VOICE-BRIDGE v" VOICE_BRIDGE_VERSION " :: %s%s", upper(pageName(g_page)).c_str(), cursor ? "_" : "");
	ImGui::SetCursorPos(ImVec2(size.x - 52.f, 8.f));
	closeButton("[X]", ImVec2(40.f, 26.f));
	draw->AddLine(ImVec2(origin.x + 12.f, origin.y + 40.f), ImVec2(origin.x + size.x - 12.f, origin.y + 40.f), col(alpha(t.accent, 0.5f)));

	// Menu column.
	const float column = 176.f;
	ImGui::SetCursorPos(ImVec2(12.f, 52.f));
	ImGui::BeginChild("menu", ImVec2(column, size.y - 52.f - 44.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	for (const PageInfo& info : kPages)
	{
		const std::string label = upper(tr(info.name[0], info.name[1]));
		const ImVec2 position = ImGui::GetCursorScreenPos();
		const ImVec2 item(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() + 10.f);
		ImGui::PushID(info.name[0]);
		if (ImGui::InvisibleButton("##item", item))
		{
			g_page = info.page;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool selected = g_page == info.page;
		if (selected)
		{
			draw->AddRectFilled(position, ImVec2(position.x + item.x, position.y + item.y), col(t.accent));
		}
		const std::string text = (selected ? "> " : hovered ? "- " : "  ") + label;
		ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 6.f, position.y + 5.f), col(selected ? t.windowBg : hovered ? t.accent : t.muted), text.c_str());
	}
	ImGui::EndChild();
	draw->AddLine(ImVec2(origin.x + column + 20.f, origin.y + 52.f), ImVec2(origin.x + column + 20.f, origin.y + size.y - 44.f), col(alpha(t.accent, 0.3f)));

	// Page.
	ImGui::SetCursorPos(ImVec2(column + 28.f, 48.f));
	const bool changed = pageArea(ImVec2(size.x - column - 40.f, size.y - 48.f - 44.f), ImVec2(8.f, 6.f));

	// Footer like a status line.
	draw->AddLine(ImVec2(origin.x + 12.f, origin.y + size.y - 36.f), ImVec2(origin.x + size.x - 12.f, origin.y + size.y - 36.f), col(alpha(t.accent, 0.5f)));
	const char* server = status.server == ServerKind::VoiceBridge ? "voice-bridge" : status.server == ServerKind::SampVoice ? "sampvoice" : "none";
	const char* link = status.transport == vb::transport::udp ? "udp" : status.transport == vb::transport::tunnel ? "tunnel" : "offline";
	char footer[160];
	std::snprintf(footer, sizeof(footer), "%s/ESC: %s | srv: %s | link: %s", upper(keyName(static_cast<uint8_t>(GetSettings().menuKey)).c_str()).c_str(),
		upper(tr("close", "fechar")).c_str(), server, link);
	draw->AddText(ImVec2(origin.x + 16.f, origin.y + size.y - 27.f), col(t.muted), footer);
	ImGui::End();
	if (g_monoFont)
	{
		ImGui::PopFont();
	}
	return changed;
}

void drawMenu(const VoiceStatus& status)
{
	const int index = std::clamp(GetSettings().menuTheme, 0, kThemeCount - 1);
	g_theme = &kThemes[index];
	if (g_styledTheme != index)
	{
		applyStyle();
		g_styledTheme = index;
	}
	waitForMenuKey();
	g_pageStatus = &status;
	bool changed = false;
	switch (g_theme->layout)
	{
	case Layout::Classic:
		changed = drawClassic();
		break;
	case Layout::Lounge:
		changed = drawLounge();
		break;
	case Layout::Neon:
		changed = drawNeon();
		break;
	case Layout::Terminal:
		changed = drawTerminal();
		break;
	}
	g_pageStatus = nullptr;
	if (changed)
	{
		VoiceClient::Get().settingsChanged();
	}
}

void applyStyle()
{
	const Theme& t = *g_theme;
	ImGuiStyle& style = ImGui::GetStyle();
	ImGui::StyleColorsDark(&style);
	style.WindowRounding = t.windowRounding;
	style.ChildRounding = t.windowRounding * 0.8f;
	style.FrameRounding = t.frameRounding;
	style.GrabRounding = t.frameRounding;
	style.PopupRounding = t.frameRounding;
	style.ScrollbarRounding = t.frameRounding;
	style.WindowBorderSize = t.layout == Layout::Terminal ? 1.f : 0.f;
	style.ChildBorderSize = 1.f;
	style.FramePadding = ImVec2(10.f, 7.f);
	style.ItemSpacing = ImVec2(10.f, 9.f);
	style.GrabMinSize = 14.f;
	ImVec4* colors = style.Colors;
	colors[ImGuiCol_WindowBg] = t.windowBg;
	colors[ImGuiCol_ChildBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
	colors[ImGuiCol_PopupBg] = alpha(mix(t.windowBg, t.cardBg, 0.6f), 0.98f);
	colors[ImGuiCol_Border] = t.layout == Layout::Terminal ? alpha(t.accent, 0.6f) : alpha(t.text, 0.08f);
	colors[ImGuiCol_FrameBg] = t.frameBg;
	colors[ImGuiCol_FrameBgHovered] = t.frameHover;
	colors[ImGuiCol_FrameBgActive] = mix(t.frameHover, t.accent, 0.12f);
	colors[ImGuiCol_SliderGrab] = t.accent;
	colors[ImGuiCol_SliderGrabActive] = t.accent2;
	colors[ImGuiCol_CheckMark] = t.accent;
	colors[ImGuiCol_Button] = t.button;
	colors[ImGuiCol_ButtonHovered] = t.buttonHover;
	colors[ImGuiCol_ButtonActive] = alpha(t.accent, 0.6f);
	colors[ImGuiCol_Header] = alpha(t.accent, 0.25f);
	colors[ImGuiCol_HeaderHovered] = alpha(t.accent, 0.35f);
	colors[ImGuiCol_HeaderActive] = alpha(t.accent, 0.45f);
	colors[ImGuiCol_Separator] = t.separator;
	colors[ImGuiCol_TableRowBgAlt] = alpha(t.text, 0.025f);
	colors[ImGuiCol_TableHeaderBg] = t.cardBg;
	colors[ImGuiCol_TableBorderLight] = t.separator;
	colors[ImGuiCol_Text] = t.text;
	colors[ImGuiCol_TextDisabled] = t.muted;
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
	colors[ImGuiCol_ScrollbarGrab] = alpha(t.muted, 0.35f);
	colors[ImGuiCol_ScrollbarGrabHovered] = alpha(t.muted, 0.5f);
	colors[ImGuiCol_ScrollbarGrabActive] = alpha(t.accent, 0.6f);
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
	const std::string mono = std::string(fonts) + "\\Fonts\\consola.ttf";
	if (GetFileAttributesA(mono.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		g_monoFont = io.Fonts->AddFontFromFileTTF(mono.c_str(), 16.f, nullptr, io.Fonts->GetGlyphRangesDefault());
		const std::string monoBold = std::string(fonts) + "\\Fonts\\consolab.ttf";
		g_monoTitleFont = io.Fonts->AddFontFromFileTTF(GetFileAttributesA(monoBold.c_str()) != INVALID_FILE_ATTRIBUTES ? monoBold.c_str() : mono.c_str(),
			21.f, nullptr, io.Fonts->GetGlyphRangesDefault());
	}
	g_theme = &kThemes[std::clamp(GetSettings().menuTheme, 0, kThemeCount - 1)];
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
	if (g_inPresent)
	{
		return g_basePresent(device, source, destination, window, dirty);
	}
	struct Guard
	{
		Guard() { g_inPresent = true; }
		~Guard() { g_inPresent = false; }
	} guard;
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
		// The pause menu (Esc) and the player list (TAB) cover the game:
		// icons above heads or the speaker list would float over them.
		if (g_menu || (!game::IsMenuActive() && !samp::IsScoreboardActive()))
		{
			drawHud(status);
		}
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
	if (g_inReset)
	{
		return g_baseReset(device, parameters);
	}
	struct Guard
	{
		Guard() { g_inReset = true; }
		~Guard() { g_inReset = false; }
	} guard;
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
		if (!g_basePresent)
		{
			g_basePresent = g_present;
		}
	}
	if (vtable[kResetIndex] != reinterpret_cast<void*>(&resetHook))
	{
		g_reset = reinterpret_cast<ResetFn>(vtable[kResetIndex]);
		if (!g_baseReset)
		{
			g_baseReset = g_reset;
		}
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
		if (g_window && !IsWindow(g_window))
		{
			return true; // the game is closing
		}
		// Minimised or in the background (alt+tab) the game stops drawing:
		// that is not a lost hook.
		if (g_window && (IsIconic(g_window) || GetForegroundWindow() != g_window))
		{
			g_lastPresent = GetTickCount64();
			continue;
		}
		IDirect3DDevice9* current = nullptr;
		void** vtable = nullptr;
		void* present = nullptr;
		if (!samp::Read(kGameDevice, current) || !current || !samp::SafeRead(current, &vtable, sizeof(vtable)) || !vtable
			|| !samp::SafeRead(&vtable[kPresentIndex], &present, sizeof(present)))
		{
			continue;
		}
		if (present == reinterpret_cast<void*>(&presentHook) || GetTickCount64() - g_lastPresent < 2000)
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
void PreviewFrame(IDirect3DDevice9* device, int page, bool portuguese, int theme)
{
	GetSettings().menuTheme = theme;
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

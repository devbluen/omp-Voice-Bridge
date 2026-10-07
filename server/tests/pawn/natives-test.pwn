// Runtime smoke test of the natives: compile against <a_samp> or <open.mp>
// and load as the gamemode.  Every line must end with "ok".
#if defined USE_OPEN_MP
	#include <open.mp>
#else
	#include <a_samp>
#endif
#include <voice-bridge>
#include <sampvoice>

main() {}

check(const name[], bool:condition)
{
	printf("[vbtest] %s %s", name, condition ? ("ok") : ("FAILED"));
}

public OnGameModeInit()
{
	new version[16];
	VB_GetPluginVersion(version, sizeof version);
	check("version", strlen(version) > 0);
	check("port", VB_GetVoicePort() > 0);

	new global = VB_CreateGlobalStream(0xFFFF0000, "Global");
	check("global stream", global != VB_INVALID_STREAM && VB_IsValidStream(global));
	check("stream type", VB_GetStreamType(global) == VB_STREAM_GLOBAL);

	new point = VB_CreateStaticStreamAtPoint(25.0, 1.0, 2.0, 3.0, 0, "Point");
	new Float:x, Float:y, Float:z;
	check("point position", VB_GetStreamPosition(point, x, y, z) && x == 1.0 && y == 2.0 && z == 3.0);
	check("distance", VB_GetStreamDistance(point) == 25.0);
	check("set distance", VB_SetStreamDistance(point, 40.0) && VB_GetStreamDistance(point) == 40.0);

	check("dynamic at point", VB_CreateDynamicStreamAtPoint(30.0, 5, 0.0, 0.0, 0.0) != VB_INVALID_STREAM);
	check("stream at missing player", VB_CreateDynamicStreamAtPlayer(30.0, VB_INFINITE, 999) == VB_INVALID_STREAM);

	check("parameter", VB_SetStreamParameter(global, VB_PARAM_VOLUME, 0.5) && VB_GetStreamParameter(global, VB_PARAM_VOLUME) == 0.5);
	check("parameter default", VB_GetStreamParameter(point, VB_PARAM_VOLUME) == 1.0);
	check("legacy parameter", SvStreamParameterGet(SV_STREAM:global, SV_PARAMETER_VOLUME) == 0.5);

	new radio = VB_CreatePresetEffect(VB_PRESET_RADIO);
	check("preset effect", radio != VB_INVALID_EFFECT && VB_AttachEffect(radio, global));
	new reverb = SvEffectCreateReverb(0, 0.0, -4.0, 1000.0, 0.5);
	check("legacy effect", reverb != SV_NULL && VB_IsValidEffect(reverb));
	SvEffectAttachStream(reverb, global);
	check("detach effect", VB_DetachEffect(reverb, global));

	new legacy = SvCreateDLStreamAtPoint(20.0, SV_INFINITY, 0.0, 0.0, 0.0, 0xff0000ff, "Local");
	check("legacy dynamic stream", legacy != SV_NULL);
	check("legacy handle in VB API", VB_GetStreamType(legacy) == VB_STREAM_DYNAMIC_POINT);
	check("no plugin for player 0", !VB_HasPlugin(0) && SvGetVersion(0) == 0);
	check("delete", VB_DeleteStream(point) && !VB_IsValidStream(point));
	print("[vbtest] done");
	return 1;
}

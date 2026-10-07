/*
 *  Voice Bridge example 05 - moderation
 *
 *  /vinfo <id>      voice client, version and microphone of a player
 *  /vmute <id>      blocks a player's microphone
 *  /vunmute <id>
 *  /vblock <id>     you stop hearing a player (only you)
 */

#include <open.mp>
#include <voice-bridge>

main() {}

public VB_OnPlayerStartTalking(playerid)
{
	SetPlayerChatBubble(playerid, "(talking)", 0xCCCCCCFF, 15.0, 1000);
	return 1;
}

public VB_OnPlayerClientStatus(playerid, bool:micavailable, bool:micmuted, bool:soundmuted)
{
	if (soundmuted)
	{
		SendClientMessage(playerid, 0xFFAA00FF, "You turned voice chat off in the voice menu (F11).");
	}
	return 1;
}

ShowVoiceInfo(playerid, target)
{
	new client[16], version[16], message[144];
	switch (VB_GetClientType(target))
	{
		case VB_CLIENT_VOICEBRIDGE: client = "Voice Bridge";
		case VB_CLIENT_SAMPVOICE: client = "SampVoice";
		default: client = "none";
	}
	VB_GetClientVersionString(target, version);
	format(message, sizeof message, "Player %d: %s %s | microphone: %s | talking: %s | muted: %s",
		target, client, version,
		VB_HasMicrophone(target) ? ("yes") : ("no"),
		VB_IsTalking(target) ? ("yes") : ("no"),
		VB_IsPlayerMuted(target) ? ("yes") : ("no"));
	SendClientMessage(playerid, 0xFFFFFFFF, message);
}

// "/cmd 12" -> command = "/cmd", target = 12
bool:ParseCommand(const text[], command[16], &target)
{
	new space = strfind(text, " ");
	if (space == -1)
	{
		return false;
	}
	strmid(command, text, 0, space, 16);
	target = strval(text[space + 1]);
	return bool:IsPlayerConnected(target);
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	new command[16], target;
	if (!ParseCommand(cmdtext, command, target))
	{
		return 0;
	}
	if (!strcmp(command, "/vinfo", true))
	{
		ShowVoiceInfo(playerid, target);
		return 1;
	}
	if (!strcmp(command, "/vmute", true))
	{
		VB_MutePlayer(target, true);
		VB_Notify(target, "You were muted by an administrator", 0xFF5555FF, 5000);
		return 1;
	}
	if (!strcmp(command, "/vunmute", true))
	{
		VB_MutePlayer(target, false);
		VB_Notify(target, "You can talk again", 0x66FF66FF);
		return 1;
	}
	if (!strcmp(command, "/vblock", true))
	{
		new bool:blocked = !VB_IsSpeakerBlocked(playerid, target);
		VB_BlockSpeaker(playerid, target, blocked);
		SendClientMessage(playerid, 0xFFFFFFFF, blocked ? ("Player blocked.") : ("Player unblocked."));
		return 1;
	}
	return 0;
}

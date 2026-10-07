/*
 *  Exemplo 06 do Voice Bridge - gamemode do SampVoice sem alterações
 *
 *  Este é o exemplo oficial do SampVoice.  Ele compila com o sampvoice.inc
 *  que acompanha o Voice Bridge e roda no plugin Voice Bridge.  Os jogadores
 *  podem usar tanto o sampvoice.asi quanto o voice-bridge.asi.
 */

#include <open.mp>
#include <sampvoice>

new SV_GSTREAM:gstream = SV_NULL;
new SV_LSTREAM:lstream[MAX_PLAYERS] = { SV_NULL, ... };

main() {}

public SV_VOID:OnPlayerActivationKeyPress(SV_UINT:playerid, SV_UINT:keyid)
{
	if (keyid == 0x42 && lstream[playerid]) SvAttachSpeakerToStream(lstream[playerid], playerid);
	if (keyid == 0x5A && gstream) SvAttachSpeakerToStream(gstream, playerid);
}

public SV_VOID:OnPlayerActivationKeyRelease(SV_UINT:playerid, SV_UINT:keyid)
{
	if (keyid == 0x42 && lstream[playerid]) SvDetachSpeakerFromStream(lstream[playerid], playerid);
	if (keyid == 0x5A && gstream) SvDetachSpeakerFromStream(gstream, playerid);
}

public OnPlayerConnect(playerid)
{
	if (SvGetVersion(playerid) == SV_NULL)
	{
		SendClientMessage(playerid, -1, "Could not find plugin sampvoice.");
	}
	else if (SvHasMicro(playerid) == SV_FALSE)
	{
		SendClientMessage(playerid, -1, "The microphone could not be found.");
	}
	else if ((lstream[playerid] = SvCreateDLStreamAtPlayer(40.0, SV_INFINITY, playerid, 0xff0000ff, "Local")))
	{
		SendClientMessage(playerid, -1, "Press Z to talk to global chat and B to talk to local chat.");
		if (gstream) SvAttachListenerToStream(gstream, playerid);
		SvAddKey(playerid, 0x42);
		SvAddKey(playerid, 0x5A);
	}
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (lstream[playerid])
	{
		SvDeleteStream(lstream[playerid]);
		lstream[playerid] = SV_NULL;
	}
	return 1;
}

public OnGameModeInit()
{
	gstream = SvCreateGStream(0xffff0000, "Global");
	return 1;
}

public OnGameModeExit()
{
	if (gstream) SvDeleteStream(gstream);
	return 1;
}

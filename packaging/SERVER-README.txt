Voice Bridge - servidor (open.mp e SA-MP)
==========================================

OPEN.MP
  1. components/voice-bridge.(dll|so)  ->  pasta "components" do servidor
  2. include/*.inc                     ->  qawno/include

SA-MP 0.3.7-R2 / 0.3.DL
  1. plugins/voice-bridge.(dll|so)     ->  pasta "plugins" do servidor
  2. server.cfg, linha plugins:  voice-bridge  (Linux: voice-bridge.so)
     Remova o sampvoice dessa linha.
  3. include/*.inc                     ->  pawno/include

FIREWALL
  Libere a porta UDP de voz: a porta do jogo + 1 (7777 -> 7778).
  O log do servidor mostra a porta ao iniciar.

CONFIGURACAO (opcional)
  config.json:  "voice_bridge": { "port": 7778, "bitrate": 24000 }
  server.cfg:   voice_port 7778
  Todas as opcoes estao no README do projeto.

PAWN
  #include <open.mp>        (ou <a_samp>)
  #include <voice-bridge>
  Gamemodes do SampVoice (#include <sampvoice>) funcionam sem alteracoes.

JOGADORES
  Funcionam o voice-bridge.asi (Voice Bridge) e o sampvoice.asi (SampVoice).
  Para aceitar so um deles: voice_allow_sampvoice / voice_allow_voicebridge.

==========================================================================

Voice Bridge - server (open.mp and SA-MP)

OPEN.MP
  1. components/voice-bridge.(dll|so)  ->  server "components" folder
  2. include/*.inc                     ->  qawno/include

SA-MP 0.3.7-R2 / 0.3.DL
  1. plugins/voice-bridge.(dll|so)     ->  server "plugins" folder
  2. server.cfg, plugins line:  voice-bridge  (Linux: voice-bridge.so)
     Remove sampvoice from that line.
  3. include/*.inc                     ->  pawno/include

FIREWALL
  Open the voice UDP port: game port + 1 (7777 -> 7778).
  The server log prints the port at startup.

CONFIGURATION (optional)
  config.json:  "voice_bridge": { "port": 7778, "bitrate": 24000 }
  server.cfg:   voice_port 7778
  Every option is listed in the project README.

PAWN
  #include <open.mp>        (or <a_samp>)
  #include <voice-bridge>
  SampVoice gamemodes (#include <sampvoice>) work unchanged.

PLAYERS
  Both voice-bridge.asi (Voice Bridge) and sampvoice.asi (SampVoice) work.
  To accept only one: voice_allow_sampvoice / voice_allow_voicebridge.

==========================================================================

Voice Bridge - by devbluen
https://github.com/devbluen/omp-Voice-Bridge

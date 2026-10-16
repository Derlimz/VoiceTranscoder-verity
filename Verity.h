#pragma once

// VERITY: per-client voice recording to WAV for the Verity AMXX plugin.
// All entry points are called from the engine thread only.

#include <cstddef>
#include <cstdint>

// Registers the vtc_verity_* cvars and the vtc_verity_listen server command.
void Verity_Init();

// Clears the record flag and any buffered audio (connect / disconnect).
void Verity_ResetClient(size_t clientIndex);

// Called after VTC decoded one incoming voice packet (8 kHz mono int16).
// vpcMask has bit N set for each new-codec VPC_* command seen in the packet.
void Verity_OnVoiceDecoded(size_t clientIndex, bool hasNewCodec, int sampleRate, uint32_t vpcMask,
	size_t receivedBytes, const int16_t *samples8k, size_t sampleCount);

// Speaker slot for sounds started by PlaySound (vtc_verity_sender), 0 = none.
size_t Verity_PlaySender();

// Called right before VTC fires OnClientStopSpeak; writes the WAV if there is enough audio
// and sets the vtc_verity_lastwav cvar for the forward that follows.
void Verity_OnStopSpeak(size_t clientIndex);

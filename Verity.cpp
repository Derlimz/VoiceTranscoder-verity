// VERITY: per-client voice recording to WAV for the Verity AMXX plugin.
//
// vtc_verity_listen <clientIndex> <0|1> turns recording on/off for one client.
// While on, every decoded voice packet (8 kHz mono int16) is resampled to 16 kHz and
// appended to the client's buffer, capped at vtc_verity_maxsec seconds.
// When VTC detects the client stopped speaking, a buffer of >= 0.4 s is written to
// <gamedir>/addons/verity/voice/<clientIndex>_<seq>.wav.tmp and renamed to .wav;
// vtc_verity_lastwav then holds the gamedir-relative path during OnClientStopSpeak.

#include "Verity.h"
#include <extdll.h>
#include <meta_api.h>
#include "Main.h"
#include "VoiceCodecs/SILK/SKP_Silk_SigProc_FIX.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace {
	constexpr int VERITY_IN_RATE = 8000; // VTC decodes every codec to 8 kHz
	constexpr int VERITY_OUT_RATE = 16000;
	constexpr size_t VERITY_MIN_SAMPLES = VERITY_OUT_RATE * 4 / 10; // 0.4 s
	const char VERITY_VOICE_DIR[] = "addons/verity/voice";

	struct verityClient_t {
		bool Listening;
		bool ResamplerReady;
		bool Capped;
		SKP_Silk_resampler_state_struct Resampler;
		std::vector<int16_t> Samples16k;
	};

	verityClient_t g_verityClients[MAX_CLIENTS];
	unsigned g_veritySeq;

	cvar_t g_cvarVerityMaxSec = {"vtc_verity_maxsec", "8", FCVAR_EXTDLL, 0, nullptr};
	cvar_t *g_pcvarVerityMaxSec;
	cvar_t g_cvarVerityDebug = {"vtc_verity_debug", "0", FCVAR_EXTDLL, 0, nullptr};
	cvar_t *g_pcvarVerityDebug;
	cvar_t g_cvarVerityLastWav = {"vtc_verity_lastwav", "", FCVAR_EXTDLL, 0, nullptr};

	bool IsValidClientIndex(size_t clientIndex) {
		return clientIndex >= 1 && clientIndex <= (size_t)gpGlobals->maxClients;
	}

	bool IsDebug() {
		return g_pcvarVerityDebug != nullptr && g_pcvarVerityDebug->value != 0.0f;
	}

	size_t MaxSamples() {
		float seconds = g_pcvarVerityMaxSec->value;
		seconds = std::min(std::max(seconds, 1.0f), 30.0f);
		return size_t(seconds * VERITY_OUT_RATE);
	}

	void ClearBuffer(verityClient_t &vc) {
		vc.Samples16k.clear();
		vc.ResamplerReady = false;
		vc.Capped = false;
	}

	void MakeDir(const std::string &path) {
#ifdef _WIN32
		_mkdir(path.c_str());
#else
		mkdir(path.c_str(), 0755);
#endif
	}

	bool MoveReplacing(const std::string &from, const std::string &to) {
#ifdef _WIN32
		return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
		return rename(from.c_str(), to.c_str()) == 0;
#endif
	}

	void PutU16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
	void PutU32(uint8_t *p, uint32_t v) { PutU16(p, uint16_t(v)); PutU16(p + 2, uint16_t(v >> 16)); }

	// 16 kHz, mono, signed 16-bit little-endian PCM WAV
	bool WriteWav(const std::string &path, const int16_t *samples, size_t sampleCount) {
		FILE *file = fopen(path.c_str(), "wb");
		if (file == nullptr) {
			return false;
		}

		uint32_t dataBytes = uint32_t(sampleCount * sizeof(int16_t));
		uint8_t header[44];
		memcpy(&header[0], "RIFF", 4);
		PutU32(&header[4], 36 + dataBytes);
		memcpy(&header[8], "WAVEfmt ", 8);
		PutU32(&header[16], 16);                 // fmt chunk size
		PutU16(&header[20], 1);                  // PCM
		PutU16(&header[22], 1);                  // mono
		PutU32(&header[24], VERITY_OUT_RATE);    // sample rate
		PutU32(&header[28], VERITY_OUT_RATE * 2); // byte rate
		PutU16(&header[32], 2);                  // block align
		PutU16(&header[34], 16);                 // bits per sample
		memcpy(&header[36], "data", 4);
		PutU32(&header[40], dataBytes);

		bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
		ok = ok && fwrite(samples, sizeof(int16_t), sampleCount, file) == sampleCount;
		ok = (fclose(file) == 0) && ok;
		return ok;
	}

	std::string VpcMaskToString(bool hasNewCodec, uint32_t vpcMask) {
		if (!hasNewCodec) {
			return "speex";
		}

		std::string out;
		auto add = [&](uint32_t vpc, const char *name) {
			if (vpcMask & (1u << vpc)) {
				if (!out.empty()) out += '+';
				out += name;
			}
		};
		add(VPC_SETSAMPLERATE, "rate");
		add(VPC_VDATA_SILENCE, "silence");
		add(VPC_VDATA_SILK, "silk");
		add(VPC_VDATA_OPUS_PLC, "opus");
		for (uint32_t vpc = 0; vpc < 32; vpc++) {
			if ((vpcMask & (1u << vpc)) && vpc != VPC_SETSAMPLERATE && vpc != VPC_VDATA_SILENCE && vpc != VPC_VDATA_SILK && vpc != VPC_VDATA_OPUS_PLC) {
				if (!out.empty()) out += '+';
				out += "vpc" + std::to_string(vpc);
			}
		}
		return out.empty() ? "none" : out;
	}

	void Cmd_VerityListen() {
		if (CMD_ARGC() < 3) {
			LOG_CONSOLE(PLID, "Usage: vtc_verity_listen <clientIndex> <0|1>");
			return;
		}

		int clientIndex = atoi(CMD_ARGV(1));
		bool listen = atoi(CMD_ARGV(2)) != 0;
		if (!IsValidClientIndex(clientIndex)) {
			LOG_CONSOLE(PLID, "[VERITY] vtc_verity_listen: invalid client index %d", clientIndex);
			return;
		}

		auto &vc = g_verityClients[clientIndex - 1];
		if (vc.Listening == listen) {
			return;
		}

		vc.Listening = listen;
		if (!listen) {
			ClearBuffer(vc);
		}

		if (IsDebug()) {
			LOG_CONSOLE(PLID, "[VERITY] listen client %d = %d", clientIndex, listen ? 1 : 0);
		}
	}
}

void Verity_Init() {
	CVAR_REGISTER(&g_cvarVerityMaxSec);
	g_pcvarVerityMaxSec = CVAR_GET_POINTER(g_cvarVerityMaxSec.name);
	CVAR_REGISTER(&g_cvarVerityDebug);
	g_pcvarVerityDebug = CVAR_GET_POINTER(g_cvarVerityDebug.name);
	CVAR_REGISTER(&g_cvarVerityLastWav);

	g_engfuncs.pfnAddServerCommand("vtc_verity_listen", &Cmd_VerityListen);
}

void Verity_ResetClient(size_t clientIndex) {
	if (clientIndex < 1 || clientIndex > MAX_CLIENTS) {
		return;
	}

	auto &vc = g_verityClients[clientIndex - 1];
	vc.Listening = false;
	ClearBuffer(vc);
	vc.Samples16k.shrink_to_fit();
}

void Verity_OnVoiceDecoded(size_t clientIndex, bool hasNewCodec, int sampleRate, uint32_t vpcMask,
	size_t receivedBytes, const int16_t *samples8k, size_t sampleCount) {
	if (!IsValidClientIndex(clientIndex)) {
		return;
	}

	auto &vc = g_verityClients[clientIndex - 1];

	if (IsDebug()) {
		LOG_CONSOLE(PLID, "[VERITY] voice client %u \"%s\": codec=%s(%s) rate=%d bytes=%u samples8k=%u listen=%d buffered16k=%u",
			(unsigned)clientIndex, EngineUTIL::GetClientByIndex(clientIndex)->m_szPlayerName,
			hasNewCodec ? "new" : "old", VpcMaskToString(hasNewCodec, vpcMask).c_str(), sampleRate,
			(unsigned)receivedBytes, (unsigned)sampleCount, vc.Listening ? 1 : 0, (unsigned)vc.Samples16k.size());
	}

	if (!vc.Listening || sampleCount == 0) {
		return;
	}

	size_t maxSamples = MaxSamples();
	size_t buffered = vc.Samples16k.size();
	size_t take = buffered < maxSamples ? std::min(sampleCount, (maxSamples - buffered) / 2) : 0;
	if (take == 0) {
		if (!vc.Capped && IsDebug()) {
			LOG_CONSOLE(PLID, "[VERITY] client %u buffer full (%u samples), dropping the rest", (unsigned)clientIndex, (unsigned)buffered);
		}
		vc.Capped = true;
		return;
	}

	if (!vc.ResamplerReady) {
		memset(&vc.Resampler, 0, sizeof(vc.Resampler));
		SKP_Silk_resampler_init(&vc.Resampler, VERITY_IN_RATE, VERITY_OUT_RATE);
		vc.ResamplerReady = true;
		vc.Samples16k.reserve(maxSamples);
	}

	vc.Samples16k.resize(buffered + take * 2);
	SKP_Silk_resampler(&vc.Resampler, &vc.Samples16k[buffered], samples8k, (SKP_int32)take);
}

void Verity_OnStopSpeak(size_t clientIndex) {
	CVAR_SET_STRING(g_cvarVerityLastWav.name, "");

	if (!IsValidClientIndex(clientIndex)) {
		return;
	}

	auto &vc = g_verityClients[clientIndex - 1];
	if (!vc.Listening || vc.Samples16k.empty()) {
		ClearBuffer(vc);
		return;
	}

	size_t sampleCount = vc.Samples16k.size();
	if (sampleCount < VERITY_MIN_SAMPLES) {
		if (IsDebug()) {
			LOG_CONSOLE(PLID, "[VERITY] client %u utterance too short (%u samples), not written", (unsigned)clientIndex, (unsigned)sampleCount);
		}
		ClearBuffer(vc);
		return;
	}

	std::string gameDir = GET_GAME_INFO(PLID, GINFO_GAMEDIR);
	MakeDir(gameDir + "/addons");
	MakeDir(gameDir + "/addons/verity");
	MakeDir(gameDir + "/" + VERITY_VOICE_DIR);

	char relPath[128];
	snprintf(relPath, sizeof(relPath), "%s/%u_%u.wav", VERITY_VOICE_DIR, (unsigned)clientIndex, ++g_veritySeq);
	std::string finalPath = gameDir + "/" + relPath;
	std::string tmpPath = finalPath + ".tmp";

	if (WriteWav(tmpPath, vc.Samples16k.data(), sampleCount) && MoveReplacing(tmpPath, finalPath)) {
		CVAR_SET_STRING(g_cvarVerityLastWav.name, relPath);

		if (IsDebug()) {
			LOG_CONSOLE(PLID, "[VERITY] wrote %s (%u samples, %.2f s)", relPath, (unsigned)sampleCount, sampleCount / double(VERITY_OUT_RATE));
		}
	} else {
		remove(tmpPath.c_str());
		LOG_MESSAGE(PLID, "[VERITY] failed to write %s", finalPath.c_str());
	}

	ClearBuffer(vc);
}

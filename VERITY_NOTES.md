# VoiceTranscoder: Verity fork

This fork is VoiceTranscoder 2017RC5 (upstream tag `v2017rc5`, Unlicense) plus per-client voice recording for the Verity AMXX plugin.

- Repo: https://github.com/Derlimz/VoiceTranscoder-verity, branch `verity`. `upstream` points to WPMGPRoSToTeMa/VoiceTranscoder.
- CI: `.github/workflows/build.yml` runs on windows-latest with MSVC, Win32, Release and the static CRT. It checks the DLL's exports and uploads the artifact `VoiceTranscoder-verity-win32`, which holds the DLL and the stock `VoiceTranscoder.cfg`.
- Built DLL: `dist/VoiceTranscoder.dll` (583,168 bytes, SHA-256 `C60E2837EDA8E79025DDA24145AA8F4AE6A558D469E9819161CE33479E1098CE`), from run 38805525042.

## What changed
Every change is marked `// VERITY`.

| File | Change |
|---|---|
| `Verity.h`, `Verity.cpp` (new) | All the recording logic: cvars, the listen command, the per-client 16 kHz buffer, the WAV writer and the debug log. |
| `Main.cpp` | Six one-line hooks: `Verity_Init()` in `Meta_Attach`; `Verity_ResetClient()` on connect and disconnect; a VPC bitmask in the new-codec parse loop; `Verity_OnVoiceDecoded()` right after decoding; `Verity_OnStopSpeak()` just before `g_OnClientStopSpeak` in `OnFrameStarted`. Also adds `#include <string>`, which current MSVC needs to build. |
| `CMakeLists.txt` | Adds `Verity.h` and `Verity.cpp`. |
| `.github/workflows/build.yml` | The CI build. It replaces `win32-msvc-xp-static-ipo.cmake`, which needs the VS2017 `v141_xp` toolset that hosted runners no longer have. The CI build targets Windows 7 or later, not XP. |

Normal behaviour is unchanged. The recorder only reads the decoded samples. It runs before any volume change or re-encoding, and it never alters routing, muting or blocking.

## Install (separate step, needs approval)
1. Copy `dist/VoiceTranscoder.dll` and `dist/VoiceTranscoder.cfg` into `hlds/cstrike/addons/VoiceTranscoder/`. The cfg must sit next to the DLL, because VTC execs it from the plugin's own folder.
2. In `hlds/cstrike/addons/metamod/plugins.ini`, add this line **above** the amxmodx line. VTC registers its API with ReHLDS, and ReAPI's `VTC_*` natives look that API up when the module loads.
   ```
   win32 addons\VoiceTranscoder\VoiceTranscoder.dll
   win32 addons\amxmodx\dlls\amxmodx_mm.dll
   ```
3. Check: `meta list` shows VoiceTranscoder 2017RC5 running, and `vtc_verity_listen` with no arguments prints its usage line.

## Command and cvars

| Name | Default | Meaning |
|---|---|---|
| `vtc_verity_listen <clientIndex> <0\|1>` | (server command) | Turns recording on or off for player slot 1..maxplayers. Turning it off discards any audio buffered but not yet written. The flag is also cleared on connect and on disconnect, and audio unsent at disconnect is discarded. |
| `vtc_verity_maxsec` | `8` | Cap per utterance in seconds, clamped to 1..30. Audio past the cap is dropped, so the first N seconds are kept. |
| `vtc_verity_debug` | `0` | `1` logs one console line per incoming voice packet, plus listen changes, too-short or capped utterances and written files. |
| `vtc_verity_lastwav` | `""` | Set just before VTC fires OnClientStopSpeak, so it is valid inside `VTC_OnClientStopSpeak`. It holds the gamedir-relative path of the file just written for that client, such as `addons/verity/voice/3_17.wav`, or `""` if nothing was written. Read it in the forward: the next stop event overwrites it. |

A debug line looks like this:
```
[VERITY] voice client 3 "Name": codec=new(rate+opus) rate=24000 bytes=93 samples8k=160 listen=1 buffered16k=6400
```
- `codec=old(speex)` means VTC treats the client as a legacy Speex client.
- `codec=new(...)` lists the Steam voice commands found in the packet: `rate`, `silence`, `silk`, `opus`, or `vpcN` for unknown ones.
- `samples8k` is the decoded sample count at 8 kHz, since VTC decodes every codec to 8 kHz.

**S5 pass criteria:** with an HL25 Steam client talking, lines appear with `samples8k` > 0, which is about 160 per 20 ms frame. If the lines show `codec=old(speex)` for a Steam client, or VTC logs "Voice packet unknown command" or "checksum" drops, the client's codec detection or format does not match VTC. In that case S5 fails and needs investigation.

## WAV output contract
- Path: `<gamedir>/addons/verity/voice/<clientIndex>_<seq>.wav`, for example `hlds/cstrike/addons/verity/voice/3_17.wav`. The folders are created on demand.
- `seq` is one counter shared by all clients. It starts at 1 each time the DLL loads and goes up by one per written file. After a server restart, old names are reused and overwritten, so the consumer should delete files once it has processed them.
- Format: RIFF/WAVE, PCM, 1 channel, 16000 Hz, signed 16-bit little-endian, with a 44-byte header and no extra chunks. VTC decodes at 8 kHz, so the audio is 8 kHz content upsampled 2x with the SILK high-quality resampler. Nothing above 4 kHz is present.
- Atomicity: the file is written fully as `<name>.wav.tmp`, then renamed to `<name>.wav` (MoveFileEx with REPLACE_EXISTING). Consumers should only pick up `*.wav` and ignore `*.tmp`.
- When a file is written: when VTC decides the client stopped speaking (no voice for about 200 ms past the expected end of the last packet), and only if the flag is on and the buffer holds at least 0.4 s (6400 samples). Shorter clips are discarded. The buffer is reset after each stop.
- Timing: the file exists and `vtc_verity_lastwav` is set before AMXX's `VTC_OnClientStopSpeak(id)` runs, in the same server frame. The write is plain fwrite of at most 30 s × 32 KB/s, about 256 KB at the default 8 s.
- Only packets that VTC accepts are recorded. Packets dropped by its flood check (`VTC_MaxDelta`), by `sv_voiceenable 0`, or for blocked clients are not.

## Rebuilding
Push to the `verity` branch or run the workflow manually: `gh workflow run build.yml -R Derlimz/VoiceTranscoder-verity`. Portable git and gh are in `../_tools/` (`mingit/cmd/git.exe`, `gh/bin/gh.exe`). Git is not on PATH. When pushing, pass gh's credential helper on the command line instead of setting it globally:
```
git -c credential.helper= -c "credential.helper=!gh auth git-credential" push origin verity
```

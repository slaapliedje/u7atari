// Debug builds: the script step "speech N" (platform/common/script.c) starts speech track N as the
// usecode's StartSpeech intrinsic does, so the main game's speech path can be tested without
// waiting for the story to call for it.
#include "u7port.h"
#include "plat.h"
#include "voice.h"
#include "preload.h"
#include "itemcmd.h"

extern "C" void debug_play_speech(int16_t track)
{
	if ((int8_t) SpeechPlayer.selectTrack(track))
		SpeechPlayer.playFile(SpeechFileName, track);
	else
		plat_log("speech: no such track\r\n");
}

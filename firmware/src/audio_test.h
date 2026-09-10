#pragma once
#ifdef CLAWDMETER_AUDIO_TEST
void audio_test_init();
void audio_test_tick();
bool audio_test_command(const char* command);
#else
inline void audio_test_init() {}
inline void audio_test_tick() {}
inline bool audio_test_command(const char*) { return false; }
#endif

#ifndef WAV_PLAYER_H_
#define WAV_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wav_player_init(void);
bool wav_player_start(const char *filepath);
void wav_player_stop(void);
void wav_player_pause(void);
void wav_player_resume(void);
bool wav_player_is_active(void);
bool wav_player_get_next_sample(int16_t *sample);

#ifdef __cplusplus
}
#endif

#endif

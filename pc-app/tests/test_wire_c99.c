/* Compile the shared header as strict C99 and do a minimal round trip. */
#include <phonemic/wire.h>
#include <stdio.h>

int main(void) {
    uint8_t buf[PM_AUDIO_HEADER_LEN + 4];
    const uint8_t pcm[4] = {1, 2, 3, 4};
    const uint8_t* payload = NULL;
    pm_audio_header h, d;
    memset(&h, 0, sizeof h);
    h.codec = PM_CODEC_PCM16;
    h.session_id = 7;
    h.seq = 9;
    h.timestamp_us = 11;
    h.sample_rate = 48000;
    if (pm_encode_audio(buf, sizeof buf, &h, pcm, 4) != (int)sizeof buf) return 1;
    if (pm_decode_audio(buf, sizeof buf, &d, &payload) != PM_OK) return 2;
    if (d.seq != 9 || d.session_id != 7 || payload[3] != 4) return 3;
    puts("c99 ok");
    return 0;
}

/*
 * sndtest FILE [SAMPLES]: the sound card from Linux, for tools/sound_reference.py to check
 * (docs/sound.md). A tone from sound memory on the left and FILE's bytes, played from the ROM
 * where they lie, on the right, for SAMPLES of the device's clock (default 24000).
 */
#include <stdio.h>
#include <stdlib.h>

#include "shaderemu_sound.h"

#define WAVE 256

int main(int argc, char **argv)
{
	uint32_t run = argc > 2 ? strtoul(argv[2], NULL, 0) : 24000, tone_at, rom = 0, start;
	char begins[16];
	struct stat st;
	int16_t *tone;
	int file, a, b, i, changed = 0;

	if (argc < 2 || snd_open()) {
		printf(argc < 2 ? "usage: sndtest FILE [SAMPLES]\n" : "sndtest: no sound card\n");
		return 1;
	}
	a = snd_claim();
	b = snd_claim();
	tone = snd_memory(WAVE * 2, &tone_at);
	file = open(argv[1], O_RDONLY);
	if (file >= 0 && fstat(file, &st) == 0 && read(file, begins, sizeof begins) == sizeof begins)
		rom = snd_rom(file, begins, sizeof begins);
	if (a < 0 || b < 0 || !tone || !rom) {
		printf("sndtest: voices %d %d, memory %p, %s at %#x\n", a, b, (void *)tone, argv[1], rom);
		return 1;
	}
	for (i = 0; i < WAVE; i++) {
		/* Bhaskara's sine, as programs/sound has it */
		int x = i & 127, p = x * (128 - x), s = 12000 * 16 * p / (5 * 128 * 128 - 4 * p);
		tone[i] = i < 128 ? s : -s;
	}
	start = SND_CLOCK;
	snd_play(a, SND_PCM16 | SND_LOOPED | SND_SMOOTH, tone_at, WAVE, 0, snd_step(WAVE * 440), SND_VOLUME(160, 0));
	snd_play(b, SND_PCM8, rom, st.st_size < 20000 ? st.st_size : 20000, 0, snd_step(8000), SND_VOLUME(0, 200));
	while (SND_CLOCK - start < run) {
		if (!changed && SND_CLOCK - start >= run / 2) {
			SND_VOICE[a].step = snd_step(WAVE * 660);
			changed = 1;
		}
		snd_wait();
	}
	printf("sndtest: voices %d %d, rom %#x, done\n", a, b, rom);
	return 0;
}

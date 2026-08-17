/*
 * ac97.h — Intel 82801AA AC'97 audio driver. (GPLv2)
 *
 * AC'97 splits the sound card in two, and the split is worth understanding
 * because it is why there are two BARs:
 *
 *   - the *codec* (the analogue half: volume, mute, sample rate) is reached
 *     through the "native audio mixer" register bank, BAR0;
 *   - the *bus master* (the digital half: a DMA engine that walks a list of
 *     buffers and streams them at the codec) lives in BAR1.
 *
 * Both banks are in I/O port space, not memory space -- this is an ICH-era
 * device and it never moved.
 *
 * Playback is not "write samples to a FIFO".  You build a Buffer Descriptor
 * List, an array of (physical address, sample count) pairs, tell the engine
 * where the list is and which entry is the last valid one, then set the run
 * bit and the card fetches the audio itself.  Software's only job after that
 * is to stay ahead of the play position.
 */
#ifndef GNOS_AC97_H
#define GNOS_AC97_H

#include <stdint.h>

/* Find the codec, reset it, arm the DMA engine.  1 on success, 0 if absent. */
int ac97_init(void);

/* True once ac97_init() has succeeded. */
int ac97_present(void);

/*
 * Queue a 440 Hz tone and confirm the DMA engine really consumes it.
 * Prints the verdict to the debug console.  1 on pass.
 */
int ac97_selftest(void);

#endif

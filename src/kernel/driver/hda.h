/*
 * hda.h — Intel High Definition Audio controller + codec driver. (GPLv2)
 *
 * HD Audio is a different shape of device from AC'97, and the difference is
 * the whole reason this file exists next to ac97.h rather than instead of it.
 *
 * AC'97 was one chip with a fixed register map: the driver knew where the
 * volume control was because the specification said so.  HDA splits the card
 * into a *controller* (a DMA engine with a documented, fixed register block
 * in memory space) and one or more *codecs* (the analogue side) that hang off
 * a serial link and describe themselves at runtime.  Nothing about the codec
 * is known in advance -- not how many converters it has, not which pins are
 * outputs, not even how many nodes exist.  The driver asks.
 *
 * So bring-up has two halves:
 *
 *   1. The controller.  Take it out of reset, wait for it to report which
 *      codec addresses answered, then set up a DMA "stream": a buffer
 *      descriptor list, a cyclic buffer length, and a stream number.
 *
 *   2. The codec.  Walk its node tree with GET_PARAMETER verbs -- root node
 *      says how many function groups, the audio function group says how many
 *      widgets, each widget says what it is -- until a digital-to-analogue
 *      converter and an output-capable pin turn up.  Then wire them together:
 *      power on, set the sample format, tag the converter with the stream
 *      number the controller is playing, unmute, enable the pin.
 *
 * Verbs travel over the *immediate command* registers rather than the CORB
 * and RIRB ring buffers.  Real drivers use the rings because they are
 * asynchronous and interrupt-driven; the immediate interface is a synchronous
 * "write verb, poll for answer" path that every controller must implement,
 * and it makes the codec walk read like the sequence of questions it is.
 */
#ifndef GNOS_HDA_H
#define GNOS_HDA_H

#include <stdint.h>

/* Reset the controller, enumerate the codec, and configure an output path.
 * Returns 1 if a usable DAC-to-pin path was found, 0 otherwise. */
int hda_init(void);

/* True once hda_init() has succeeded. */
int hda_present(void);

/*
 * Stream 16-bit stereo 48 kHz samples to the codec.  Blocks (busy-waits on
 * the link position, the way the self-test does) until the whole buffer has
 * been copied into the DMA ring and handed to the engine.  Returns the
 * number of bytes queued, or 0 when no output path is configured.
 */
int hda_play(const void *samples, uint32_t bytes);

/*
 * Stream a 440 Hz tone and assert that the controller's DMA engine really
 * consumed it -- the link position register has to advance.  Prints the
 * verdict to the debug console.  1 on pass.
 */
int hda_selftest(void);

#endif

# Time expansion: hearing the whole call

*Part 8 of [Building a bat detector](00-introduction.md). With the last 16.8 seconds always sitting in the PSRAM ([part 7](07-psram-ring-buffer.md)), one press of a button can play a bat back ten times slower, with nothing lost. This was the payoff for all the PSRAM work, and it was the first feature in the project that worked first time.*

---

## What the heterodyne can't tell you

The heterodyne ([part 3](03-dsp.md)) is live and sensitive, but it only plays a window about ±8 kHz wide around the LO, folded about the LO. A pipistrelle's call is a steep downward sweep that flattens into a near-constant tail (the "hockey stick"). Through a heterodyne tuned near the tail, it comes out as a wet slap. The sweep is mostly outside the window, the harmonics are gone, and you can't tell which side of the LO the tail sits on.

**Time expansion** keeps everything. Record the full bandwidth, then play it back N times slower:

- every frequency is divided by N;
- every duration is multiplied by N.

At 10×, a 45 kHz pipistrelle call becomes a 4.5 kHz chirp, and a 5 ms call lasts 50 ms. The whole call survives: the sweep, the flat tail, the harmonics, and the rhythm between calls. That's the detail bat identification relies on.

Its weakness is that it isn't live: you hear the past, ten times slower. That's exactly what the ring buffer is for. The PSRAM has been recording all along, so the bat you just heard on the heterodyne is already stored.

## Version 1: one button, fixed 10×

Press PLAYBACK and the detector plays the **last 2 seconds** of raw audio at **10× slower**, which takes 20 seconds. Press it again to stop early. While it plays, the screen shows "PLAY 10x  20s" in place of the LO readout, counting down.

### Why a fixed 10×

I considered scaling each call to a fixed pitch (say 2 kHz) automatically, and decided against it:

- **Pitch identifies species.** At 10×, a noctule (20 kHz) plays at 2 kHz, a pipistrelle (45 kHz) at 4.5 kHz, and a *Myotis* sweep falls from about 8 kHz to 3 kHz. Scaling every call to 2 kHz would make them all sound alike, and throw away the most useful cue.
- **Duration is predictable.** 2 seconds always takes 20. Auto-scaling a pipistrelle to 2 kHz would need about 22×. A lesser horseshoe (110 kHz) would need 55×, and 2 seconds would take nearly two minutes.
- **10× is the convention.** Reference recordings of bat calls are published at 10×, so what you hear can be compared with them directly.
- **It fits the hardware.** The mic's rated 20–80 kHz becomes 2–8 kHz: inside the speaker's range, and below the amp's 6–10 kHz input filter. The honest exception is the horseshoes: 82–112 kHz becomes 8–11 kHz, right at the filter's edge. They're above the mic's rated band anyway.
- **Cleaner sound,** for the reason in the next section.

The engine steps through the recording with a 32-bit fractional position, so any factor works. A knob-set factor is an easy later option.

## How it plays

### Upsampling, not a slower clock

The obvious way to play something 10× slower is to run the DAC 10× slower: 25 kHz instead of 250. I rejected that early, for two reasons:

- **The images would land in the audio band.** A DAC holding each sample for a long period makes copies of the signal around multiples of its sample rate. At 25 kHz, a 4.5 kHz tone has an image at 25 − 4.5 = **20.5 kHz**, and others below it as the tone rises. The amp's gentle RC filter only partly removes them.
- **The DAC shares its clock.** TIM2 triggers both the ADC and the DAC ([part 1](01-adc-and-dac.md)). Slowing it would stop the live input too, and the detector and waterfall are supposed to keep running during playback.

So the DAC keeps running at 250 kHz, and playback **upsamples**. Each output sample steps 1/10 of a sample forward through the recording, and its value is linearly interpolated between the two recorded samples either side:

```
recorded:   x[0]                          x[1]                          x[2]
output:     •    •    •    •    •    •    •    •    •    •    •    •    •  ...
            ←── 10 output samples per recorded sample, on a straight line ──→
```

```c
const float weight = (float)fraction * (1.0f / 4294967296.0f);   // 0..1
const float value  = current + ((next - current) * weight);

fraction += PLAYBACK_STEP;               // 2³² / 10
if(fraction < PLAYBACK_STEP)             // wrapped: on to the next recorded sample
{
    index++;
    current = next;
    next = Playback_Fetch(index + 1U);
}
```

The position is a 32-bit fraction, and it wraps round exactly when it passes a whole sample, the same trick as the heterodyne's phase accumulator. Upsampled this way, the images land around **multiples of 25 kHz** (25 ± 4.5 kHz, 50 ± 4.5 kHz…). That's ultrasonic, further reduced by the interpolation itself, and filtered by the amp's input RC.

It's also why 10× is about the limit for linear interpolation. At 20×, the first image moves down to 12.5 kHz minus the tone, which lands near 10 kHz: audible, and it would need a proper anti-imaging filter.

### The same high-pass as the heterodyne

The recorded samples are raw ADC codes, including the op-amp's mid-rail bias and the room's audible noise. Slowed 10×, everything audible below 10 kHz would become rumble below 1 kHz. So the samples first go through the same **10 kHz high-pass** as the heterodyne path, which removes both the bias and the audible band.

### Two block slots

Playback reads the ring one 512-byte block at a time, into **two slots**:

- One slot plays. A block of 256 recorded samples lasts 10.24 ms at 10×.
- The other loads the next block, which takes about 0.25 ms.

That's a 40× margin. The reads are queued from the audio interrupt itself, through the ring's read queue ([part 7](07-psram-ring-buffer.md)). If a block somehow isn't there in time, it plays as silence and is counted in `playbackUnderruns`.

### Pausing the ring

There's a trap here. Playing 2 seconds at 10× takes 20 seconds, but the ring only holds 16.8 seconds. If it kept recording during playback, the writer would come round the ring and **overwrite the recording while it was being played**.

So the ring **pauses** during playback. Blocks that arrive in the meantime aren't recorded, and recording resumes when playback ends. Two details make this safe:

- `Playback_Start` pauses the ring first, *then* reads how far it has written, so the last block can't change underneath it.
- The ring's stuck-burst check counts time in blocks even while paused, so a read that gets stuck during playback is still recovered.

The alternative, keeping the ring recording, only works if the segment × the factor fits inside the ring with room to spare: 1.6 seconds at 10×, 0.8 seconds at 20×. Pausing is simpler and allows longer segments.

### Live monitoring carries on

While playback runs, the heterodyne's output is replaced, and its mixer and filters are skipped to save time. Everything else keeps running on the **live** input: the bat detector, the USB debug capture and the waterfall. So the screen shows what's happening now while the speaker plays what happened then.

**Loudness.** The playback gain is half `DSP_GAIN`, scaled by the volume knob as usual. The heterodyne's mixer halves the amplitude (the ½ in the product identity), so half the gain makes playback as loud as the heterodyne for the same input.

**The button** is debounced over 30 ms. A press left over from the power-up SD speed test (which also uses PLAYBACK) has to be released before it counts, so holding the button through boot doesn't start a playback.

## It worked first time

Ultrasound made near the mic (jingling keys, fingertips rubbed together) played back slowed down and lower, sounding nothing like it does live. The countdown ran, the stop worked, and the LO readout came back afterwards. The numbers:

| Measurement | Result |
|---|---|
| `playbackUnderruns` | 0 |
| Ring faults, bursts over tCEM | 0, 0 |
| Worst-case DSP load during playback | 46% (unchanged) |
| `capture.py verify` afterwards | 160 KB identical: the ring records normally again |

It cost 1.1 KB of RAM (the two slots and state), leaving about 1.7 KB free, and 1.3 KB of flash. The CPU cost is *less* than the heterodyne's while playing, because the mixer and its filters are skipped, plus one block read per 10 ms.

Most of the work had already been done. Playback needed no new capture path, no new DMA and no new timing. It's a reader on a buffer that was already being written.

## What's next for playback

Version 1 is deliberately simple. Two improvements are parked:

- **Start at the last bat pass.** The detector ([part 9](09-bat-detector.md)) already knows when a pass started. Playing from there, rather than "the last 2 seconds", would play the bat rather than whatever came after it.
- **A knob-set factor,** for the occasional bat that's clearer at 20× (with a better interpolation filter) or 5×.

## What I learned

- **Choose the time-expansion factor from the whole output chain's passband** (mic, amp filter, speaker), not from a target pitch alone.
- **Upsample rather than slow the clock.** It keeps the images ultrasonic, and it keeps the shared sample clock (and live monitoring) running.
- **An always-on recording buffer makes features like this cheap.** Playback was a reader, not a recorder.
- **Count the failure you're designing against** (`playbackUnderruns`), even when the margin looks like 40×.

---

*Next: [Part 9, teaching it to hear bats](09-bat-detector.md): a detector, a USB data link, and a room full of ultrasound.*

*Previous: [Part 7, the ring buffer](07-psram-ring-buffer.md)*

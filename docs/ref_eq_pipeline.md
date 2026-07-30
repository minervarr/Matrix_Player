# Reference EQ Audio Pipeline

## Overview

Reference EQ is the non-bitperfect playback mode. It applies parametric EQ filters in
64-bit double precision on the decoded audio, then writes to the DAC at the highest
quality the output device supports. Bitperfect mode is unchanged — it aborts on any
format mismatch.

---

## Signal chain

### When source rate == device rate (no resampling needed)

```
FLAC file
    → dr_flac decode → int32 (left-justified, lossless)
    → EQ biquads in 64-bit double (in-place)
    → single quantize snap → int32
    → DAC at device's native rate and bit depth
```

### When source rate != device rate (resampling needed — e.g. Bluetooth)

```
FLAC file
    → dr_flac decode → int32 (left-justified, lossless)
    → EqManager::processToDouble() → double[] (EQ applied, no quantize yet)
    → soxr FLOAT64_I + SOXR_VHQ (28-bit precision, ~140 dB SNR)
    → double[] at device rate
    → TPDF dither + single quantize → int32 at device's max bit depth
    → DAC
```

The key design principle: **single quantization point**. All processing stays in 64-bit
double until the very last step. Quantizing before resampling would introduce a second
rounding error; here it happens exactly once.

---

## Device format negotiation

On `onPlay()`, the player negotiates the best format the device supports:

1. Try WASAPI exclusive at the file's native rate and 32-bit.
2. If that fails (non-bitperfect mode only):
   - WASAPI: call `WasapiOutput::probeRates()` — tests 8 standard rates
     `{44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000}` via
     `IsFormatSupported` (exclusive, no `Initialize` call).
   - USB Direct: use `usbDriver_.getOutputRates()` from UAC descriptor parsing.
   - `pickOutputRate()` selects the best match (prefers integer multiples of source rate).
   - Retry exclusive configure at the negotiated rate.
3. If exclusive still fails (e.g. Bluetooth, which never supports exclusive mode):
   fall back to WASAPI shared mode. The OS mix rate (typically 48 kHz) becomes
   `outSr`. The resample path handles the mismatch.
4. `WasapiOutput::getMaxBitDepth()` queries the device ceiling by probing
   Int32 → Int24In32 → Int16 in order. The first that passes becomes `capturedBits`.
   USB Direct uses `usbDriver_.getConfiguredBitDepth()`.

All of this is automatic — no user toggle needed. The same Ref EQ button works on a
hi-res USB DAC (exclusive, 32-bit, no resample) and on Bluetooth headphones
(shared, 16-bit, resampled).

---

## TPDF dither

When bit depth is reduced (e.g. 32-bit processing → 16-bit Bluetooth output),
Triangular Probability Density Function dither is applied before quantization.
TPDF adds ±1 LSB of shaped random noise that eliminates quantization distortion and
replaces it with low-level uncorrelated noise — inaudible in practice, and far
preferable to harmonic distortion artifacts from straight truncation.

At 32-bit output no dither is applied at all: the quantization error already sits
below any DAC's noise floor, so dither would only spend headroom. This is a
separate loop in `ae::TpdfQuantizer::process()`, not an amplitude of zero — the
old single-loop form still ran the full noise generator per sample and then
scaled it by zero, which measured as an identical per-sample cost at 16, 24 and
32 bits.

The generator's state lives in the `TpdfQuantizer` instance and must persist
across callbacks. A generator restarted per buffer repeats the same noise every
buffer, which is audible as a correlated tone rather than a flat noise floor.

---

## Key implementation locations

| What | Where |
|---|---|
| EQ biquad filters (double math) | `framework/audio_engine/core/include/core/dsp/eq_processor.h` — `processStereoCascade()`, `process32()` |
| EQ → double (no snap) | `eq_processor.h` — `processToDouble()` |
| EqManager resample-aware path | `core/src/eq_manager.cpp` — `processToDouble()` |
| soxr resampler setup + callback | `gui/src/player_view.cc` — Ref EQ branch in `onPlay()` |
| TPDF dither + quantize | `framework/audio_engine/core/include/core/dsp/dither.h` — `ae::TpdfQuantizer` |
| Exact inline rounding (`llround`/`lrint` replacements) | `core/dsp/round.h` — `ae::roundHalfAway`, `ae::roundHalfEven` |
| Bit-exactness gate for all of the above | `framework/audio_engine/tests/dsp_null_test.cpp` |
| WASAPI rate probing | `gui/src/wasapi_output.cc` — `probeRates()`, `getMaxBitDepth()` |
| Rate selection logic | `gui/src/player_view.cc` — `pickOutputRate()` (prefers integer multiples) |

---

## Bitperfect mode (unchanged)

Bitperfect mode decodes via the same lossless int32 path but skips EQ entirely and
writes verbatim to the DAC. If the device doesn't support the file's native rate or
bit depth, playback is aborted with an error message — no resampling, no fallback.

---

## Known limitations

- Gapless transitions that require a rate change (e.g. 44.1 kHz → 96 kHz album) still
  trigger a full `onPlay()` restart with an audible gap. Only same-rate transitions
  are seamless.
- The soxr resampler has a small startup latency (~20 ms at VHQ quality) on the first
  callback. This is absorbed by the ring buffer pre-fill before `output_->start()`.
- DSD is not yet supported in either mode.

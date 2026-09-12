# P4 OpenH264 decoder

Upstream: Cisco OpenH264 v2.6.0, commit `652bdb7719f30b52b08e506645a7322ff1b2cc6f`.
Only portable common + decoder C++ sources are compiled; no encoder, SIMD assembly or decoder threads. The caller submits complete Annex-B IDR access units with SPS/PPS and consumes cropped, strided I420 planes synchronously.

ESP-IDF adaptations:
- WelsThreadLib.cpp: ESP single-core decoder scheduling query; use ESP pthread defaults rather than unsupported POSIX scope / FIFO attributes.
- memory_align.cpp: aligned codec allocations use PSRAM; failed allocations log size and available memory.
- decoder_core.cpp: match two ExpandBs function definitions to their existing int32_t declarations (ESP ILP32 typedef uses long).
- Generated version string identifies the pinned source. Codec target uses -O3 and -fno-strict-aliasing; upstream logging format warnings remain warnings on ESP ILP32.

See upstream/LICENSE. A copy is included with distributed firmware as OpenH264-LICENSE.txt.

- decoder.cpp: two picture slots for the enforced IDR-only, error-concealment-disabled input contract. This port is not a general P/B frame decoder.
- Firmware disables full-image XiP copying to PSRAM, freeing the firmware image memory for decoding. Code/constants use the flash cache.

- The wrapper drains the display-order queue with FlushFrame after a complete IDR. High/Main pictures must not be left waiting for a future picture.

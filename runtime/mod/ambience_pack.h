#ifndef __AMBIENCE_PACK_H__
#define __AMBIENCE_PACK_H__

// The ambient sound file on Eltale Reborn discs (ambience.bin), written by tools/ambpack from
// eltale/ambience and read by runtime/mod/ambience.c. Little-endian, as both sides are.
//
// The file: the header, AMB_PACK_MAX clip records, AMB_PACK_STARTS start records for each clip, then
// the clips' data. Every clip is 22050 Hz AICA ADPCM (4 bits a sample, low nibble first). A clip's
// data starts on a disc sector and is cut into chunks of AMB_PACK_CHUNK bytes of each channel: stereo
// clips go left chunk, right chunk, left chunk... The last chunk is padded with zeros.
//
// The decoder starts every channel at the same state (0, step 127), but a loop's data was encoded on
// from wherever the encoder had got to, and played from anywhere else it goes wrong for good. So each
// loop has starts: a chunk where its data can take over, and a preamble of codes that takes the
// decoder from its key-on state to the encoder's state there. Preambles sound like nothing in
// particular and are played muted. One-shots play from their first sample, encoded from (0, 127).

#include <stdint.h>

#define AMB_PACK_MAGIC  0x32424D41u  // "AMB2"
#define AMB_PACK_RATE   22050
#define AMB_PACK_CHUNK  8192
#define AMB_PACK_ALIGN  2048
#define AMB_PACK_MAX    32
#define AMB_PACK_STARTS 8
#define AMB_PACK_PRE    512          // preamble codes at most

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t rate;
    uint32_t chunk;
    uint32_t data;      // where the clips' data begins: everything before it is the tables
    uint32_t reserved[3];
} AmbPackHeader;

typedef struct {
    uint32_t sample;    // a chunk start, where the clip's data takes over
    uint16_t codes;     // preamble length, a multiple of 8 and the same for both channels
    uint16_t pad;
    uint8_t pre[2][AMB_PACK_PRE / 2];
} AmbPackStart;

// Indoors the PC mod hears the clips through a 700 Hz low-pass, 1.4 times as loud, mixed in by the
// muffle amount m: out = (1 - m) x + m w, w = 1.4 lowpass(x). With wet = E[w^2] / E[x^2] and
// cross = E[x w] / E[x^2], that's sqrt((1 - m)^2 + m^2 wet + 2 (1 - m) m cross) times as loud.
typedef struct {
    uint32_t offset;    // of the clip's first chunk in the file
    uint32_t samples;   // of each channel, even
    uint32_t audible;   // samples before the silence that pads a one-shot (= samples for a loop)
    int32_t gain_mdb;   // channel gain in 1/1000 dB that plays the clip as loud as the PC mod at volume 1
    float wet, cross;
    uint8_t channels;   // 1 or 2
    uint8_t loops;
    uint8_t pad[2];
    char name[36];
} AmbPackClip;

#endif

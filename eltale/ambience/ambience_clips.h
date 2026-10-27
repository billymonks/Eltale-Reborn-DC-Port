#ifndef Q64_AMBIENCE_CLIPS_H
#define Q64_AMBIENCE_CLIPS_H

// The ambience clips (sfx/<file>.ogg), shared by the mod (src/ambience.c) and its native library
// (native/ambience.c), which embeds them. Gains are in dB and bring every clip to the same loudness: the
// loops to -42 dBFS RMS, the one-shots' loudest 400 ms to about -35. X(id, file, gain, loops)
#define AMBIENCE_CLIPS(X)                                   \
    X(AMB_FIELD,      "birds-with-wind-field",    13.9, 1)  \
    X(AMB_NIGHT,      "night-chirping",            4.0, 1)  \
    X(AMB_FOREST,     "forest-leaves-rustling",    8.9, 1)  \
    X(AMB_STREAM,     "forest-water",              5.6, 1)  \
    X(AMB_HARBOR,     "harbor",                    0.7, 1)  \
    X(AMB_WATER,      "peaceful-water",            5.8, 1)  \
    X(AMB_LAVA,       "lava-loop",               -18.0, 1)  \
    X(AMB_DUNGEON,    "dark-dungeon",             -6.9, 1)  \
    X(AMB_MAGIC_CAVE, "dark-magic-cave-ambiance", -7.4, 1)  \
    X(AMB_WINDY_CAVE, "windy-cave",               -0.7, 1)  \
    X(AMB_MAMMON,     "world-of-mannon",          -9.0, 1)  \
    X(AMB_OWL,        "owl-coos",                -10.5, 0)  \
    X(AMB_ROOSTER,    "rooster",                 -11.0, 0)

#define AMBIENCE_ENUM(id_, file_, gain_, loops_) id_,
enum { AMBIENCE_CLIPS(AMBIENCE_ENUM) AMB_COUNT };
#undef AMBIENCE_ENUM

#endif

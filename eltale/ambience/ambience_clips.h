#ifndef Q64_AMBIENCE_CLIPS_H
#define Q64_AMBIENCE_CLIPS_H

// The ambience clips (sfx/<file>.ogg), shared by the mod (src/ambience.c) and its native library
// (native/ambience.c), which embeds them. Gains are in dB and bring every loop to -29 LUFS integrated
// loudness (ITU-R BS.1770) as played, a mono file counting on both speakers. forest-leaves-rustling is
// capped 1.7 LU short by its peak. The one-shots sit a few LU above the beds they call over.
// X(id, file, gain, loops)
#define AMBIENCE_CLIPS(X)                                   \
    X(AMB_FIELD,      "birds-with-wind-field",    13.9, 1)  \
    X(AMB_NIGHT,      "night-chirping",           10.9, 1)  \
    X(AMB_FOREST,     "forest-leaves-rustling",   16.8, 1)  \
    X(AMB_STREAM,     "forest-water",             13.9, 1)  \
    X(AMB_HARBOR,     "harbor",                   10.4, 1)  \
    X(AMB_WATER,      "peaceful-water",           14.0, 1)  \
    X(AMB_LAVA,       "lava-loop",                -1.3, 1)  \
    X(AMB_DUNGEON,    "dark-dungeon",              6.3, 1)  \
    X(AMB_MAGIC_CAVE, "dark-magic-cave-ambiance",  3.5, 1)  \
    X(AMB_WINDY_CAVE, "windy-cave",                9.2, 1)  \
    X(AMB_MAMMON,     "world-of-mannon",           2.7, 1)  \
    X(AMB_OWL,        "owl-coos",                 -4.5, 0)  \
    X(AMB_ROOSTER,    "rooster",                 -10.5, 0)

#define AMBIENCE_ENUM(id_, file_, gain_, loops_) id_,
enum { AMBIENCE_CLIPS(AMBIENCE_ENUM) AMB_COUNT };
#undef AMBIENCE_ENUM

#endif

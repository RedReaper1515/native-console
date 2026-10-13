/*
 * Indirect textures: a texture lookup whose texel is not a colour.
 *
 * Up to four indirect stages sample a map and read the result as a vector
 * of offsets, which perturbs the texture coordinate of a later ordinary
 * TEV stage. The wiring runs the opposite way from the name: a TEV stage
 * names an indirect stage, in its own command register 0x10 + stage, and
 * any number of TEV stages may name the same one.
 *
 * What the offset is *for* is bump mapping, warping and tiling - a small
 * map that says, per texel, where to read a larger one. docs/gc-fog-
 * indirect.md part B has the registers, the enumerations and - read it
 * before changing anything here - the list of what could not be
 * established. Several details in this path are the best available
 * reading rather than measured fact, and each is marked where it is
 * implemented.
 */
#ifndef GC_GX_INDIRECT_H
#define GC_GX_INDIRECT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

/* The coordinate a TEV stage should sample with.
 *
 * `tev_stage` is the ordinary stage doing the sampling and `map` the
 * texture it is about to read, which is needed because the offset arrives
 * in texels of that map and the coordinate is normalised. `generated` is
 * the whole set of texgen outputs, because the indirect lookup uses one of
 * them and it is rarely the same one; they are as the transform left them,
 * and each is scaled to the indirect map through its own size registers.
 * `in` is the stage's own coordinate, already normalised to `map`.
 *
 * Writes `in` straight through when the stage's matrix selector is off,
 * which is what GX_Init leaves on every stage, so the ordinary path costs
 * one register read. */
void gc_gx_indirect_coordinate(GcMachine *m, unsigned tev_stage, unsigned map,
                               const float generated[8][2], const float in[2], float out[2]);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_INDIRECT_H */

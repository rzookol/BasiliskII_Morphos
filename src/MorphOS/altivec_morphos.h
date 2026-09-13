#ifndef BASILISK_ALTIVEC_MORPHOS_H
#define BASILISK_ALTIVEC_MORPHOS_H

#include "sysdeps.h"

#ifdef __cplusplus
extern "C" {
#endif

int BasiliskMorphOSHostHasAltiVec(void);
int BasiliskMorphOSAltiVecCompiled(void);
int BasiliskMorphOSAltiVecSelfTest(void);
uint32 BasiliskMorphOSAltiVecRGB555ToRGB16PC(const uint8 *src, uint8 *dst, uint32 pixels);
uint32 BasiliskMorphOSAltiVecRGB565ToRGB16PC(const uint8 *src, uint8 *dst, uint32 pixels);

#ifdef __cplusplus
}
#endif

#endif

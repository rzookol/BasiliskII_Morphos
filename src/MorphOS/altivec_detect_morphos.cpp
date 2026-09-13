#include <exec/system.h>
#include <proto/exec.h>
#include "altivec_morphos.h"

int BasiliskMorphOSHostHasAltiVec(void)
{
#ifdef SYSTEMINFOTYPE_PPC_ALTIVEC
    ULONG available = 0;
    if (NewGetSystemAttrsA(&available, sizeof(available), SYSTEMINFOTYPE_PPC_ALTIVEC, NULL) != 0)
        return available != 0;
#endif
    return 0;
}

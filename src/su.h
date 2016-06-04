#ifndef __MB_SU_H__
#define __MB_SU_H__

#include <unistd.h>
#include <pwd.h>


int
mb_su_canroot(void);


int
mb_su_gainroot(void);


void
mb_su_droproot(void);

#endif

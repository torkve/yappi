
#ifndef YTIMING_H
#define YTIMING_H

#include "Python.h"

#if !defined(HAVE_LONG_LONG)
#error "yapp� requires long longs!"
#endif

long long
tickcount(void);


double
tickfactor(void);

#endif



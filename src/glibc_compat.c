/*
 * glibc_compat.c - Compatibility wrappers for glibc math functions
 *
 * PURPOSE:
 *   Provides __wrap_fmod and __wrap_fmodf implementations to ensure
 *   compatibility across different glibc versions. Older glibc versions
 *   may lack these functions or have incompatible implementations.
 *
 * USAGE:
 *   Linked via --wrap=fmod --wrap=fmodf linker flags. This intercepts
 *   calls to fmod/fmodf and redirects them to these implementations.
 *
 * WHEN TO REMOVE:
 *   If the minimum supported glibc version is raised to 2.27+ (Ubuntu 18.04+),
 *   these wrappers may no longer be needed. Verify by testing on the oldest
 *   target system before removal.
 *
 * LAST REVIEWED: 2026-07-27
 */

#include <math.h>

float __wrap_fmodf(float x, float y)
{
    return x - truncf(x / y) * y;
}

double __wrap_fmod(double x, double y)
{
    return x - trunc(x / y) * y;
}

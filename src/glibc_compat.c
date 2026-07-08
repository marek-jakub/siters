#include <math.h>

float __wrap_fmodf(float x, float y)
{
    return x - truncf(x / y) * y;
}

double __wrap_fmod(double x, double y)
{
    return x - trunc(x / y) * y;
}

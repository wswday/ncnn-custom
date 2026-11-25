// Math stub functions for uclibc compatibility
#include <math.h>

static int rounding_mode = 0; // FE_TONEAREST

void fesetround(int mode) {
    rounding_mode = mode;
}

int fegetround(void) {
    return rounding_mode;
}

float nearbyintf(float x) {
    return roundf(x);
}

// Test: #include resolution
// Expected: types from common.h are resolved correctly

#include "common.h"

int main() {
    struct Point p;
    p.x = 10;
    p.y = 20;

    struct Color c;
    c.r = 255;
    c.g = 128;
    c.b = 0;

    int sum = p.x + c.r;
    return sum;
}

// Test: Dynamic memory allocation
// Expected IR: HeapObjVar, external call handling

#include <stdlib.h>

int main() {
    int *p = (int*)malloc(sizeof(int));
    *p = 42;
    int x = *p;
    free(p);
    return x;
}

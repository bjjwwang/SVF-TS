// Test: Pointer operations
// Expected IR: Addr, Store, Load edges

int main() {
    int x = 10;
    int *p = &x;
    *p = 5;
    int y = *p;
    return y;
}

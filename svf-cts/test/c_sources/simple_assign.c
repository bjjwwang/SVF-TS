// Test: Simple assignment and copy
// Expected IR: StackObj, Addr, Store, Load, Copy

int main() {
    int x = 1;
    int y = x;
    return y;
}

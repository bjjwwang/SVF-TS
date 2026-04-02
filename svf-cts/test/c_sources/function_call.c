// Test: Function call with pointer parameter
// Expected IR: FunObjVar, CallPE, RetPE

void foo(int *p) {
    *p = 1;
}

int main() {
    int x = 0;
    foo(&x);
    return x;
}

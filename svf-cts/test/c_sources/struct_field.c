// Test: Struct field access
// Expected IR: GepStmt with field offset

struct S {
    int x;
    int y;
};

int main() {
    struct S s;
    s.x = 1;
    s.y = 2;
    int z = s.x;
    return z;
}

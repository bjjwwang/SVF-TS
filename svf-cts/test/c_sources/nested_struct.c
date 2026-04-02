// Test: Nested struct with various field types
// Expected: struct field types are correct (not all int)

struct Inner {
    int a;
    float b;
};

struct Outer {
    struct Inner inner;
    int count;
    char flag;
};

int main() {
    struct Outer o;
    o.inner.a = 10;
    o.inner.b = 3.14;
    o.count = 5;
    o.flag = 'x';
    int v = o.inner.a;
    return v;
}

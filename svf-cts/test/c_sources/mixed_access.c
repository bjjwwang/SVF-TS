// Test: Mixed struct/array access
// Expected IR: Chained GepStmts

struct Point {
    int x;
    int y;
};

int main() {
    struct Point pts[3];
    pts[0].x = 1;
    pts[0].y = 2;
    int v = pts[0].x;
    return v;
}

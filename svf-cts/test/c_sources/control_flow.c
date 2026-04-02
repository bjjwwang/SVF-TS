// Test: Control flow (if/else)
// Expected IR: PhiStmt at join, BranchStmt

int main() {
    int c = 1;
    int x;
    if (c) {
        x = 1;
    } else {
        x = 2;
    }
    int y = x;
    return y;
}

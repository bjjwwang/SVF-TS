// Test: Array index access
// Expected IR: GepStmt with array offset

int main() {
    int arr[5];
    arr[0] = 10;
    arr[2] = 20;
    int x = arr[0];
    return x;
}

#include <coco.h>

int add(int a, int b)
{
    int c;
    c = a + b;
    return c;
}

int main()
{
    int x = 5;
    int y = 10;
    int z;

    z = add(x, y);
    printf("z=%d\n", z);
    return 0;
}

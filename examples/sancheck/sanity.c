volatile int r;
volatile unsigned char mem8[16];
volatile unsigned short mem16[16];
volatile unsigned mem32[16];

void _start(void)
{
    volatile int a = 100000;
    volatile int b = 1;
    r = a - b;             /* 99999 -> 17 effective bits */

    volatile int c = 5;
    volatile int d = 3;
    r = c + d;              /* 8 -> 4 bits */

    volatile int e = 255;
    volatile int f = 1;
    r = e & f;               /* 1 -> 1 bit */

    volatile int g = 0;
    volatile int h = 0;
    r = g - h;                /* 0 -> 0 bits */
    if (g == h) { r = r + 1; } /* CMP 0,0 -> result 0 -> 0 bits */

    volatile int big = 70000;
    if (big > 1) { r = r + 1; } /* CMP big,1 -> 69999 -> 17 bits */

    mem8[0] = (unsigned char)a;   /* 100000 & 0xFF = 0xA0 = 160 -> 8 bits */
    mem16[0] = (unsigned short)a; /* 100000 & 0xFFFF = 34464 -> 16 bits */
    mem32[0] = (unsigned)a;        /* 100000 -> 17 bits */

    while (1) { }
}

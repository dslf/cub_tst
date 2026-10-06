/*
 * scramble.c -- WCA 3x3x3 scramble generator + ASCII cube renderer
 *
 * Pure C89. No platform headers, ASCII-only output (safe on cmd.exe /
 * conhost with any codepage).
 *
 *   gcc -std=c89 -Wall -Wextra -pedantic -O2 -o scramble scramble.c
 *   scramble.exe -n 5 -v
 *
 * WCA rules for a 3x3x3 scramble (20 moves):
 *   - every move is one of U D L R F B, with suffix none / ' / 2
 *   - no two consecutive moves on the same face        (R R)   forbidden
 *   - no three consecutive moves on the same two faces (R L R) forbidden
 *
 * Modules:
 *   1. rng      xorshift32 + rejection sampling (all loops provably bounded)
 *   2. gen      WCA scramble generator (no rejection loop at all)
 *   3. cube     54-sticker state, moves derived from 3D rotation math
 *   4. render   unfolded-net ASCII view
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXMOVES 200
#define MAXCOUNT 1000
#define NST      54          /* stickers: 6 faces * 9 */

/* face indices: opposite pairs are (0,1) (2,3) (4,5) */
static const char *FNAME[6] = { "U", "D", "L", "R", "F", "B" };
static const char  FCH[7]   = "UDLRFB";
static const int   OPP[6]   = { 1, 0, 3, 2, 5, 4 };
static const char *SUF[3]   = { "", "'", "2" };

/* ------------------------------------------------------------------ rng */

static unsigned long rng_state;

static void rng_seed(unsigned long s)
{
    rng_state = s ? s : 0x9E3779B9UL;
}

static unsigned long rng_next(void)
{
    unsigned long x = rng_state;
    x ^= (x << 13) & 0xFFFFFFFFUL;
    x ^= (x >> 17);
    x ^= (x << 5) & 0xFFFFFFFFUL;
    rng_state = x & 0xFFFFFFFFUL;
    return rng_state;
}

/* uniform in [0,n). Rejection, but with a hard iteration cap so this loop
 * can never hang: after 64 tries we fall back to plain modulo.            */
static unsigned long rng_below(unsigned long n)
{
    unsigned long r, limit;
    int tries;

    if (n < 1 || n > 0xFFFFFFFFUL)
        return 0;
    limit = (0xFFFFFFFFUL / n) * n;      /* largest multiple of n <= 2^32-1 */
    tries = 0;
    do {
        r = rng_next();
        if (++tries > 64)
            return r % n;                /* bounded fallback, never loops  */
    } while (r >= limit);
    return r % n;
}

/* ------------------------------------------------------------------- gen */

/*
 * No rejection loop: build the list of legal faces (always 4 or 5 of them)
 * and pick one uniformly. Guarantees termination in O(1).
 */
static void gen_scramble(int *f, int *s, int n)
{
    int i, t, face, prev, prev2, cand[6], nc;

    prev  = -1;
    prev2 = -1;
    for (i = 0; i < n; i++) {
        nc = 0;
        for (t = 0; t < 6; t++) {
            if (t == prev)
                continue;
            if (prev2 >= 0 && t == prev2 && OPP[prev] == t)
                continue;
            cand[nc++] = t;
        }
        face = cand[(int)rng_below((unsigned long)nc)];
        f[i] = face;
        s[i] = (int)rng_below(3);
        prev2 = prev;
        prev  = face;
    }
}

/* ------------------------------------------------------------------ cube */

/*
 * Coordinates: x right, y up, z front (right handed, x cross y = z).
 * A sticker is identified by its position vector (x,y,z), components in
 * {-1,0,1}, exactly one component equal to +-1 pointing out of the face.
 * A face turn rotates every sticker whose coordinate along the turn axis
 * equals the face sign -- that is exactly the 9 face stickers + the 12
 * side stickers of that layer (21 stickers).
 */

/* face -> axis, sign */
static const int AXIS[6] = { 1, 1, 0, 0, 2, 2 };
static const int SGN [6] = { 1,-1,-1, 1, 1,-1 };

/*
 * In-plane lookup: for a face, which axis drives the column and which the
 * row, as affine maps coord -> index (0..2).  Faces sharing an axis must
 * agree on the axes, they may differ in sign (that is what C1/R1 encode).
 *        U: x = c-1, z = r-1     D: x = c-1, z = 1-r
 *        L: z = c-1, y = 1-r    R: z = 1-c, y = 1-r
 *        F: x = c-1, y = 1-r    B: x = 1-c, y = 1-r
 */
static const int A0[6] = { 0, 0, 2, 2, 0, 0 };              /* col axis */
static const int A1[6] = { 2, 2, 1, 1, 1, 1 };              /* row axis */
static const int C1[6] = { 1, 1, 1,-1, 1,-1 };              /* col = C1*v + 1 */
static const int R1[6] = { 1,-1,-1,-1,-1,-1 };              /* row = R1*v + 1 */
static const int POFA[3][2] = { { 3, 2 }, { 0, 1 }, { 4, 5 } }; /* axis,sign -> face */

/* slot -> coords */
static int CX[NST], CY[NST], CZ[NST];

/* PERM[face][0=CW 1=CCW][slot] : where each sticker goes */
static int PERM[6][2][NST];

/* A sticker is (normal axis, normal sign, position); a turn rotates the
 * normal and the position, then the new face's grid gives the slot. */
static int slot_of(int na, int ns, int x, int y, int z)
{
    int v[3], f, r, c;

    v[0] = x; v[1] = y; v[2] = z;
    f = POFA[na][ns > 0 ? 0 : 1];
    r = R1[f] * v[A1[f]] + 1;
    c = C1[f] * v[A0[f]] + 1;
    return f * 9 + r * 3 + c;
}

static void init_cube(void)
{
    int fi, r, c, i, x, y, z;

    for (fi = 0; fi < 6; fi++) {
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                switch (fi) {           /* net: U above F, D below, L left,
                                         * R right, B right of R */
                case 0: x = c - 1; y =  1; z = r - 1; break;   /* U */
                case 1: x = c - 1; y = -1; z = 1 - r; break;   /* D */
                case 2: x = -1;    y = 1 - r; z = c - 1; break;/* L */
                case 3: x =  1;    y = 1 - r; z = 1 - c; break;/* R */
                case 4: x = c - 1; y = 1 - r; z =  1; break;   /* F */
                default: x = 1 - c; y = 1 - r; z = -1; break;  /* B */
                }
                i = fi * 9 + r * 3 + c;
                CX[i] = x; CY[i] = y; CZ[i] = z;
            }
        }
    }

    for (fi = 0; fi < 6; fi++) {
        int k = AXIS[fi], s = SGN[fi], p = (k + 1) % 3, q = (k + 2) % 3;
        int d, dir, na, ns;
        int n[3], m[3], v[3], w[3];

        for (d = 0; d < 2; d++) {
            /* CW seen from outside the face == rotation by -90 deg about
             * +axis when the face is on the + side, +90 deg when on -  */
            dir = (s > 0) ? (d == 0 ? 1 : -1) : (d == 0 ? -1 : 1);
            for (i = 0; i < NST; i++) {
                v[0] = CX[i]; v[1] = CY[i]; v[2] = CZ[i];
                if (v[k] == s) {
                    n[0] = n[1] = n[2] = 0;
                    n[AXIS[i / 9]] = SGN[i / 9];   /* sticker's own normal */
                    w[0] = v[0]; w[1] = v[1]; w[2] = v[2];
                    m[0] = n[0]; m[1] = n[1]; m[2] = n[2];
                    if (dir > 0) { w[p] =  v[q]; w[q] = -v[p];
                                   m[p] =  n[q]; m[q] = -n[p]; }
                    else         { w[p] = -v[q]; w[q] =  v[p];
                                   m[p] = -n[q]; m[q] =  n[p]; }
                    na = (m[0] != 0) ? 0 : ((m[1] != 0) ? 1 : 2);
                    ns = m[na];
                    PERM[fi][d][i] = slot_of(na, ns, w[0], w[1], w[2]);
                } else {
                    PERM[fi][d][i] = i;
                }
            }
        }
    }
}

static void do_move(int *st, int face, int ccw)
{
    static int tmp[NST];
    int i;
    const int *p = PERM[face][ccw ? 1 : 0];
    for (i = 0; i < NST; i++) tmp[p[i]] = st[i];
    for (i = 0; i < NST; i++) st[i] = tmp[i];
}

static int is_solved(const int *st)
{
    int i;
    for (i = 0; i < NST; i++)
        if (st[i] != i / 9) return 0;
    return 1;
}

/* --------------------------------------------------------------- render */

/*
 * ANSI background colours, one per face (WCA colour scheme).  \033 is used
 * instead of \e because \e is not standard C.  The escape sequences are
 * plain ASCII bytes, so they are safe on any Windows console that knows
 * ANSI (ConEmu, Windows Terminal, Git Bash, ...); use -a for the plain
 * letter net on older ones.
 */
static const char *CLR[6] = {
    "\033[48;05;15m",    /* U white  */
    "\033[48;05;226m",   /* D yellow */
    "\033[48;05;214m",   /* L orange */
    "\033[48;05;196m",   /* R red    */
    "\033[48;05;46m",    /* F green  */
    "\033[48;05;21m"     /* B blue   */
};
static const char *RST = "\033[0m";
static int use_color = 0;

static void legend(void)
{
    int f;
    if (!use_color) return;
    for (f = 0; f < 6; f++) {
        fputs(CLR[f], stdout);
        printf("  ");
        fputs(RST, stdout);
        printf(" %s", FNAME[f]);
    }
    putchar('\n');
}

/*
 * No grid characters: colour mode paints each sticker as a solid block of
 * 2 spaces, plain mode prints one letter per sticker.  A face is 3 cells
 * wide, faces are separated by one blank.  cw = chars per sticker.
 */
static void row(int indent, const int *st, const int *fc, int nfc, int r, int cw)
{
    int i, k, j;
    for (i = 0; i < indent; i++) putchar(' ');
    for (i = 0; i < nfc; i++) {
        if (i) putchar(' ');                       /* gap between faces */
        for (k = 0; k < 3; k++) {
            int col = st[fc[i] * 9 + r * 3 + k];
            if (use_color) {
                fputs(CLR[col], stdout);
                for (j = 0; j < cw; j++) putchar(' ');
                fputs(RST, stdout);
            } else {
                putchar(FCH[col]);
            }
        }
    }
    putchar('\n');
}

static void render(const int *st)
{
    static const int MID[4] = { 2, 4, 3, 5 };   /* L F R B */
    static int ONE[1];                          /* reused for U and D */
    int r, cw, indent;

    cw = use_color ? 2 : 1;
    indent = 3 * cw + 1;                        /* width of L face + gap */

    ONE[0] = 0;                                 /* U */
    for (r = 0; r < 3; r++) row(indent, st, ONE, 1, r, cw);
    putchar('\n');
    for (r = 0; r < 3; r++) row(0, st, MID, 4, r, cw);
    putchar('\n');
    ONE[0] = 1;                                 /* D */
    for (r = 0; r < 3; r++) row(indent, st, ONE, 1, r, cw);
}

/* ------------------------------------------------------------- selftest */

static int selftest(void)
{
    int st[NST], i, f, d, n, bad = 0;
    static const int sx_f[] = { 3, 0, 3, 0 };     /* R U R' U' */
    static const int sx_c[] = { 0, 0, 1, 1 };

    for (i = 0; i < NST; i++) st[i] = i / 9;

    /* 1: table round-trip -- coords -> slot -> coords must be stable */
    for (i = 0; i < NST; i++)
        if (slot_of(AXIS[i / 9], SGN[i / 9], CX[i], CY[i], CZ[i]) != i) {
            printf("selftest: bad coord tables\n"); return 1;
        }

    /* 1b: every move table is a bijection moving exactly 21 stickers */
    for (f = 0; f < 6; f++) {
        for (d = 0; d < 2; d++) {
            int seen[NST], moved = 0, ok = 1;
            for (i = 0; i < NST; i++) seen[i] = 0;
            for (i = 0; i < NST; i++) {
                int j = PERM[f][d][i];
                if (seen[j]) ok = 0;
                seen[j] = 1;
                if (j != i) moved++;
            }
            for (i = 0; i < NST; i++) if (!seen[i]) ok = 0;
            if (!ok || moved != 20) {   /* 21 in the layer, the centre stays */
                printf("selftest: bad perm %s\n", FNAME[f]); return 1;
            }
        }
    }

    /* 2: every face turn has order 4, CW then CCW is identity */
    for (f = 0; f < 6; f++) {
        for (i = 0; i < NST; i++) st[i] = i / 9;
        for (d = 0; d < 4; d++) do_move(st, f, 0);
        if (!is_solved(st)) { printf("selftest: %s4 != 1\n", FNAME[f]); bad = 1; }
        for (i = 0; i < NST; i++) st[i] = i / 9;
        do_move(st, f, 0); do_move(st, f, 1);
        if (!is_solved(st)) { printf("selftest: %s%s != 1\n", FNAME[f], FNAME[f]); bad = 1; }
    }

    /* 3: U sends the R top row to the F top row (notation check) */
    for (i = 0; i < NST; i++) st[i] = i / 9;
    do_move(st, 0, 0);
    if (st[4 * 9 + 0] != 3) { printf("selftest: U direction wrong\n"); bad = 1; }

    /* 4: sexy move has order 6 */
    for (i = 0; i < NST; i++) st[i] = i / 9;
    for (n = 0; n < 6; n++)
        for (d = 0; d < 4; d++) do_move(st, sx_f[d], sx_c[d]);
    if (!is_solved(st)) { printf("selftest: (R U R' U')^6 != 1\n"); bad = 1; }

    /* 5: R U has order 105 (proves the two layers really do rotate) */
    for (i = 0; i < NST; i++) st[i] = i / 9;
    for (n = 0; n < 105; n++) { do_move(st, 3, 0); do_move(st, 0, 0); }
    if (!is_solved(st)) { printf("selftest: (R U)^105 != 1\n"); bad = 1; }
    for (i = 0; i < NST; i++) st[i] = i / 9;
    for (n = 0; n < 104; n++) { do_move(st, 3, 0); do_move(st, 0, 0); }
    if (is_solved(st)) { printf("selftest: (R U)^104 == 1 (wrong)\n"); bad = 1; }

    /* 3b: D (clockwise from below) sends the L bottom row to the F bottom row */
    for (i = 0; i < NST; i++) st[i] = i / 9;
    do_move(st, 1, 0);
    if (st[4 * 9 + 6] != 2) { printf("selftest: D direction wrong\n"); bad = 1; }

    /* 6: a scramble followed by its inverse is the identity */
    {
        int mv[20], sf[20];
        rng_seed(987654321UL);
        gen_scramble(mv, sf, 20);
        for (i = 0; i < NST; i++) st[i] = i / 9;
        for (i = 0; i < 20; i++) {
            do_move(st, mv[i], sf[i] == 1);
            if (sf[i] == 2) do_move(st, mv[i], 0);
        }
        for (i = 19; i >= 0; i--) {
            if (sf[i] == 2) { do_move(st, mv[i], 0); do_move(st, mv[i], 0); }
            else do_move(st, mv[i], sf[i] == 0 ? 1 : 0);
        }
        if (!is_solved(st)) { printf("selftest: scramble + inverse != 1\n"); bad = 1; }
    }

    return bad;
}

/* ----------------------------------------------------------------- main */

static void usage(const char *prog)
{
    printf("usage: %s [-n count] [-l length] [-s seed] [-v] [-a] [-c] [-t] [-h]\n", prog);
    printf("  -n count   number of scrambles   (1..%d,   default 1)\n", MAXCOUNT);
    printf("  -l length  moves per scramble    (1..%d,  default 20)\n", MAXMOVES);
    printf("  -s seed    PRNG seed (0..4294967295, default: from clock)\n");
    printf("  -v         draw the cube (unfolded net) in ANSI colours after each scramble\n");
    printf("  -a         same net, but plain letters (no ANSI, for old consoles)\n");
    printf("  -c         print the 54-sticker string after each scramble\n");
    printf("  -t         run the cube-math self test and exit\n");
    printf("  -h         this help\n");
    printf("example: %s -n 5 -v\n", prog);
}

int main(int argc, char *argv[])
{
    int  count = 1, len = 20, i, k, j;
    int  have_seed = 0, show = 0, showstr = 0, test = 0;
    unsigned long seed = 0;
    int  f[MAXMOVES], s[MAXMOVES];
    int  st[NST];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            len = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seed = strtoul(argv[++i], NULL, 10);
            have_seed = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            show = 1;
            use_color = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            show = 1;
            use_color = 0;
        } else if (strcmp(argv[i], "-c") == 0) {
            showstr = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            test = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (count < 1)        count = 1;
    if (count > MAXCOUNT) count = MAXCOUNT;
    if (len < 1)          len = 1;
    if (len > MAXMOVES)   len = MAXMOVES;

    init_cube();

    if (test) {
        if (selftest()) { printf("FAILED\n"); return 1; }
        printf("cube math OK (all tests passed)\n");
        for (i = 0; i < NST; i++) st[i] = i / 9;
        printf("\nsolved cube:\n");
        legend();
        render(st);
        return 0;
    }

    if (!have_seed) {
        seed = (unsigned long)time(NULL);
        seed ^= (unsigned long)clock() * 0x01000193UL;
        seed ^= (unsigned long)argc * 0x85EBCA6BUL;
        for (i = 0; argv[0][i] != '\0'; i++)
            seed = seed * 33UL + (unsigned long)(unsigned char)argv[0][i];
    }
    rng_seed(seed);

    printf("WCA 3x3x3 scramble  --  seed %lu  (rerun with -s %lu)\n", seed, seed);

    if (show) legend();

    for (k = 0; k < count; k++) {
        gen_scramble(f, s, len);
        if (count > 1) printf("%2d:", k + 1);
        for (j = 0; j < len; j++)
            printf(" %s%s", FNAME[f[j]], SUF[s[j]]);
        printf("\n");

        if (show || showstr) {
            for (i = 0; i < NST; i++) st[i] = i / 9;
            for (j = 0; j < len; j++) {
                do_move(st, f[j], s[j] == 1);
                if (s[j] == 2) do_move(st, f[j], 0);
            }
            if (showstr) {
                for (i = 0; i < NST; i++) putchar(FCH[st[i]]);
                printf("\n");
            }
            if (show) {
                printf("\n");
                render(st);
                if (k + 1 < count) printf("\n");
            }
        }
    }

    return 0;
}

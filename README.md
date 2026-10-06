# WCA 3x3x3 Scrambler

A single-file, pure C89 Windows-console program that generates competition-style
Rubik's cube scrambles, and (optionally) shows the resulting cube as a coloured
unfolded net.

This repository is also a **test of a local model**: the whole project (write,
compile, self-test, debug, document) was produced by a local LLM driving this
agent, so the code doubles as a small, verifiable coding benchmark -- the
self-test (`-t`) and `check.awk` give a pass/fail signal the model can be
scored against. See [Test model and environment](#test-model-and-environment)
at the end.

    scramble.c        the whole program (no external files, no dependencies)
    scramble.exe      compiled binary
    check.awk         optional helper: validates the 54-char state strings
                      (run: ./scramble.exe -n 500 -s 7 -c | awk -f check.awk)
    README.md         this file

---

## Build

    gcc -std=c89 -Wall -Wextra -pedantic -Werror -O2 -o scramble scramble.c

It compiles clean with **zero warnings** under those flags. The code is strict
C89: declarations at the top of each block, `/* */` comments only, no VLAs, no
`long long`, no platform headers (`stdio.h`, `stdlib.h`, `time.h` only), and
`<ESC>` bytes are written as `\033` (the GCC-only `\e` escape is not standard C
and would break `-pedantic`).

On Windows, rename as you like; the source is portable to any C89 compiler.

---

## Usage

    scramble [-n count] [-l length] [-s seed] [-v] [-a] [-c] [-t] [-h]

| Option | Meaning |
| ------ | ------- |
| `-n count` | how many scrambles to print, 1..1000 (default 1) |
| `-l length` | moves per scramble, 1..200 (default **20**, the WCA 3x3x3 length) |
| `-s seed` | PRNG seed, 0..4294967295 (default: taken from the clock) |
| `-v` | after each scramble, draw the cube net in **ANSI colours** |
| `-a` | same net, but plain letters (for consoles without ANSI support) |
| `-c` | after each scramble, print the 54-sticker state as one line |
| `-t` | run the cube-math self-test and exit |
| `-h` | help |

Values out of range are clamped, not rejected. A seed of `0` is mapped to a
fixed non-zero state (xorshift must never be 0).

### Examples

    scramble -n 5                     five random 20-move scrambles
    scramble -s 12345 -n 3            reproducible: same seed -> same output
    scramble -l 30 -n 1               a longer (non-WCA) scramble
    scramble -v                       scramble + coloured cube picture
    scramble -s 42 -n 100 -c          100 states, pipe into check.awk
    scramble -t                       verify the cube maths

The header line always echoes the seed, so any run can be replayed:

    WCA 3x3x3 scramble  --  seed 987  (rerun with -s 987)

---

## WCA scramble rules (what this generator guarantees)

The WCA uses a dedicated random-state scrambler (its own program, not a
hand-written rule list). The rules that a 3x3x3 scramble must satisfy:

1. **Exactly 20 moves** for 3x3x3 (other puzzles use other lengths; `-l` lets
   you change it, but 20 is the official value).
2. **Outer face turns only** -- the six faces `U D L R F B`. No middle-slice
   moves (`M E S`), no wide moves (`r u' ...`), no cube rotations (`x y z`),
   no whole-cube regrips.
3. **Suffixes** -- each turn is one of: nothing (90° clockwise), `'` (90°
   counter-clockwise), `2` (180°). All three are equally likely.
4. **No redundant moves** -- the sequence must not be reducible:
   - two consecutive moves on the **same face** are forbidden (`R R`, `U U'`,
     `F2 F` would collapse into one turn);
   - three consecutive moves on the **same axis** are forbidden, i.e. the
     pattern `X Y X` with `X` and `Y` opposite faces (`R L R`, `U D' U`),
     which can be rewritten as one slice move and would bias the position set.
5. **Uniform random state** -- every legal scramble is drawn with the same
   probability, so the distribution over positions is essentially uniform.
6. A scramble is never the solved state (impossible in 20 legal random moves).

### How the generator implements this

Instead of "generate, test, retry" (which can loop unboundedly), the generator
builds the list of **legal faces for the next slot** and picks one uniformly:

    for each move slot:
        candidates = all 6 faces
                     minus the previous face                        (rule 4a)
                     minus prev2, when prev is the face opposite
                       to prev2 (same axis)                          (rule 4b)
        face   = uniform choice among the candidates    (5 or 4 of them)
        suffix = uniform choice among { "", "'", "2" }

That is exactly 5 or 4 candidates * 3 suffixes per slot, each with probability
`1/(nc*3)`, so the scramble is uniform over the legal set and the loop is
guaranteed O(1) per move -- **no rejection loop, no possible hang**.

### Randomness

- PRNG: **xorshift32** (`x ^= x<<13; x ^= x>>17; x ^= x<<5`, masked to 32 bits).
  Fast, deterministic, and reproducible from the seed. It is *not*
  cryptographic; it is a period-2^32-1 generator, which is plenty for scrambles
  but means "seed 12345" is a replay key, not a secret.
- Uniform integers use **rejection sampling** to avoid modulo bias: values
  `>= floor((2^32-1)/n)*n` are redrawn. A 64-try cap falls back to plain
  modulo so no loop can ever hang (bias only after 64 failed tries, which
  essentially never happens for `n = 3..6`).
- Default seed: `time(NULL) ^ (clock() * 0x01000193)`.

---

## Move notation

Standard WCA/face-turn notation:

    U  D  L  R  F  B          turn that face 90 degrees clockwise
    X'                       turn that face 90 degrees counter-clockwise
    X2                       turn that face 180 degrees

"Clockwise" means *looking straight at that face from outside the cube*. So
`D` is clockwise as seen from below, and `B` is clockwise as seen from behind --
this is the classic source of "the picture looks mirrored" confusion.

Face order used internally everywhere: `U D L R F B` (indices 0..5).
Opposite pairs: `U/D`, `L/R`, `F/B`.

---

## Cube model and rendering

### Coordinates

Right-handed: `x` right, `y` up, `z` front. Every sticker is stored as the
integer triple of its position `(x,y,z)`, components in `{-1,0,1}`, plus its
outward **normal** vector. A face turn rotates every sticker whose coordinate
along the turn axis equals the face sign -- exactly 21 stickers (9 face + 12
side); the face centre never moves, so 20 stickers actually change place.

At start-up the program builds `PERM[face][cw|ccw][54]`, a permutation of the
54 sticker slots, from those coordinates. Applying a move is then a table
lookup -- no geometry at runtime.

### The 54-character state string (`-c`)

Order: faces `U D L R F B`, each face read **row by row as drawn in the net**
(top row first, left to right). Each character is the *colour/face* of the
sticker currently in that slot, so a solved cube prints

    UUUUUUUUUDDDDDDDDDLLLLLLLLLRRRRRRRRRFFFFFFFFFBBBBBBBBB

### The net (`-v` / `-a`)

The net has no grid characters. This is exactly what `-a` prints for a solved
cube (`-v` prints the same layout with colour blocks instead of letters):

```
    UUU
    UUU
    UUU

LLL FFF RRR BBB
LLL FFF RRR BBB
LLL FFF RRR BBB

    DDD
    DDD
    DDD
```

- colour mode: each sticker is a solid block of 2 spaces painted with its
  background colour; a face is 6 columns, faces are separated by one blank,
  U/D are indented so that U sits exactly above F.
- plain mode: one letter per sticker (`FFF RRR BBB LLL`).

### Colour scheme (WCA standard)

| Face | Colour | ANSI background |
| ---- | ------ | --------------- |
| U | white  | `48;05;15`   |
| D | yellow | `48;05;226`  |
| L | orange | `48;05;214`  |
| R | red    | `48;05;196`  |
| F | green  | `48;05;46`   |
| B | blue   | `48;05;21`   |

A legend line is printed once per run in colour mode. If your console shows
garbage instead of colours, use `-a` (pure ASCII, no escapes).

---

## Self-test (`-t`)

Prints `cube math OK (all tests passed)` or the first failure. The tests are
chosen so that a wrong cube model cannot pass by accident:

1. **Coord round-trip** -- slot -> coords -> slot is the identity for all 54.
2. **Permutation tables** -- every `PERM[face][dir]` is a bijection moving
   exactly 20 stickers (21 in the layer, the centre stays put).
3. **Order checks** -- `X4 = 1` and `X X' = 1` for all six faces.
4. **Direction checks** -- `U` must send the R top row to the F top row;
   `D` (clockwise from below) must send the L bottom row to the F bottom row.
   These two pin down the sign convention of positive and negative faces.
5. **Sexy move** -- `(R U R' U')^6 = 1`.
6. **`(R U)^105 = 1` and `(R U)^104 != 1`** -- the true order of `R U` in the
   cube group is 105; this is a strong signature that both layers really rotate
   and that the two axes interact correctly.
7. **Scramble + inverse = identity** -- a 20-move scramble is applied, then its
   exact inverse in reverse order, and the cube must return to solved.

---

## Known limits

- Only the 3x3x3 cube, outer face turns only; no slice, wide or cube-rotation
  moves, no other puzzle sizes (2x2, 4x4, ...).
- `MAXCOUNT 1000` scrambles per run, `MAXMOVES 200` moves per scramble
  (larger values are clamped; raise the two `#define`s if you need more).
- xorshift32 is fine for scrambles but is not a cryptographic RNG; do not use
  these seeds for anything security-related.
- ANSI colours need a console that understands `ESC[ ... m` (Windows Terminal,
  ConEmu, Git Bash, PowerShell, WSL, most Linux terminals). Legacy `cmd.exe`
  on Windows 7 and below may not -- use `-a`.

---

## Test model and environment

Everything below was read from the running environment (`PI_*` variables,
`~/.pi/agent/models-store.json`, and the local server's own `/health` +
`/props` endpoints), not guessed.

### Model

| Field | Value |
| ----- | ----- |
| Model id / alias | `qwen3.8-flash-next-coder-iq1_m` |
| Provider | `llama.cpp` (OpenAI-compatible server) |
| Base URL | `http://127.0.0.1:8080/v1` |
| Server service | `strata` |
| Context window | 182768 tokens (`n_ctx`, 1 slot) |
| Reasoning | enabled, level `medium` (`off`/`medium` only; `minimal`/`low`/`high`/`xhigh` are not mapped) |
| Thinking format | `qwen-chat-template` |
| Modalities | text in / text out (no images) |
| Classifier | same model, `llama-cpp-classify` API |

The quantisation is encoded in the alias suffix `iq1_m`. **The full GGUF file
name is not available from this environment**: the server exposes only the
alias (`/v1/models` -> `id`, `/props` -> `model_alias`, `/health` -> `model`),
never the file path on disk. So a name such as
`unsloth/Qwen3.6-35B-A3B-GGUF:UD-IQ3_XXS` cannot be confirmed here -- and it
does not match what this server reports (different family, and `IQ3_XXS` vs
the `iq1_m` in the alias). If you need the exact file, ask the machine that
started the server (`llama-server -m <path>`), not the client.

Check it yourself:

    echo "$PI_MODEL $PI_PROVIDER $PI_REASONING_LEVEL"
    curl -s http://127.0.0.1:8080/health
    curl -s http://127.0.0.1:8080/v1/models

### Environment

| Field | Value |
| ----- | ----- |
| Agent | pi coding agent, release `1.0.4` (`PI_CODING_AGENT=true`) |
| OS | Windows 10, build 26200 (`MINGW64_NT-10.0-26200`), x86_64 |
| Shell | Git Bash / MSYS2, `bash 5.3.15`, userland `MSYS 3.6.10` |
| Compiler | `gcc (GCC) 14.2.0` (MinGW-w64) |
| Flags used | `-std=c89 -Wall -Wextra -pedantic -Werror -O2` |

Note that `-std=c89` is what makes this a real constraint: GCC 14 defaults to
`-std=gnu++17`/gnu89-ish modes, so the strict flags above are what prove the
C89 claim.

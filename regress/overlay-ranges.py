#!/usr/bin/env python3
"""Compile the real overlay range functions and check against a cell oracle.

No server, installed binary, terminal or user configuration is touched.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "server-client.c").read_text()
functions = []
for name in ("server_client_overlay_range", "server_client_overlay_range_subtract"):
    match = re.search(r"void\n" + name + r"\([^\n]*(?:\n[^{}]*)?\n\{.*?^\}", source, re.M | re.S)
    if match is None:
        raise RuntimeError("missing source function: " + name)
    functions.append(match.group())
header = (root / "tmux.h").read_text()
capacity = re.search(r"^#define OVERLAY_MAX_RANGES (\d+)$", header, re.M).group(1)
program = r'''
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OVERLAY_MAX_RANGES CAPACITY
#define fatalx(...) abort()
struct overlay_ranges { unsigned int px[OVERLAY_MAX_RANGES], nx[OVERLAY_MAX_RANGES]; };
FUNCTIONS
int main(void) {
    struct overlay_ranges r;
    unsigned int expected[128], actual[128], seed = 17, i, j, t, k, x, n, y;
    for (t = 0; t < 2000; t++) {
        memset(&r, 0xa5, sizeof r);
        server_client_overlay_range(10, 2, 8, 3, 0, 1, 128, &r);
        memset(expected, 0, sizeof expected);
        for (i = 0; i < 128; i++) expected[i] = 1;
        for (k = 0; k < 17; k++) {
            seed = seed * 1664525U + 1013904223U; x = seed % 128;
            seed = seed * 1664525U + 1013904223U; n = seed % 16 + 1;
            seed = seed * 1664525U + 1013904223U; y = seed % 3;
            server_client_overlay_range_subtract(x, y, n, 1, 1, &r);
            if (y == 1)
                for (i = x; i < x+n && i < 128; i++) expected[i] = 0;
            memset(actual, 0, sizeof actual);
            for (i = 0; i < OVERLAY_MAX_RANGES; i++) {
                if (r.px[i] + r.nx[i] > 128) abort();
                for (j = r.px[i]; j < r.px[i] + r.nx[i]; j++) actual[j]++;
            }
            if (memcmp(actual, expected, sizeof actual)) abort();
        }
    }
    puts("overlay range oracle: 34000 checks passed");
}
'''.replace("CAPACITY", capacity).replace("FUNCTIONS", "\n".join(functions))
with tempfile.TemporaryDirectory(prefix="tmux-overlay-unit-") as directory:
    test = Path(directory) / "ranges.c"
    test.write_text(program)
    binary = Path(directory) / "ranges"
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

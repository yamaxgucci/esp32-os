/*
 * ArgonOS - how much of the standard library a .AXE can actually have.
 *
 * CXXTEST proves the language works.  This proves the library does, which is a
 * different question: the headers are all there in the toolchain, but half of
 * std::string and std::vector is out-of-line code in libstdc++.a, and that
 * archive was built for a hosted newlib with exceptions on.  What matters is
 * not whether it links but what it drags in when it does.
 *
 * Every container here is one a vendored C++ engine will use on its first
 * page.  If this runs, porting one is a question of size and speed rather than
 * of whether the toolchain can be made to produce an image at all.
 *
 *   run h:\cxxstl.axe
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "ag_cxx.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

AG_APP("CXXSTL", "1.0", "argon", 0);

static int s_failures;

static void check(const char *what, bool ok)
{
    ag_printf("%-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++s_failures;
    }
}

extern "C" int ag_cxx_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_printf("CXXSTL - the standard library in a .AXE\n\n");

    /* Long enough to be on the heap rather than in the small-string buffer:
     * the interesting path is the one that calls operator new. */
    std::string s = "master";
    s += ".dat";
    s.insert(0, "fallout/");
    check("std::string", s == "fallout/master.dat" && s.size() == 18);

    std::string big(200, 'x');
    big += big;
    check("std::string on the heap", big.size() == 400 && big[399] == 'x');

    std::vector<int> v;
    for (int i = 0; i < 1000; ++i) {
        v.push_back((i * 37) % 101);
    }
    std::sort(v.begin(), v.end());
    check("std::vector and std::sort",
          v.size() == 1000 && v.front() <= v.back() &&
              std::is_sorted(v.begin(), v.end()));

    std::vector<std::string> names;
    names.push_back("critter");
    names.push_back("master");
    names.push_back("art");
    std::sort(names.begin(), names.end());
    check("vector of strings", names[0] == "art" && names[2] == "master");

    std::map<std::string, int> lut;
    lut["hp"] = 30;
    lut["ap"] = 8;
    lut["ac"] = 5;
    check("std::map", lut.size() == 3 && lut["hp"] == 30 &&
                          lut.find("none") == lut.end());

    ag_printf("\n%d failure(s)\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}

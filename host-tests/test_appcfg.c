/*
 * ArgonOS - the per-application settings file, as text (ABI 0.71).
 *
 * Two things are being pinned.  First, that a key can be read back exactly as it
 * was written - including a value with spaces, quotes and the `;` that would
 * otherwise start a comment, which is the case the shop itself needs for a
 * directory called "A:\APPS".  Second, that setting a key leaves every other
 * line alone: these files are meant to be opened and edited by a person, and a
 * write that reformatted or reordered somebody's file would be a write they
 * cannot trust.
 *
 * The file I/O is not here - it is appcfg.c, and it needs a filesystem.  This is
 * the half that can be wrong silently, which is why it is the half with tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "../components/argon_kernel/src/core/appcfg_text.h"

#include <stdio.h>
#include <string.h>

static void test_get(void)
{
    const char *text =
        "; a comment line\n"
        "[section]\n"
        "install.dir = A:\\APPS\n"
        "path = \"C:\\APPS;A:\\APPS\"\n"
        "spaces   =   trimmed   \n"
        "empty =\n"
        "\n"
        "number = 42\n"
        "hex = 0x1F\n"
        "negative = -7\n"
        "quoted-semicolon = \"a ; b\"\n";

    char value[AG_APPCFG_VALUE_MAX];

    AG_CHECK(ag_appcfg_text_get(text, "install.dir", value, sizeof(value)));
    AG_CHECK_STR(value, "A:\\APPS");

    /* A quoted value keeps its ';': this is why SYSTEM.CFG grew quoting, and the
     * settings file inherits the rule so the two files read the same way. */
    AG_CHECK(ag_appcfg_text_get(text, "path", value, sizeof(value)));
    AG_CHECK_STR(value, "C:\\APPS;A:\\APPS");

    /* Surrounding spaces are not part of the value; a hand-aligned file is
     * still a file with the values the person meant. */
    AG_CHECK(ag_appcfg_text_get(text, "spaces", value, sizeof(value)));
    AG_CHECK_STR(value, "trimmed");

    AG_CHECK(ag_appcfg_text_get(text, "empty", value, sizeof(value)));
    AG_CHECK_STR(value, "");

    AG_CHECK(ag_appcfg_text_get(text, "quoted-semicolon", value,
                                sizeof(value)));
    AG_CHECK_STR(value, "a ; b");

    /* A key that is not there, and a prefix of one that is. */
    AG_CHECK(!ag_appcfg_text_get(text, "install", value, sizeof(value)));
    AG_CHECK(!ag_appcfg_text_get(text, "nothing", value, sizeof(value)));

    /* An unquoted comment after a value, which is how a person annotates one. */
    AG_CHECK(ag_appcfg_text_get("key = value ; why\n", "key", value,
                                sizeof(value)));
    AG_CHECK_STR(value, "value");

    AG_CHECK_INT(ag_appcfg_text_get_int(text, "number", -1), 42);
    AG_CHECK_INT(ag_appcfg_text_get_int(text, "hex", -1), 0x1F);
    AG_CHECK_INT(ag_appcfg_text_get_int(text, "negative", 0), -7);
    AG_CHECK_INT(ag_appcfg_text_get_int(text, "missing", 99), 99);
    AG_CHECK_INT(ag_appcfg_text_get_int(text, "install.dir", 99), 99);
}

static void test_set(void)
{
    char out[AG_APPCFG_TEXT_MAX];

    /* Appended when the key is new. */
    size_t n = ag_appcfg_text_set("keep = me\n", "install.dir", "A:\\APPS", out,
                                  sizeof(out));
    AG_CHECK(n > 0);
    AG_CHECK_STR(out, "keep = me\ninstall.dir = A:\\APPS\n");

    /* Replaced in place, and everything else copied byte for byte - comments
     * included, because the comment may be why the value is what it is. */
    const char *original = "; why: the card slot\n"
                           "install.dir = C:\\APPS\n"
                           "other = 1\n";
    n = ag_appcfg_text_set(original, "install.dir", "A:\\APPS", out,
                           sizeof(out));
    AG_CHECK(n > 0);
    AG_CHECK_STR(out, "; why: the card slot\ninstall.dir = A:\\APPS\nother = 1\n");

    /* The same key twice: the first wins, and the second is left alone.  A file
     * edited by hand may have both, and silently picking the last would make the
     * file's meaning depend on the editor. */
    n = ag_appcfg_text_set("a = 1\na = 2\n", "a", "3", out, sizeof(out));
    AG_CHECK(n > 0);
    AG_CHECK_STR(out, "a = 3\na = 2\n");

    /* A value with spaces and quotes survives the round trip. */
    n = ag_appcfg_text_set("", "path", "\"C:\\APPS;A:\\APPS\"", out,
                           sizeof(out));
    AG_CHECK(n > 0);
    char back[AG_APPCFG_VALUE_MAX];
    AG_CHECK(ag_appcfg_text_get(out, "path", back, sizeof(back)));
    AG_CHECK_STR(back, "C:\\APPS;A:\\APPS");

    /* Too little room: nothing is written, so a full file cannot lose the
     * settings it already had. */
    char tiny[16];
    n = ag_appcfg_text_set("a = 1\n", "install.dir", "A:\\APPS", tiny,
                           sizeof(tiny));
    AG_CHECK_INT(n, 0);
}

static void test_file_name(void)
{
    char name[AG_APPCFG_KEY_MAX];

    ag_appcfg_file_name("STORE", name, sizeof(name));
    AG_CHECK_STR(name, "STORE");

    /* An application's name is whatever its author typed: case and punctuation
     * must not decide which file it lands in, and a name with a path separator
     * must not become a path. */
    ag_appcfg_file_name("store", name, sizeof(name));
    AG_CHECK_STR(name, "STORE");
    ag_appcfg_file_name("..\\..\\SYSTEM", name, sizeof(name));
    AG_CHECK_STR(name, "SYSTEM");
    ag_appcfg_file_name("my app", name, sizeof(name));
    AG_CHECK_STR(name, "MYAPP");

    ag_appcfg_file_name("", name, sizeof(name));
    AG_CHECK_STR(name, "APP");
    ag_appcfg_file_name(NULL, name, sizeof(name));
    AG_CHECK_STR(name, "APP");

    /* The buffer is the caller's, and a name longer than it is cut rather than
     * overflowed. */
    char small[5];
    ag_appcfg_file_name("ABCDEFGH", small, sizeof(small));
    AG_CHECK_STR(small, "ABCD");
}

void run_appcfg_tests(void)
{
    test_get();
    test_set();
    test_file_name();
}

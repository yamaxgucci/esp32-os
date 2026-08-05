/* Fixture for the loader tests: exercises the relocation kinds an application
 * actually produces - internal calls, literal pool loads, string constants,
 * static data, and calls out to the kernel through the API table. */
#include <argon/argon.h>

AG_APP("SAMPLE", "1.0", "argon", 0);

static int  s_counter;
static char s_scratch[64];

static const char k_message[] = "sample application";

static int triple(int x) { return x * 3; }

int helper_visible(int x);
int helper_visible(int x) { return triple(x) + s_counter; }

int ag_main(int argc, char **argv)
{
    s_counter = argc;
    ag_print(k_message);
    ag_printf("argv0=%s\n", (argc > 0) ? argv[0] : "none");
    for (int i = 0; i < (int)sizeof(s_scratch); i++) {
        s_scratch[i] = (char)i;
    }
    return helper_visible(argc) + s_scratch[3];
}
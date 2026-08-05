/* Fixture for the loader tests: exercises the relocation kinds an application
 * actually produces - internal calls, literal pool loads, string constants,
 * static data, calls out to the kernel through the API table, and initialised
 * pointers in both directions across the split, which are the words that need
 * the bias of a part other than the one they live in. */
#include <argon/argon.h>

AG_APP("SAMPLE", "1.0", "argon", 0);

static int  s_counter;
static char s_scratch[64];

static const char k_message[] = "sample application";

/*
 * A word in the data part holding an address in the code part, and one holding
 * an address in the data part.  Both have external linkage on purpose: GCC moves
 * a static pointer it can prove is never written into .rodata, which would put
 * these in the code part and leave the case they exist to cover untested.
 */
const char *g_message = k_message;
char       *g_cursor = s_scratch;

static int triple(int x) { return x * 3; }

int helper_visible(int x);
int helper_visible(int x) { return triple(x) + s_counter; }

int ag_main(int argc, char **argv)
{
    s_counter = argc;
    ag_print(g_message);
    ag_printf("argv0=%s\n", (argc > 0) ? argv[0] : "none");
    for (int i = 0; i < (int)sizeof(s_scratch); i++) {
        g_cursor[i] = (char)i;
    }
    return helper_visible(argc) + s_scratch[3];
}

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
static const char k_alpha[] = "alpha";
static const char k_beta[] = "beta";

/*
 * A word in the data part holding an address in the code part, and one holding
 * an address in the data part.  Both have external linkage on purpose: GCC moves
 * a static pointer it can prove is never written into .rodata, and a constant is
 * a different case from writable data - the point here is the writable one.
 */
const char *g_message = k_message;
char       *g_cursor = s_scratch;

static int triple(int x) { return x * 3; }

int helper_visible(int x);
int helper_visible(int x) { return triple(x) + s_counter; }

/*
 * Constants travel with the data, so a table of pointers to them is a run of
 * words in the data part pointing back into it, and a table of function pointers
 * is a run pointing into the code part - the combination that did not exist while
 * .rodata rode along with the code.
 */
typedef int (*sample_fn_t)(int);

const char *const k_words[] = {k_alpha, k_beta, k_message};
const sample_fn_t k_handlers[] = {triple, helper_visible};

/*
 * The escape hatch, and so the one constant that stays in the code part.  It
 * holds an address in the data part, which makes it also a word in the code part
 * that is not a literal pool entry.
 */
AG_HOT_RODATA const char *const k_hot[] = {k_beta, k_alpha};

/*
 * A switch wide enough that the compiler builds a jump table for it.  That table
 * is a constant, so it lands in the data part, and it holds addresses in the code
 * part - the same class of word as k_handlers, but put there by the compiler
 * rather than by anyone.  If a future compiler stops building the table, this
 * function covers less than it does now; k_handlers is what keeps the case
 * covered regardless.
 */
int step(int x);
int step(int x)
{
    switch (x) {
    case 0:  ag_print(k_words[0]); break;
    case 1:  ag_print(k_words[1]); break;
    case 2:  s_counter += 3; break;
    case 3:  g_cursor[0] = 'a'; break;
    case 4:  ag_print(k_words[2]); break;
    case 5:  s_counter *= 5; break;
    case 6:  g_cursor[1] = 'b'; break;
    case 7:  ag_print(g_message); break;
    case 8:  s_counter -= 7; break;
    case 9:  g_cursor[2] = 'c'; break;
    case 10: ag_print(k_hot[0]); break;
    case 11: s_counter ^= 11; break;
    case 12: g_cursor[3] = 'd'; break;
    case 13: ag_print(k_hot[1]); break;
    case 14: s_counter += 13; break;
    case 15: g_cursor[4] = 'e'; break;
    default: s_counter = 0; break;
    }
    return s_counter;
}

int ag_main(int argc, char **argv)
{
    s_counter = argc;
    ag_print(g_message);
    ag_print(k_words[argc % 3]);
    ag_printf("argv0=%s hot=%s\n", (argc > 0) ? argv[0] : "none",
              k_hot[argc & 1]);
    for (int i = 0; i < (int)sizeof(s_scratch); i++) {
        g_cursor[i] = (char)i;
    }
    return k_handlers[argc & 1](argc) + step(argc & 15) + s_scratch[3];
}

/*
 * ArgonOS port: ESP-IDF - catching a fault and blaming the right process.
 *
 * Two halves, and the file is split down the middle by the instruction set
 * because argon/port/fault.h says this is the one place where that shows: an
 * Xtensa half that works, and a RISC-V half that says so.
 *
 * Without an MMU a wild pointer can reach anything, and that is a deliberate
 * trade [Т-12].  What is not acceptable is the consequence: an application that
 * dereferences nothing taking the whole system down with it.  Most faults are
 * not corruption at all - a null pointer, a bad cast, a jump through an
 * uninitialised function pointer - and the machine is perfectly healthy
 * afterwards.  Those should cost one process, not a reboot.
 *
 * How it works.  Xtensa dispatches general exceptions through a table of
 * per-cause handlers, and the documented contract of that table is exactly what
 * is needed: "if the handler returns, the thread context will be restored, and
 * any values in the exception frame modified by the handler will be restored as
 * part of the context".  So the handler changes the return PC to point at a
 * recovery routine and returns.  The faulting task resumes - in ordinary task
 * context, on its own stack - inside code that reports what happened and ends
 * the process.
 *
 * What the handler must not do: take a lock, log, or allocate.  It runs in an
 * exception context where any of those would be a second, worse failure.  It
 * writes down what it saw and gets out; everything that needs a lock happens in
 * the recovery routine, which is a normal task again.
 *
 * When it steps aside.  If the faulting task is not an application, or it is
 * inside the kernel holding a lock, or it has already faulted once, the fault is
 * left to take its normal course - the panic handler, with its full report and a
 * reboot.  A process unwound out of a held kernel lock would leave that lock
 * held forever, which is a hung system: exactly what this is trying to avoid.
 *
 * One case is out of reach by construction: a fault taken while the flash cache
 * is disabled - during a flash write, say - cannot even fetch this handler, and
 * goes straight to the panic path.  That is the correct outcome anyway, and it is
 * why the panic handler is the one that lives in IRAM and this one does not.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/fault.h>

/* Wanted by both halves: the watchpoint at the end of this file is portable. */
#include "esp_cpu.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(__XTENSA__)

#include <xtensa/corebits.h>

#include "esp_rom_sys.h"
#include "xtensa_api.h"

#include <argon/port/task.h>

/*
 * The causes worth taking over: the ones that mean the program is wrong.
 *
 * Deliberately absent: SYSCALL (1), LEVEL1_INTERRUPT (4), ALLOCA (5) and the
 * coprocessor-disabled causes (32..39).  Those are working machinery - window
 * handling, interrupts, lazy FPU save - and taking them over would break the
 * system rather than protect it.
 */
static const int k_causes[] = {
    EXCCAUSE_ILLEGAL,               /* 0  */
    EXCCAUSE_INSTR_ERROR,           /* 2  */
    EXCCAUSE_LOAD_STORE_ERROR,      /* 3  */
    EXCCAUSE_DIVIDE_BY_ZERO,        /* 6  */
    EXCCAUSE_PC_ERROR,              /* 7  */
    EXCCAUSE_PRIVILEGED,            /* 8  */
    EXCCAUSE_UNALIGNED,             /* 9  */
    EXCCAUSE_INSTR_DATA_ERROR,      /* 12 */
    EXCCAUSE_LOAD_STORE_DATA_ERROR, /* 13 */
    EXCCAUSE_INSTR_ADDR_ERROR,      /* 14 */
    EXCCAUSE_LOAD_STORE_ADDR_ERROR, /* 15 */
    EXCCAUSE_INSTR_RING,            /* 18 */
    EXCCAUSE_INSTR_PROHIBITED,      /* 20 */
    EXCCAUSE_LOAD_STORE_RING,       /* 26 */
    EXCCAUSE_LOAD_PROHIBITED,       /* 28 */
    EXCCAUSE_STORE_PROHIBITED,      /* 29 */
};

#define AG_FAULT_CAUSE_MAX 40

static xt_exc_handler s_chain[AG_FAULT_CAUSE_MAX];
static bool           s_chain_saved;
static bool           s_installed;

static ag_port_fault_note_fn    s_note;
static ag_port_fault_recover_fn s_recover;

const char *ag_port_fault_cause_name(uint32_t cause)
{
    switch (cause) {
    case EXCCAUSE_ILLEGAL:               return "illegal instruction";
    case EXCCAUSE_INSTR_ERROR:           return "instruction fetch error";
    case EXCCAUSE_LOAD_STORE_ERROR:      return "load or store error";
    case EXCCAUSE_DIVIDE_BY_ZERO:        return "divide by zero";
    case EXCCAUSE_PC_ERROR:              return "jump to an illegal address";
    case EXCCAUSE_PRIVILEGED:            return "privileged instruction";
    case EXCCAUSE_UNALIGNED:             return "unaligned load or store";
    case EXCCAUSE_INSTR_DATA_ERROR:      return "bus error fetching code";
    case EXCCAUSE_LOAD_STORE_DATA_ERROR: return "bus error on data";
    case EXCCAUSE_INSTR_ADDR_ERROR:      return "bad address fetching code";
    case EXCCAUSE_LOAD_STORE_ADDR_ERROR: return "bad address on data";
    case EXCCAUSE_INSTR_RING:            return "code fetch not permitted";
    case EXCCAUSE_INSTR_PROHIBITED:      return "execute from a non-code address";
    case EXCCAUSE_LOAD_STORE_RING:       return "data access not permitted";
    case EXCCAUSE_LOAD_PROHIBITED:       return "read from an invalid address";
    case EXCCAUSE_STORE_PROHIBITED:      return "write to an invalid address";
    default:                             return "fault";
    }
}

/*
 * Where a faulted application lands.  A separate function, and not static, so
 * that its address is a plain constant to put in the frame and so the compiler
 * cannot decide to inline it into somewhere it does not belong.
 */
void ag_fault_trampoline(void);

void ag_fault_trampoline(void)
{
    s_recover();

    /*
     * Unreachable: recover does not return.  If it somehow did, going back to
     * the instruction that faulted would loop forever, so stop here instead and
     * let the watchdog have it.
     */
    for (;;) {
        ag_port_task_delay(AG_PORT_FOREVER);
    }
}

static void fault_handler(XtExcFrame *frame)
{
    const uint32_t cause = (uint32_t)frame->exccause;

    /*
     * Nothing here locks, logs or allocates: this is an exception context, and
     * the note callback is written to the same rule.  It answers whether this
     * fault belongs to a recoverable application.
     */
    /*
     * a0 is the return address in the windowed ABI - the same thing RISC-V
     * calls ra - and it is read here for the same reason: the record should say
     * not only where the program died but where it came from.
     */
    if (s_note(cause, (uint32_t)frame->pc, (uint32_t)frame->excvaddr,
               (uint32_t)frame->a1, (uint32_t)frame->a0)) {
        /*
         * Say it here, from the exception itself, with esp_rom_printf - the one
         * output that is safe in this context (it busy-writes UART0 through ROM,
         * no lock, no heap, the same call the panic handler uses).  The tidy
         * report comes later from a task context, but only if recovery gets
         * there: an application that faulted by corrupting its own stack can
         * take the unwind down with it, and then this line is the only record
         * of where it died.
         */
        esp_rom_printf("\n[fault] app pc=0x%08x addr=0x%08x cause=%u (%s)\n",
                       (uint32_t)frame->pc, (uint32_t)frame->excvaddr,
                       (unsigned)cause, ag_port_fault_cause_name(cause));
        frame->pc = (long)&ag_fault_trampoline;
        return;
    }

    /* Not ours to recover: the normal path, with its report and its reboot. */
    if (cause < AG_FAULT_CAUSE_MAX && s_chain[cause] != NULL) {
        s_chain[cause](frame);
    }
}

/* Runs once per core: the exception table belongs to the core it is set on. */
static void install_here(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < sizeof(k_causes) / sizeof(k_causes[0]); i++) {
        const int         cause = k_causes[i];
        xt_exc_handler    previous = xt_set_exception_handler(cause,
                                                             fault_handler);
        if (!s_chain_saved && cause < AG_FAULT_CAUSE_MAX) {
            s_chain[cause] = previous;
        }
    }
}

ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
                            ag_port_fault_recover_fn recover)
{
    if (note == NULL || recover == NULL) {
        return -AG_EINVAL;
    }
    if (s_installed) {
        return AG_OK;
    }

    /* Set before any handler can run, which is the moment install_here returns. */
    s_note = note;
    s_recover = recover;

    install_here(NULL);
    s_chain_saved = true; /* the other core's table holds the same handlers */

#if portNUM_PROCESSORS > 1
    /*
     * The application runs on the other core, which has an exception table of
     * its own; the handler has to be installed there by something running there.
     */
    const BaseType_t other = (xPortGetCoreID() == 0) ? 1 : 0;
    if (esp_ipc_call_blocking((uint32_t)other, install_here, NULL) != ESP_OK) {
        return -AG_EIO;
    }
#endif

    s_installed = true;
    return AG_OK;
}

/*
 * No record is kept across a reboot on Xtensa, and that is a real gap rather
 * than a decision: an application fault is recovered here and written to the
 * crash log by the supervisor, so the only panic left is one the kernel itself
 * takes - rare, and visible on the console of a board somebody is looking at.
 * The RISC-V half below keeps one, because there a panic during an unattended
 * soak is the case that has to be caught.
 */
bool ag_port_fault_last_panic(ag_port_panic_t *out)
{
    (void)out;
    return false;
}

/* Nothing is kept here, so there is nothing to forget. */
void ag_port_fault_panic_reported(void)
{
}

#else /* !__XTENSA__ - the RISC-V parts (C3, C6, H2, P4) */

/*
 * RECOVERING A FAULT ON RISC-V, which this file used to say was impossible.
 *
 * The Xtensa half above rests on a table of per-cause handlers whose documented
 * contract is that returning from one restores the thread context, including
 * whatever the handler changed in the frame.  RISC-V has no such table, and this
 * file used to stop there.  Two facts about ESP-IDF 5.5 change the answer.
 *
 * 1. THE VECTOR ALREADY SUPPORTS A RETURNING HANDLER, for exceptions as well as
 *    interrupts.  `components/riscv/vectors.S` saves an RvExcFrame on the
 *    faulting task's own stack, keeps mcause, and then, for a real exception,
 *    calls `panic_from_exception` - an assembler alias for the C function
 *    `xt_unhandled_exception`.  Immediately after that call the assembly says,
 *    in a comment of its own, that it arrives there "if the exception handler
 *    has returned", and jumps to `_return_from_exception`, which writes mepc
 *    back from the frame with `restore_xepc`, restores the general registers and
 *    executes `mret`.  The trap is the hardware's; the frame is ours to modify.
 *    So the Xtensa trick is available here in a different spelling: change the
 *    PC in the frame, return, and the faulting task resumes where we pointed it,
 *    in ordinary task context, on its own stack.  That is the whole of what the
 *    contract in argon/port/fault.h asks for.
 *
 * 2. THE FUNCTION IS A PLAIN GLOBAL SYMBOL, so the linker can route that one
 *    call to us - `-Wl,--wrap=xt_unhandled_exception`, set for this component in
 *    CMakeLists.txt - without patching ESP-IDF or copying the vector into this
 *    tree.  `--wrap` rewrites just the reference, and the reference is exactly
 *    one, in the trap entry.
 *
 * THE PANIC PATH IS HOOKED AS WELL, and for a different reason.  A fault the
 * port must refuse - the kernel's own, one taken while a kernel lock is held, a
 * second one while the first is being unwound, or one taken with the flash cache
 * disabled, where the code that decides cannot be fetched at all - goes on to the
 * panic handler and reboots the machine.  That is the panic this whole exercise
 * is about, and today it leaves NOTHING: /sys/crash.log is written by the
 * supervisor, the supervisor never runs again, and the console report is gone
 * with the scrollback.  So the frame is copied into RTC memory - raw numbers, no
 * lock, no allocation, no flash access, nothing that could fail a second time -
 * and read back at the next boot.  It is written before anything else in the
 * path, so that it exists whether or not we recover.
 *
 * WHY THAT ORDER.  The record is written first and the claim decision second.
 * The reverse would be tidier and wrong: the claim decision calls into the
 * kernel, and a fault taken there would leave no record at all.
 */

#include <stddef.h>

#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_rom_sys.h"
#include "riscv/rvruntime-frames.h"
#include "soc/soc_memory_layout.h"

#include <argon/port/sys.h> /* AG_PORT_NOINIT: memory that survives the reboot */
#include <argon/port/task.h>

/* mcause, bit 31: set for an interrupt, clear for an exception. */
#define AG_RV_MCAUSE_INTERRUPT 0x80000000u
#define AG_RV_MCAUSE_CODE_MASK 0x0000001fu

/*
 * The record as it is kept between the panic and the next boot.  `magic` is
 * written LAST and cleared first: a machine that dies while writing this leaves
 * something that is not mistaken for a record.  `check` is the same idea for a
 * record that was half overwritten by a later, shorter one.
 */
#define AG_PANIC_MAGIC 0x50414E43u /* "PANC" */

typedef struct {
    uint32_t magic;
    uint32_t check;
    uint32_t cause;
    uint32_t pc;
    uint32_t ra;
    uint32_t vaddr;
    uint32_t sp;
    uint32_t image;
    uint32_t words;
    uint32_t stack[AG_PORT_FAULT_STACK_WORDS];
} ag_panic_record_t;

static AG_PORT_NOINIT ag_panic_record_t s_panic;

/* In ordinary RAM, and deliberately: it only has to stop the panic path from
 * overwriting a fault record with a coarser one for the same panic. */
static volatile bool s_recorded;

static ag_port_fault_note_fn     s_note;
static ag_port_fault_recover_fn  s_recover;
static bool                      s_installed;

/*
 * FNV-1a over the record, `magic` and `check` excluded.  Not a checksum against
 * corruption, a checksum against half a record - a half-written one is worse
 * than none.
 *
 * IT USED TO START AT `check`, and that is why no panic record ever survived a
 * reboot.  A field cannot be included in its own checksum: `record_save` hashes
 * the record while `check` still holds the PREVIOUS record's number (or zero),
 * stores the result into `check`, and the next boot recomputes over the number
 * that is now there and gets something else.  Every record was therefore a
 * mismatch, `ag_port_fault_last_panic` answered false for all of them, and the
 * one defect this file exists to end - a panic that leaves nothing to read -
 * looked exactly like an RTC domain that does not retain, which is a different
 * and much more expensive conclusion.  Measured: /sys/crash.log had never once
 * contained the words "panic before this boot".
 *
 * So the hash begins at `cause`, the first byte that is neither the marker nor
 * the checksum.  `check` needs no protection of its own: a damaged checksum
 * reads as a mismatch, which is the same answer as a damaged record.
 */
static IRAM_ATTR uint32_t record_check(const ag_panic_record_t *r)
{
    const uint8_t *p = (const uint8_t *)&r->cause;
    const size_t   n = sizeof(*r) - offsetof(ag_panic_record_t, cause);
    uint32_t       h = 2166136261u;

    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static IRAM_ATTR int hex_digit(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return 0;
}

/*
 * The first four bytes of the running image's ELF SHA256, from the string IDF
 * keeps in RAM for exactly this purpose (esp_app_desc.h says so in as many
 * words: "can be used in panic handler or core dump when cache is disabled").
 * Four bytes is not the SHA; it is enough to tell two builds of this tree apart
 * when the kept ELF is chosen, which is all the record needs.
 */
static IRAM_ATTR uint32_t image_prefix(void)
{
    const char *sha = esp_app_get_elf_sha256_str();
    uint32_t    v = 0;

    if (sha == NULL) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (sha[i] == '\0') {
            break;
        }
        v = (v << 4) | (uint32_t)hex_digit(sha[i]);
    }
    return v;
}

/*
 * Reading the faulting stack, carefully.  The SP in the frame is the one the
 * task had before the trap, so the words above it are its own return addresses
 * and locals, and they are what a backtrace would walk.  The guard is not
 * theory: a fault caused by a corrupted SP would make THIS read fault, turning
 * one panic into a double one, so anything outside a mapped RAM region is not
 * read at all.  Eight words is short enough to be worth having and long enough
 * to hold the two or three frames above the fault.
 */
static IRAM_ATTR void record_stack(ag_panic_record_t *r, uint32_t sp)
{
    r->words = 0;
    if (sp == 0 || (sp & 3u) != 0) {
        return;
    }

    const uint32_t *p = (const uint32_t *)sp;
    for (uint32_t i = 0; i < AG_PORT_FAULT_STACK_WORDS; i++) {
        const void *at = (const void *)(p + i);
        if (!esp_ptr_in_dram(at) && !esp_ptr_external_ram(at)) {
            break; /* a corrupted SP must not make the record fault too */
        }
        r->stack[i] = p[i];
        r->words = i + 1;
    }
}

static IRAM_ATTR void record_save(uint32_t cause, uint32_t pc, uint32_t ra,
                                  uint32_t vaddr, uint32_t sp)
{
    ag_panic_record_t *r = &s_panic;

    r->magic = 0; /* a record being written is not a record yet */
    r->cause = cause;
    r->pc = pc;
    r->ra = ra;
    r->vaddr = vaddr;
    r->sp = sp;
    r->image = image_prefix();
    record_stack(r, sp);
    r->check = record_check(r);
    r->magic = AG_PANIC_MAGIC;
    s_recorded = true;
}

/*
 * Where a faulted application lands.  A separate function, and not static, so
 * that its address is a plain constant to put in the frame and the compiler
 * cannot decide to inline it into somewhere it does not belong.
 */
void ag_fault_trampoline(void);

void ag_fault_trampoline(void)
{
    s_recover();

    /* Unreachable: recover does not return.  Going back to the instruction that
     * faulted would loop for ever, so stop here and let the watchdog have it. */
    for (;;) {
        ag_port_task_delay(AG_PORT_FOREVER);
    }
}

const char *ag_port_fault_cause_name(uint32_t cause)
{
    /* mcause exception codes, for the crash record. */
    switch (cause) {
    case 0u:  return "instruction address misaligned";
    case 1u:  return "instruction access fault";
    case 2u:  return "illegal instruction";
    case 3u:  return "breakpoint";
    case 4u:  return "load address misaligned";
    case 5u:  return "load access fault";
    case 6u:  return "store address misaligned";
    case 7u:  return "store access fault";
    case 8u:  return "environment call";
    case 11u: return "environment call from machine mode";
    case AG_PORT_FAULT_CAUSE_PANIC: return "panic, not a fault";
    default:  return "fault";
    }
}

/*
 * The trap entry, wrapped.  IRAM: this runs before anything has been decided
 * about the state of the flash cache, and a fault taken while the cache is
 * disabled must still reach a handler that can run.
 */
void IRAM_ATTR __real_xt_unhandled_exception(void *frame);

void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame)
{
    if (frame == NULL) {
        __real_xt_unhandled_exception(frame);
        return; /* not reached */
    }

    RvExcFrame  *f = (RvExcFrame *)frame;
    const uint32_t raw = (uint32_t)f->mcause;
    const bool interrupt = (raw & AG_RV_MCAUSE_INTERRUPT) != 0;
    const uint32_t cause = interrupt ? AG_PORT_FAULT_CAUSE_PANIC
                                     : (raw & AG_RV_MCAUSE_CODE_MASK);

    /* Before the claim, not after: see WHY THAT ORDER above. */
    record_save(cause, (uint32_t)f->mepc, (uint32_t)f->ra, (uint32_t)f->mtval,
                (uint32_t)f->sp);

    /*
     * A deliberate abort is not an application fault.  ESP-IDF's panic_abort()
     * is an illegal instruction on this architecture, so it arrives here looking
     * exactly like one; recovering from it would kill the application and leave
     * IDF's abort flag set for whatever faults next.  Assertions and aborts are
     * the machine saying it cannot continue, and it goes on saying it.
     *
     * spi_flash_cache_enabled() is the other half of "when it steps aside".  The
     * claim below runs kernel code out of flash, and this handler is in IRAM
     * precisely so that a fault taken with the cache off still reaches it - but
     * the decision itself cannot be fetched then, so that fault is recorded and
     * left to the panic path, which is in IRAM all the way down.
     */
    const bool claimable = !interrupt && !g_panic_abort && s_note != NULL &&
                           spi_flash_cache_enabled();

    if (claimable) {
        /*
         * The note callback answers whether this fault belongs to an
         * application that can be unwound.  Like the Xtensa handler it takes no
         * lock, logs nothing and allocates nothing; everything that needs any of
         * that happens in the trampoline, which is ordinary task context again.
         */
        if (s_note(cause, (uint32_t)f->mepc, (uint32_t)f->mtval,
                   (uint32_t)f->sp, (uint32_t)f->ra)) {
            /*
             * One line from the exception itself, with esp_rom_printf - the one
             * output that is safe here (it busy-writes through the ROM, no lock,
             * no heap; the panic handler uses the same call).  The tidy record
             * comes later from the supervisor, but only if recovery gets there:
             * an application that faulted by corrupting its own stack can take
             * the unwind down with it, and then this line is all there is.
             */
            esp_rom_printf("\n[fault] app pc=0x%08x ra=0x%08x addr=0x%08x "
                           "cause=%u (%s) sp=0x%08x\n",
                           (unsigned)f->mepc, (unsigned)f->ra,
                           (unsigned)f->mtval, (unsigned)cause,
                           ag_port_fault_cause_name(cause), (unsigned)f->sp);

            /*
             * The vector restores the PC from this field and mrets.  Point it at
             * the trampoline and the faulting task wakes up there, on its own
             * stack, with the console lock and everything else exactly as they
             * were.
             */
            f->mepc = (long)(uintptr_t)&ag_fault_trampoline;

            /*
             * AND THE RECORD GOES, because this fault is not a panic after all.
             *
             * It was written before the claim - see WHY THAT ORDER above - so
             * that a fault taken INSIDE the claim would still leave one.  The
             * claim said yes, so what follows is an ordinary application kill,
             * which the kernel writes to /sys/crash.log itself, from ordinary
             * context, with the journal tail and everything a record wants.
             *
             * Leaving it behind is not harmless: the record is handed over once,
             * at the NEXT boot, under the words "panic before this boot".  A
             * bench that recovers a fault and is rebooted later - or merely has
             * its console opened, which resets this chip - would then read a
             * panic that never happened, at an address that is real, which is
             * the most expensive kind of wrong: it costs somebody a day and
             * sends them after a bug they cannot find because it is not there.
             * The record means "a panic", and a recovered fault is not one.
             *
             * A word store and nothing else, and it is safe here: no lock, no
             * allocation, and this is the one path in the handler that has
             * already decided it will not panic.  A second fault during the
             * unwind re-enters the wrapper ABOVE, which writes a fresh record
             * first and then refuses to claim it (p->fault.pending is set), so
             * clearing here cannot lose a panic that follows this one.
             */
            s_panic.magic = 0;
            s_panic.check = 0;
            s_recorded = false;
            return;
        }
    }

    /* Not ours to recover: the normal path, with its report and its reboot. */
    __real_xt_unhandled_exception(frame);
}

/*
 * The other entry into the panic path: a watchdog, a cache error, a memory
 * protection fault, or anything the SoC reports as a pseudo-cause, none of which
 * arrive as an exception the vector calls us for.  All of them end in
 * esp_panic_handler(), which is where the record is taken for them.  A fault
 * that we already recorded has the better frame, so this does not overwrite it.
 */
void IRAM_ATTR __real_esp_panic_handler(panic_info_t *info);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (!s_recorded && info != NULL && info->frame != NULL) {
        const RvExcFrame *f = (const RvExcFrame *)info->frame;
        const uint32_t    raw = (uint32_t)f->mcause;
        /*
         * A pseudo-cause is not an exception of the hardware's: the SoC has
         * written its own marker into mcause for a watchdog or a cache error, and
         * a number that is not an exception code must not be printed as one.
         */
        const bool fault = !info->pseudo_excause &&
                           (info->exception == PANIC_EXCEPTION_FAULT) &&
                           !g_panic_abort;

        record_save(fault ? (raw & AG_RV_MCAUSE_CODE_MASK)
                          : AG_PORT_FAULT_CAUSE_PANIC,
                    (uint32_t)f->mepc, (uint32_t)f->ra, (uint32_t)f->mtval,
                    (uint32_t)f->sp);
    }
    __real_esp_panic_handler(info);
}

ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
                            ag_port_fault_recover_fn recover)
{
    if (note == NULL || recover == NULL) {
        return -AG_EINVAL;
    }
    if (s_installed) {
        return AG_OK;
    }

    /* Set before an exception can run one: the wrap is installed by the linker,
     * so the moment this function returns, the handler will use them. */
    s_note = note;
    s_recover = recover;

    /*
     * A record left by the previous boot is NOT touched here: the supervisor
     * reads it a few lines later in its own boot, and this call comes first.
     * It is cleared where it is handed over, which is that read.
     */
    s_recorded = false;

    /*
     * Populate the ELF SHA256 string now, in ordinary context with the flash
     * cache on: from here on the panic path only reads it, out of RAM.
     */
    {
        char sha[16];
        (void)esp_app_get_elf_sha256(sha, sizeof(sha));
    }

    s_installed = true;
    return AG_OK;
}

bool ag_port_fault_last_panic(ag_port_panic_t *out)
{
    if (out == NULL) {
        return false;
    }
    if (s_panic.magic != AG_PANIC_MAGIC || s_panic.check != record_check(&s_panic)) {
        return false;
    }

    out->cause = s_panic.cause;
    out->pc = s_panic.pc;
    out->ra = s_panic.ra;
    out->vaddr = s_panic.vaddr;
    out->sp = s_panic.sp;
    out->image = s_panic.image;
    out->words = (s_panic.words <= AG_PORT_FAULT_STACK_WORDS) ? s_panic.words
                                                             : 0;
    for (uint32_t i = 0; i < out->words; i++) {
        out->stack[i] = s_panic.stack[i];
    }

    /*
     * AND THE RECORD STAYS.  The kernel calls ag_port_fault_panic_reported()
     * once it has written this to /sys/crash.log; until then it is handed over
     * again on every boot.  Clearing it here - which is what this did first -
     * loses the record to any second failure in between, and on the C3 there is
     * one: measured, the board panicked again a second or two after boot, so
     * every boot read the record of the last panic, dropped it, and died before
     * the supervisor could write it.  The crash log stayed empty through a whole
     * loop of panics, which is the exact failure this whole file exists to end.
     */
    return true;
}

void ag_port_fault_panic_reported(void)
{
    s_panic.magic = 0;
    s_panic.check = 0;
}

#endif /* __XTENSA__ */

/* ------------------------------------------------------------------------ */
/* Watching one address                                                       */
/* ------------------------------------------------------------------------ */

/*
 * The same on both architectures - esp_cpu_set_watchpoint is the portable
 * call - so it is written once, below the two halves above.
 *
 * Watchpoint 0: IDF takes number 1 for the end-of-stack watch when
 * CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK is on, and that one is worth more
 * than this one.
 */
#define AG_WATCH_SLOT 0

struct watch_req {
    const void *addr;
    size_t      bytes;
    bool        set;
    bool        ok;
};

static void watch_here(void *arg)
{
    struct watch_req *r = (struct watch_req *)arg;

    if (!r->set) {
        esp_cpu_clear_watchpoint(AG_WATCH_SLOT);
        r->ok = true;
        return;
    }
    r->ok = (esp_cpu_set_watchpoint(AG_WATCH_SLOT, r->addr, r->bytes,
                                    ESP_CPU_WATCHPOINT_STORE) == ESP_OK);
}

static bool watch_on_core(int core, struct watch_req *r)
{
#if portNUM_PROCESSORS > 1
    if (core >= 0 && core != (int)xPortGetCoreID()) {
        if (esp_ipc_call_blocking((uint32_t)core, watch_here, r) != ESP_OK) {
            return false;
        }
        return r->ok;
    }
#else
    (void)core;
#endif
    watch_here(r);
    return r->ok;
}

bool ag_port_watch_write(int core, const void *addr, size_t bytes)
{
    /* The hardware's own rules: a power of two up to 64, aligned to itself. */
    if (addr == NULL || bytes == 0u || bytes > 64u ||
        (bytes & (bytes - 1u)) != 0u ||
        ((uintptr_t)addr & (uintptr_t)(bytes - 1u)) != 0u) {
        return false;
    }
    struct watch_req r = {.addr = addr, .bytes = bytes, .set = true,
                          .ok = false};
    return watch_on_core(core, &r);
}

void ag_port_watch_clear(int core)
{
    struct watch_req r = {.addr = NULL, .bytes = 0, .set = false, .ok = false};
    (void)watch_on_core(core, &r);
}

/* ------------------------------------------------------------------------ */
/* Speaking past the kernel                                                  */
/* ------------------------------------------------------------------------ */

#include <stdarg.h>
#include <stdio.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"

void ag_port_raw_print(const char *fmt, ...)
{
    char    buf[160];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    esp_rom_printf("%s", buf);
}

static esp_timer_handle_t s_lockwatch;
static void (*s_lockwatch_fn)(void);

static void lockwatch_tick(void *arg)
{
    (void)arg;
    if (s_lockwatch_fn != NULL) {
        s_lockwatch_fn();
    }
}

bool ag_port_lockwatch_start(void (*fn)(void), uint32_t period_ms)
{
    if (fn == NULL || period_ms == 0u) {
        return false;
    }
    s_lockwatch_fn = fn;
    if (s_lockwatch == NULL) {
        const esp_timer_create_args_t args = {
            .callback = lockwatch_tick,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lockwatch",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_lockwatch) != ESP_OK) {
            return false;
        }
    } else {
        (void)esp_timer_stop(s_lockwatch);
    }
    return esp_timer_start_periodic(s_lockwatch,
                                    (uint64_t)period_ms * 1000ull) == ESP_OK;
}

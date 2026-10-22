/*
 * ArgonOS - what the standard library does instead of throwing.
 *
 * Link this alongside cxx_rt.cpp in any image that uses the STL, and before
 * -lstdc++.  Everything in libstdc++ that would report a failure reports it by
 * calling one of these; the real ones live in functexcept.o and throw, which
 * drags in __cxa_throw, the unwinder, and the .eh_frame the image discards.
 * Defining them here means the linker never has a reason to pull that object
 * out of the archive, and the whole exception machinery stays out of a 1.5 MB
 * arena.
 *
 * They panic, which is the honest translation: the caller's next line assumes
 * the function did not return, because the declaration says noreturn.  A
 * std::vector that cannot grow has nowhere to go on a machine with no swap and
 * no second chance, and the panic at least names which one it was.
 *
 * The declarations come from the toolchain's own header rather than being
 * written out: these are mangled names in namespace std, and a signature that
 * differs by a const is a link error at best and two definitions at worst.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include <bits/functexcept.h>

namespace {

[[noreturn]] void die(const char *what, const char *detail)
{
    /* Two calls rather than a format: ag_panic takes one string, and building
     * one here would need an allocation, which is exactly what may have just
     * failed. */
    ag_print("C++ std: ");
    ag_print(what);
    if (detail != nullptr) {
        ag_print(": ");
        ag_print(detail);
    }
    ag_print("\n");
    ag_panic(what);
}

}  // namespace

namespace std {

void __throw_bad_exception() { die("bad_exception", nullptr); }
void __throw_bad_alloc() { die("bad_alloc", nullptr); }
void __throw_bad_array_new_length() { die("bad_array_new_length", nullptr); }
void __throw_bad_cast() { die("bad_cast", nullptr); }
void __throw_bad_typeid() { die("bad_typeid", nullptr); }
void __throw_bad_function_call() { die("bad_function_call", nullptr); }

void __throw_logic_error(const char *s) { die("logic_error", s); }
void __throw_domain_error(const char *s) { die("domain_error", s); }
void __throw_invalid_argument(const char *s) { die("invalid_argument", s); }
void __throw_length_error(const char *s) { die("length_error", s); }
void __throw_out_of_range(const char *s) { die("out_of_range", s); }
void __throw_runtime_error(const char *s) { die("runtime_error", s); }
void __throw_range_error(const char *s) { die("range_error", s); }
void __throw_overflow_error(const char *s) { die("overflow_error", s); }
void __throw_underflow_error(const char *s) { die("underflow_error", s); }
void __throw_ios_failure(const char *s) { die("ios_failure", s); }
void __throw_ios_failure(const char *s, int) { die("ios_failure", s); }

/* The variadic one formats the offending index into the message; there is no
 * formatter here that does not allocate, so the format string goes out as it
 * stands and says which container it was. */
void __throw_out_of_range_fmt(const char *fmt, ...) { die("out_of_range", fmt); }

void __throw_system_error(int) { die("system_error", nullptr); }
void __throw_future_error(int) { die("future_error", nullptr); }

}  // namespace std

#ifndef ARGON_SMS_ASSERT_H
#define ARGON_SMS_ASSERT_H

void __assert_func(const char *file, int line, const char *func,
                   const char *expr);

#define assert(x)                                                              \
    ((x) ? (void)0 : __assert_func(__FILE__, __LINE__, __func__, #x))

#endif

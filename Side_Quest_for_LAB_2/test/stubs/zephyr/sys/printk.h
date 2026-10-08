/* Host stand-in: printk goes to stdout, or nowhere when QUIET_PRINTK is defined. */
#ifndef STUB_ZEPHYR_PRINTK_H_
#define STUB_ZEPHYR_PRINTK_H_

#include <stdio.h>

#ifdef QUIET_PRINTK
#define printk(...) do { if (0) { printf(__VA_ARGS__); } } while (0)
#else
#define printk printf
#endif

#endif /* STUB_ZEPHYR_PRINTK_H_ */

/* Host stand-in: printk goes to stdout. */
#ifndef STUB_ZEPHYR_PRINTK_H_
#define STUB_ZEPHYR_PRINTK_H_

#include <stdio.h>

#define printk printf

#endif /* STUB_ZEPHYR_PRINTK_H_ */

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void diag_early_init(void);
void diag_putc(char c);
void diag_puts(const char *s);
void diag_put_hex(uint32_t val);

#ifdef __cplusplus
}
#endif

#endif // DIAGNOSTICS_H

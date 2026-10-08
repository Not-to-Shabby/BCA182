/* Host stand-in: the host tests are single threaded, so masking interrupts is a no-op. */
#ifndef STUB_ZEPHYR_IRQ_H_
#define STUB_ZEPHYR_IRQ_H_

static inline unsigned int irq_lock(void) { return 0; }
static inline void irq_unlock(unsigned int key) { (void)key; }

#endif /* STUB_ZEPHYR_IRQ_H_ */

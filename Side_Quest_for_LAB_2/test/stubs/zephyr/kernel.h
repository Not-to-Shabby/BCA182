/* Host stand-ins for the Zephyr kernel calls used by the catalog, synth and sequencer. */
#ifndef STUB_ZEPHYR_KERNEL_H_
#define STUB_ZEPHYR_KERNEL_H_

#include <stdbool.h>
#include <stdint.h>

struct k_mutex { int unused; };
struct k_timer { int unused; };
struct k_sem { int unused; };

#define K_FOREVER 0
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define K_SEM_DEFINE(name, initial, limit) static struct k_sem name
#define K_THREAD_DEFINE(name, stack, entry, p1, p2, p3, prio, opts, delay) \
    __attribute__((unused)) static void (*name##_entry)(void *, void *, void *) = (entry)

static inline void k_mutex_init(struct k_mutex *m) { (void)m; }
static inline int k_mutex_lock(struct k_mutex *m, int t) { (void)m; (void)t; return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { (void)m; return 0; }
static inline void k_timer_init(struct k_timer *t, void *a, void *b) { (void)t; (void)a; (void)b; }
static inline void k_timer_start(struct k_timer *t, int a, int b) { (void)t; (void)a; (void)b; }
static inline void k_timer_stop(struct k_timer *t) { (void)t; }
static inline int k_sem_take(struct k_sem *s, int t) { (void)s; (void)t; return 0; }
static inline void k_sem_give(struct k_sem *s) { (void)s; }
static inline int64_t k_uptime_ticks(void) { return 0; }
static inline int64_t k_ticks_to_us_floor64(int64_t ticks) { return ticks * 100; }

#endif /* STUB_ZEPHYR_KERNEL_H_ */

/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

/* shim_rt.h -- OURS: the runtime under the rtw88 shim (shim/include/shim_core.h), C-callable, implemented in
 * shim_rt.cpp on nkern (allocation, the big mutex, spin locks, completions, work items + delayed work on the WLAN DFC
 * queue, timers, delays, time, logging) and in a64sdio.cpp (the SDIO function API over SMHC1). Everything the vendored
 * driver needs from "the OS" ends up here. */
#ifndef SHIM_RT_H
#define SHIM_RT_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

struct mutex; struct spinlock; struct completion; struct work_struct; struct delayed_work; struct timer_list;
struct sk_buff; struct firmware;

/* memory */
void *shim_alloc(size_t n);
void *shim_zalloc(size_t n);
void shim_free(void *p);
void *shim_memdup(const void *s, size_t n);
int shim_memcmp(const void *a, const void *b, size_t n);
size_t shim_strlen(const char *s);
int shim_strcmp(const char *a, const char *b);
int shim_strncmp(const char *a, const char *b, size_t n);
char *shim_strncpy(char *d, const char *s, size_t n);
int shim_snprintf(char *buf, size_t n, const char *fmt, ...);
int shim_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void shim_log(const char *fmt, ...);
void shim_hexdump(const char *prefix, const void *buf, size_t len);
void shim_panic(const char *what);
void shim_random_bytes(uint8_t *p, unsigned n);

/* sk_buff */
struct sk_buff *shim_alloc_skb(unsigned size);
void shim_free_skb(struct sk_buff *skb);
struct sk_buff *shim_skb_copy(const struct sk_buff *skb);

/* locks: the mutex is a real (sleeping) kernel mutex, the spin lock a fast mutex (the driver runs on one DFC thread) */
void shim_mutex_init(struct mutex *m);
void shim_mutex_destroy(struct mutex *m);
void shim_mutex_lock(struct mutex *m);
void shim_mutex_unlock(struct mutex *m);
int shim_mutex_trylock(struct mutex *m);
int shim_mutex_is_locked(struct mutex *m);
void shim_spin_init(struct spinlock *l);
void shim_spin_lock(struct spinlock *l);
void shim_spin_unlock(struct spinlock *l);
void shim_completion_init(struct completion *c);
void shim_completion_reinit(struct completion *c);
void shim_complete(struct completion *c);
void shim_complete_all(struct completion *c);
unsigned long shim_wait_for_completion(struct completion *c, unsigned long timeout_jiffies);	/* 0 = timed out */

/* work + timers, all on the WLAN DFC thread */
void shim_work_init(struct work_struct *w, void (*fn)(struct work_struct *));
int shim_queue_work(struct work_struct *w);
int shim_cancel_work(struct work_struct *w);
void shim_flush_work(struct work_struct *w);
void shim_delayed_work_init(struct delayed_work *dw, void (*fn)(struct work_struct *));
int shim_queue_delayed_work(struct delayed_work *dw, unsigned long delay_jiffies);
int shim_cancel_delayed_work(struct delayed_work *dw);
int shim_delayed_work_pending(struct delayed_work *dw);
void shim_timer_setup(struct timer_list *t, void (*fn)(struct timer_list *));
int shim_mod_timer(struct timer_list *t, unsigned long expires_jiffies);
int shim_del_timer(struct timer_list *t);
int shim_timer_pending(struct timer_list *t);

/* time */
void shim_udelay(unsigned us);
void shim_usleep(unsigned us);
void shim_msleep(unsigned ms);
unsigned long shim_jiffies(void);			/* milliseconds since boot, as HZ = 1000 */
int64_t shim_ktime_ns(void);
void *shim_current_thread(void);

/* firmware (handed in by the user side; see a64wlan.cpp) */
int shim_request_firmware(const struct firmware **fw, const char *name);
int shim_request_firmware_nowait(const char *name, void *ctx, void (*cb)(const struct firmware *, void *));
void shim_release_firmware(const struct firmware *fw);

/* the SDIO host (a64sdio.cpp): function 1 of the RTL8723CS; addr = the CMD52/53 register address (17 bits) */
void shim_sdio_claim(void);
void shim_sdio_release(void);
int shim_sdio_enable_func(int on);
int shim_sdio_set_block_size(unsigned bytes);
uint8_t shim_sdio_readb(unsigned addr, int *err);
void shim_sdio_writeb(unsigned addr, uint8_t v, int *err);
int shim_sdio_read_incr(unsigned addr, void *dst, unsigned count);		/* CMD53, incrementing address */
int shim_sdio_write_incr(unsigned addr, const void *src, unsigned count);
int shim_sdio_claim_irq(void);
int shim_sdio_release_irq(void);

#ifdef __cplusplus
}
#endif
#endif

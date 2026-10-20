/* shim_core.h -- OURS (2026-10-10, PinePhone WiFi: the chip core). The freestanding "Linux kernel" the vendored rtw88
 * driver (../rtw88, unmodified, GPL-2.0 OR BSD-3-Clause) compiles against inside our EKA2 kernel: types, the kernel.h
 * helpers, bit/byte-order macros, lists, sk_buffs, locks, work, timers, delays and the SDIO function API -- each a
 * freestanding re-implementation of the Linux *interface* (names and semantics as rtw88 uses them), written from the
 * driver's usage, not copied from Linux headers. Every linux/<x>.h and net/<x>.h under include/ includes this file.
 * The runtime behind the inline parts (allocation, locks, work queues, timers, time, the SDIO host, logging) is
 * shim_rt.h, implemented in shim_rt.cpp on nkern / the A64 SDIO host. docs: pinephone-port.md "WiFi: the chip core". */
#ifndef SHIM_CORE_H
#define SHIM_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "../shim_rt.h"

/* ---- types ---- */
/* Linux widths (u32 = unsigned int, not newlib's unsigned long) so the driver's printf formats stay clean */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
typedef signed char s8; typedef short s16; typedef int s32; typedef long long s64;
typedef u8 __u8; typedef u16 __u16; typedef u32 __u32; typedef u64 __u64;
typedef s8 __s8; typedef s16 __s16; typedef s32 __s32; typedef s64 __s64;
typedef u16 __le16; typedef u32 __le32; typedef u64 __le64;
typedef u16 __be16; typedef u32 __be32; typedef u64 __be64;
typedef u16 __sum16;
typedef int ssize_t;
typedef unsigned int gfp_t;
typedef s64 ktime_t;
typedef u32 dma_addr_t;
typedef struct { int counter; } atomic_t;
typedef unsigned long ulong;
#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define GFP_NOWAIT 2

/* ---- compiler ---- */
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __must_check
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define __always_inline inline __attribute__((always_inline))
#define __force
#define __iomem
#define __user
#define __init
#define __exit
#define __read_mostly
#define __cold
#define __rcu
#define __bitwise
#define __attribute_const__ __attribute__((const))
#define __deprecated
#define __nonstring
#define __pure __attribute__((pure))
#define noinline __attribute__((noinline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define fallthrough __attribute__((fallthrough))
#define barrier() __asm__ __volatile__("" : : : "memory")
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#ifndef typeof
#define typeof __typeof__
#endif
#define __stringify_1(x) #x
#define __stringify(x) __stringify_1(x)
#define __CONCAT_(a, b) a##b
#define __CONCAT(a, b) __CONCAT_(a, b)
#define __UNIQUE_ID(prefix) __CONCAT(__CONCAT(__UNIQUE_ID_, prefix), __COUNTER__)
#define static_assert(e, ...) _Static_assert(e, "static assertion")
#define BUILD_BUG_ON(c) ((void)sizeof(char[1 - 2 * !!(c)]))
#define BUILD_BUG_ON_MSG(c, m) BUILD_BUG_ON(c)
#define BUILD_BUG_ON_ZERO(e) ((int)(sizeof(struct { int : (-!!(e)); })))

/* ---- kernel.h ---- */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BITS_PER_LONG 32
#define BITS_PER_LONG_LONG 64
#define BITS_PER_BYTE 8
#define BITS_PER_TYPE(t) (sizeof(t) * BITS_PER_BYTE)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define U8_MAX 0xFFu
#define U16_MAX 0xFFFFu
#define U32_MAX 0xFFFFFFFFu
#define S8_MAX 127
#define S8_MIN (-128)
#define S16_MAX 32767
#define S32_MAX 2147483647
#define INT_MAX S32_MAX
#define UINT_MAX U32_MAX
#define SZ_1K 0x400
#define SZ_4K 0x1000
#define SZ_8K 0x2000
#define SZ_16K 0x4000
#define SZ_32K 0x8000
#define SZ_64K 0x10000
#define SZ_128K 0x20000
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })
#define min3(a, b, c) min(min(a, b), c)
#define max3(a, b, c) max(max(a, b), c)
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define clamp_val(v, lo, hi) clamp_t(__typeof__(v), v, lo, hi)
#define abs(x) ({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })
#define swap(a, b) do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) (((x) / (y)) * (y))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a) ((__typeof__(p))ALIGN((unsigned long)(p), (a)))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define offsetofend(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(a, b)
#define MODULE_PARM_DESC(a, b)
#define module_param_named(a, b, c, d)
#define THIS_MODULE ((void *)0)
#define KBUILD_MODNAME "rtw88"
#define IS_ENABLED(x) 0
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(long)(e))
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define might_sleep() ((void)0)
#define cond_resched() ((void)0)
#define HZ 1000
#define jiffies shim_jiffies()
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define usecs_to_jiffies(us) ((unsigned long)DIV_ROUND_UP((us), 1000))
#define round_jiffies_relative(j) (j)
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_is_before_jiffies(a) time_after(jiffies, a)
#define WARN(cond, fmt, ...) ({ int _w = !!(cond); if (_w) shim_log("rtw88: WARN %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); _w; })
#define WARN_ON(cond) ({ int _w = !!(cond); if (_w) shim_log("rtw88: WARN_ON %s:%d\n", __FILE__, __LINE__); _w; })
#define WARN_ON_ONCE(cond) WARN_ON(cond)
#define WARN_ONCE(cond, fmt, ...) WARN(cond, fmt, ##__VA_ARGS__)
#define BUG() shim_panic("rtw88 BUG " __FILE__)
#define BUG_ON(c) do { if (c) BUG(); } while (0)
#define printk(fmt, ...) shim_log(fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) ((void)0)
#define KERN_ERR
#define KERN_WARNING
#define KERN_INFO
#define KERN_DEBUG
#define DUMP_PREFIX_OFFSET 0
#define print_hex_dump(a, b, c, d, e, buf, len, f) shim_hexdump(b, buf, len)
#define print_hex_dump_bytes(p, t, buf, len) shim_hexdump(p, buf, len)

/* ---- errno ---- */
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define ENXIO 6
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define ENOSPC 28
#define ERANGE 34
#define ENOSYS 38
#define ETIMEDOUT 110
#define EALREADY 114
#define EINPROGRESS 115
#define ENOTSUPP 524
#define EOPNOTSUPP 95
#define EILSEQ 84

/* ---- string (memcpy/memset/memmove come from the kernel, the rest from shim_rt) ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
#define memcmp shim_memcmp
#define strlen shim_strlen
#define strcmp shim_strcmp
#define strncmp shim_strncmp
#define strncpy shim_strncpy
#define strscpy(d, s, n) shim_strncpy(d, s, n)
#define snprintf shim_snprintf
#define scnprintf shim_snprintf
#define sprintf(buf, ...) shim_snprintf(buf, 256, __VA_ARGS__)
#define kstrtou32(s, b, r) (-EINVAL)

/* ---- byte order (the A64 runs little-endian) ---- */
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define __cpu_to_le16 cpu_to_le16
#define __cpu_to_le32 cpu_to_le32
#define __le16_to_cpu le16_to_cpu
#define __le32_to_cpu le32_to_cpu
#define __le64_to_cpu le64_to_cpu
#define le16_to_cpup(p) le16_to_cpu(*(p))
#define le32_to_cpup(p) le32_to_cpu(*(p))
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define be16_to_cpu(x) __builtin_bswap16((u16)(x))
#define be32_to_cpu(x) __builtin_bswap32((u32)(x))
#define __cpu_to_be16 cpu_to_be16
#define __be16_to_cpu be16_to_cpu
#define swab16(x) __builtin_bswap16((u16)(x))
#define swab32(x) __builtin_bswap32((u32)(x))
static inline u16 get_unaligned_le16(const void *p) { const u8 *b = (const u8 *)p; return (u16)(b[0] | (b[1] << 8)); }
static inline u32 get_unaligned_le32(const void *p) { const u8 *b = (const u8 *)p; return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24); }
static inline u16 get_unaligned_be16(const void *p) { const u8 *b = (const u8 *)p; return (u16)((b[0] << 8) | b[1]); }
static inline void put_unaligned_le16(u16 v, void *p) { u8 *b = (u8 *)p; b[0] = (u8)v; b[1] = (u8)(v >> 8); }
static inline void put_unaligned_le32(u32 v, void *p) { u8 *b = (u8 *)p; b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); b[3] = (u8)(v >> 24); }
#define get_unaligned(p) (*(p))
#define put_unaligned(v, p) (*(p) = (v))

/* ---- bits ---- */
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define BIT_MASK(n) (1UL << ((n) % BITS_PER_LONG))
#define BIT_WORD(n) ((n) / BITS_PER_LONG)
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#define __bf_shf(m) (__builtin_ffsll(m) - 1)
#define FIELD_PREP(mask, val) (((__typeof__(mask))(val) << __bf_shf(mask)) & (mask))
#define FIELD_PREP_CONST(mask, val) FIELD_PREP(mask, val)
#define FIELD_GET(mask, reg) ((__typeof__(mask))(((reg) & (mask)) >> __bf_shf(mask)))
#define FIELD_MAX(mask) ((mask) >> __bf_shf(mask))
#define FIELD_FIT(mask, val) (!((((u64)(val)) << __bf_shf(mask)) & ~((u64)(mask))))
static inline u8 u8_get_bits(u8 v, u8 m) { return (u8)((v & m) >> __bf_shf(m)); }
static inline u16 u16_get_bits(u16 v, u16 m) { return (u16)((v & m) >> __bf_shf(m)); }
static inline u32 u32_get_bits(u32 v, u32 m) { return (v & m) >> __bf_shf(m); }
static inline u64 u64_get_bits(u64 v, u64 m) { return (v & m) >> __bf_shf(m); }
static inline u8 u8_encode_bits(u8 v, u8 m) { return (u8)((v << __bf_shf(m)) & m); }
static inline u16 u16_encode_bits(u16 v, u16 m) { return (u16)((v << __bf_shf(m)) & m); }
static inline u32 u32_encode_bits(u32 v, u32 m) { return (v << __bf_shf(m)) & m; }
static inline u64 u64_encode_bits(u64 v, u64 m) { return (v << __bf_shf(m)) & m; }
static inline s32 sign_extend32(u32 v, int index) { u8 shift = 31 - index; return (s32)(v << shift) >> shift; }
static inline void u32p_replace_bits(u32 *p, u32 v, u32 m) { *p = (*p & ~m) | ((v << __bf_shf(m)) & m); }
static inline void u8p_replace_bits(u8 *p, u8 v, u8 m) { *p = (u8)((*p & ~m) | ((v << __bf_shf(m)) & m)); }
static inline u16 le16_get_bits(__le16 v, u16 m) { return (u16)((le16_to_cpu(v) & m) >> __bf_shf(m)); }
static inline u32 le32_get_bits(__le32 v, u32 m) { return (le32_to_cpu(v) & m) >> __bf_shf(m); }
static inline u64 le64_get_bits(__le64 v, u64 m) { return ((u64)(v) & m) >> __bf_shf(m); }	/* coex.h (W4) */
static inline __le16 le16_encode_bits(u16 v, u16 m) { return cpu_to_le16((v << __bf_shf(m)) & m); }
static inline __le32 le32_encode_bits(u32 v, u32 m) { return cpu_to_le32((v << __bf_shf(m)) & m); }
static inline void le32p_replace_bits(__le32 *p, u32 v, u32 m) { *p = cpu_to_le32((le32_to_cpu(*p) & ~m) | ((v << __bf_shf(m)) & m)); }
static inline void le16p_replace_bits(__le16 *p, u16 v, u16 m) { *p = cpu_to_le16((le16_to_cpu(*p) & ~m) | ((v << __bf_shf(m)) & m)); }
static inline unsigned long __ffs(unsigned long x) { return (unsigned long)__builtin_ctzl(x); }
static inline int ffs(int x) { return __builtin_ffs(x); }
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned int hweight8(u8 x) { return (unsigned int)__builtin_popcount(x); }
static inline unsigned int hweight16(u16 x) { return (unsigned int)__builtin_popcount(x); }
static inline unsigned int hweight32(u32 x) { return (unsigned int)__builtin_popcount(x); }
static inline unsigned long hweight_long(unsigned long x) { return (unsigned long)__builtin_popcountl(x); }
static inline int ilog2(unsigned long x) { return x ? 31 - __builtin_clzl(x) : -1; }
static inline unsigned long roundup_pow_of_two(unsigned long x) { return 1UL << fls((unsigned int)(x - 1)); }
static inline u32 rol32(u32 w, unsigned s) { return (w << s) | (w >> (32 - s)); }
static inline u32 ror32(u32 w, unsigned s) { return (w >> s) | (w << (32 - s)); }
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline void set_bit(unsigned int n, volatile unsigned long *a) { a[BIT_WORD(n)] |= BIT_MASK(n); }
static inline void clear_bit(unsigned int n, volatile unsigned long *a) { a[BIT_WORD(n)] &= ~BIT_MASK(n); }
static inline void change_bit(unsigned int n, volatile unsigned long *a) { a[BIT_WORD(n)] ^= BIT_MASK(n); }
static inline int test_bit(unsigned int n, const volatile unsigned long *a) { return (a[BIT_WORD(n)] & BIT_MASK(n)) != 0; }
static inline int test_and_set_bit(unsigned int n, volatile unsigned long *a) { int o = test_bit(n, a); set_bit(n, a); return o; }
static inline int test_and_clear_bit(unsigned int n, volatile unsigned long *a) { int o = test_bit(n, a); clear_bit(n, a); return o; }
#define __set_bit set_bit
#define __clear_bit clear_bit
#define __test_and_set_bit test_and_set_bit
#define __test_and_clear_bit test_and_clear_bit
static inline void bitmap_zero(unsigned long *a, unsigned int bits) { memset(a, 0, BITS_TO_LONGS(bits) * sizeof(long)); }
static inline void bitmap_fill(unsigned long *a, unsigned int bits) { memset(a, 0xff, BITS_TO_LONGS(bits) * sizeof(long)); }
static inline unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off)
	{ for (; off < size; ++off) if (test_bit((unsigned int)off, a)) return off; return size; }
static inline unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off)
	{ for (; off < size; ++off) if (!test_bit((unsigned int)off, a)) return off; return size; }
#define find_first_bit(a, size) find_next_bit(a, size, 0)
#define find_first_zero_bit(a, size) find_next_zero_bit(a, size, 0)
static inline unsigned long find_last_bit(const unsigned long *a, unsigned long size)
	{ unsigned long i = size; while (i--) if (test_bit((unsigned int)i, a)) return i; return size; }
#define for_each_set_bit(bit, addr, size) for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
static inline int bitmap_empty(const unsigned long *a, unsigned int bits) { return find_first_bit(a, bits) == bits; }
static inline unsigned int bitmap_weight(const unsigned long *a, unsigned int bits) { unsigned int i, n = 0; for (i = 0; i < bits; ++i) n += test_bit(i, a); return n; }

/* ---- atomic (the driver runs on one DFC thread; these are plain) ---- */
#define ATOMIC_INIT(i) { (i) }
static inline int atomic_read(const atomic_t *v) { return READ_ONCE(v->counter); }
static inline void atomic_set(atomic_t *v, int i) { WRITE_ONCE(v->counter, i); }
static inline void atomic_inc(atomic_t *v) { __sync_fetch_and_add(&v->counter, 1); }
static inline void atomic_dec(atomic_t *v) { __sync_fetch_and_sub(&v->counter, 1); }
static inline int atomic_inc_return(atomic_t *v) { return __sync_add_and_fetch(&v->counter, 1); }
static inline int atomic_dec_return(atomic_t *v) { return __sync_sub_and_fetch(&v->counter, 1); }
static inline int atomic_add_return(int i, atomic_t *v) { return __sync_add_and_fetch(&v->counter, i); }
static inline int atomic_dec_and_test(atomic_t *v) { return __sync_sub_and_fetch(&v->counter, 1) == 0; }

/* ---- lists ---- */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l; l->prev = l; }
static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next)
	{ next->prev = n; n->next = next; n->prev = prev; prev->next = n; }
static inline void list_add(struct list_head *n, struct list_head *head) { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *head) { __list_add(n, head->prev, head); }
static inline void __list_del(struct list_head *prev, struct list_head *next) { next->prev = prev; prev->next = next; }
static inline void list_del(struct list_head *e) { __list_del(e->prev, e->next); e->next = (struct list_head *)0; e->prev = (struct list_head *)0; }
static inline void list_del_init(struct list_head *e) { __list_del(e->prev, e->next); INIT_LIST_HEAD(e); }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
static inline int list_is_last(const struct list_head *e, const struct list_head *h) { return e->next == h; }
static inline void list_move(struct list_head *e, struct list_head *h) { __list_del(e->prev, e->next); list_add(e, h); }
static inline void list_move_tail(struct list_head *e, struct list_head *h) { __list_del(e->prev, e->next); list_add_tail(e, h); }
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_next_entry(pos, member) list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_first_entry_or_null(ptr, type, member) ({ struct list_head *_h = (ptr); struct list_head *_p = _h->next; _p != _h ? list_entry(_p, type, member) : (type *)0; })
#define list_for_each_entry(pos, head, member) for (pos = list_first_entry(head, __typeof__(*pos), member); &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) for (pos = list_first_entry(head, __typeof__(*pos), member), n = list_next_entry(pos, member); &pos->member != (head); pos = n, n = list_next_entry(n, member))
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)

/* ---- locks, completion, work, timers (the objects are opaque nkern things created by shim_rt) ---- */
struct mutex { void *k; };
#define DEFINE_MUTEX(m) struct mutex m = { 0 }
#define mutex_init(m) shim_mutex_init(m)
#define mutex_destroy(m) shim_mutex_destroy(m)
#define mutex_lock(m) shim_mutex_lock(m)
#define mutex_unlock(m) shim_mutex_unlock(m)
#define mutex_trylock(m) shim_mutex_trylock(m)
#define mutex_is_locked(m) shim_mutex_is_locked(m)
#define lockdep_assert_held(m) ((void)0)
#define lockdep_assert_wiphy(w) ((void)0)
#define lockdep_is_held(m) 1
typedef struct spinlock { void *k; } spinlock_t;
#define DEFINE_SPINLOCK(l) spinlock_t l = { 0 }
#define spin_lock_init(l) shim_spin_init(l)
#define spin_lock(l) shim_spin_lock(l)
#define spin_unlock(l) shim_spin_unlock(l)
#define spin_lock_bh(l) shim_spin_lock(l)
#define spin_unlock_bh(l) shim_spin_unlock(l)
#define spin_lock_irq(l) shim_spin_lock(l)
#define spin_unlock_irq(l) shim_spin_unlock(l)
#define spin_lock_irqsave(l, flags) do { (flags) = 0; shim_spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { (void)(flags); shim_spin_unlock(l); } while (0)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define rcu_dereference(p) (p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_assign_pointer(p, v) ((p) = (v))
#define RCU_INIT_POINTER(p, v) ((p) = (v))
#define synchronize_rcu() ((void)0)
struct completion { void *k; };
/* wait queues: rtw88 only uses wait_event_timeout + wake_up on one (coex); polled */
typedef struct wait_queue_head { int wakeups; } wait_queue_head_t;
#define init_waitqueue_head(q) ((q)->wakeups = 0)
#define wake_up(q) ((q)->wakeups++)
#define init_completion(c) shim_completion_init(c)
#define reinit_completion(c) shim_completion_reinit(c)
#define complete(c) shim_complete(c)
#define complete_all(c) shim_complete_all(c)
#define wait_for_completion(c) ((void)shim_wait_for_completion(c, 0))
#define wait_for_completion_timeout(c, j) shim_wait_for_completion(c, (unsigned long)(j))
#define wait_for_completion_interruptible_timeout(c, j) ((long)shim_wait_for_completion(c, (unsigned long)(j)))
struct work_struct { void (*func)(struct work_struct *); void *k; };
typedef void (*work_func_t)(struct work_struct *);
struct delayed_work { struct work_struct work; void *t; };
struct workqueue_struct;
#define INIT_WORK(w, f) shim_work_init(w, f)
#define INIT_DELAYED_WORK(dw, f) shim_delayed_work_init(dw, f)
#define queue_work(wq, w) shim_queue_work(w)
#define schedule_work(w) shim_queue_work(w)
#define queue_delayed_work(wq, dw, d) shim_queue_delayed_work(dw, (unsigned long)(d))
#define schedule_delayed_work(dw, d) shim_queue_delayed_work(dw, (unsigned long)(d))
#define mod_delayed_work(wq, dw, d) shim_queue_delayed_work(dw, (unsigned long)(d))
#define cancel_work_sync(w) shim_cancel_work(w)
#define cancel_work(w) shim_cancel_work(w)
#define flush_work(w) shim_flush_work(w)
#define cancel_delayed_work(dw) shim_cancel_delayed_work(dw)
#define cancel_delayed_work_sync(dw) shim_cancel_delayed_work(dw)
#define flush_delayed_work(dw) shim_cancel_delayed_work(dw)
#define delayed_work_pending(dw) shim_delayed_work_pending(dw)
#define work_pending(w) 0
#define alloc_workqueue(name, flags, max) ((struct workqueue_struct *)1)
#define alloc_ordered_workqueue(name, flags) ((struct workqueue_struct *)1)
#define create_singlethread_workqueue(name) ((struct workqueue_struct *)1)
#define destroy_workqueue(wq) ((void)0)
#define flush_workqueue(wq) ((void)0)
#define WQ_UNBOUND 0
#define WQ_HIGHPRI 0
static inline struct delayed_work *to_delayed_work(struct work_struct *w) { return container_of(w, struct delayed_work, work); }
struct timer_list { void (*function)(struct timer_list *); void *k; unsigned long expires; };
#define timer_setup(t, fn, flags) shim_timer_setup(t, fn)
#define mod_timer(t, exp) shim_mod_timer(t, (unsigned long)(exp))
#define del_timer(t) shim_del_timer(t)
#define del_timer_sync(t) shim_del_timer(t)
#define timer_delete(t) shim_del_timer(t)
#define timer_delete_sync(t) shim_del_timer(t)
#define timer_pending(t) shim_timer_pending(t)
#define from_timer(var, t, field) container_of(t, __typeof__(*var), field)
#define timer_container_of(var, t, field) container_of(t, __typeof__(*var), field)
#define udelay(us) shim_udelay((unsigned)(us))
#define ndelay(ns) shim_udelay(1)
#define mdelay(ms) shim_udelay(1000u * (unsigned)(ms))
#define msleep(ms) shim_msleep((unsigned)(ms))
#define msleep_interruptible(ms) (shim_msleep((unsigned)(ms)), 0)
#define usleep_range(lo, hi) shim_usleep((unsigned)(lo))
#define fsleep(us) shim_usleep((unsigned)(us))
#define ktime_get() ((ktime_t)shim_ktime_ns())
#define ktime_get_boottime() ((ktime_t)shim_ktime_ns())
#define ktime_to_us(k) ((s64)(k) / 1000)
#define ktime_to_ms(k) ((s64)(k) / 1000000)
#define ktime_us_delta(a, b) (((s64)(a) - (s64)(b)) / 1000)
#define ktime_ms_delta(a, b) (((s64)(a) - (s64)(b)) / 1000000)
#define ktime_sub(a, b) ((ktime_t)((a) - (b)))
#define ktime_add_us(k, us) ((ktime_t)((k) + (s64)(us) * 1000))
#define ktime_add_ms(k, ms) ((ktime_t)((k) + (s64)(ms) * 1000000))
#define ktime_after(a, b) ((a) > (b))
#define ktime_before(a, b) ((a) < (b))
#define ktime_compare(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
/* iopoll: poll op(args) into val until cond, sleeping sleep_us between reads, giving up after timeout_us */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before, args...) ({ \
	ktime_t _to = ktime_add_us(ktime_get(), timeout_us); int _ret = 0; \
	if (sleep_before) usleep_range(sleep_us, sleep_us); \
	for (;;) { (val) = op(args); if (cond) break; \
		if ((timeout_us) && ktime_after(ktime_get(), _to)) { (val) = op(args); if (!(cond)) _ret = -ETIMEDOUT; break; } \
		if (sleep_us) usleep_range(sleep_us, sleep_us); } _ret; })
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, delay_before, args...) ({ \
	ktime_t _to = ktime_add_us(ktime_get(), timeout_us); int _ret = 0; \
	if (delay_before) udelay(delay_us); \
	for (;;) { (val) = op(args); if (cond) break; \
		if ((timeout_us) && ktime_after(ktime_get(), _to)) { (val) = op(args); if (!(cond)) _ret = -ETIMEDOUT; break; } \
		if (delay_us) udelay(delay_us); } _ret; })
#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)

/* ---- memory ---- */
#define kmalloc(sz, gfp) shim_alloc((size_t)(sz))
#define kzalloc(sz, gfp) shim_zalloc((size_t)(sz))
#define kcalloc(n, sz, gfp) shim_zalloc((size_t)(n) * (size_t)(sz))
#define kmalloc_array(n, sz, gfp) shim_alloc((size_t)(n) * (size_t)(sz))
#define kfree(p) shim_free((void *)(p))
#define kvfree(p) shim_free((void *)(p))
#define vmalloc(sz) shim_alloc((size_t)(sz))
#define vzalloc(sz) shim_zalloc((size_t)(sz))
#define vfree(p) shim_free((void *)(p))
/* kmalloc_obj(*ptr[, gfp]) = an object of the pointee's type */
#define kmalloc_obj(e, ...) ((__typeof__(e) *)shim_alloc(sizeof(e)))
#define kzalloc_obj(e, ...) ((__typeof__(e) *)shim_zalloc(sizeof(e)))
#define kmemdup(s, n, gfp) shim_memdup(s, (size_t)(n))
#define kstrdup(s, gfp) ((char *)shim_memdup(s, shim_strlen(s) + 1))
#define devm_kzalloc(d, sz, gfp) shim_zalloc((size_t)(sz))
#define devm_kfree(d, p) shim_free((void *)(p))
#define devm_kmemdup(d, s, n, gfp) shim_memdup(s, (size_t)(n))
#define devm_kmemdup_array(d, s, n, sz, gfp) shim_memdup(s, (size_t)(n) * (size_t)(sz))
#define dev_coredumpv(d, data, len, gfp) shim_free((void *)(data))

/* ---- average.h: DECLARE_EWMA(name, precision, weight_rcp), Linux's exponentially weighted moving average ---- */
#define DECLARE_EWMA(name, _precision, _weight_rcp) \
	struct ewma_##name { unsigned long internal; }; \
	static inline void ewma_##name##_init(struct ewma_##name *e) { e->internal = 0; } \
	static inline unsigned long ewma_##name##_read(struct ewma_##name *e) { return e->internal >> (_precision); } \
	static inline void ewma_##name##_add(struct ewma_##name *e, unsigned long val) { \
		unsigned long internal = READ_ONCE(e->internal); unsigned long weight_rcp = ilog2(_weight_rcp); unsigned long precision = (_precision); \
		WRITE_ONCE(e->internal, internal ? (((internal << weight_rcp) - internal) + (val << precision)) >> weight_rcp : (val << precision)); }

/* ---- bcd ---- */
static inline unsigned bcd2bin(u8 v) { return (v & 0x0f) + (v >> 4) * 10; }
static inline u8 bin2bcd(unsigned v) { return (u8)(((v / 10) << 4) + v % 10); }

/* ---- device, firmware, pm, leds, of ---- */
struct device { void *driver_data; void *of_node; const char *name; };
#define dev_err(d, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define dev_warn(d, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define dev_info(d, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define dev_notice(d, fmt, ...) shim_log("rtw88: " fmt, ##__VA_ARGS__)
#define dev_dbg(d, fmt, ...) ((void)0)
#define dev_err_ratelimited dev_err
#define dev_warn_ratelimited dev_warn
static inline void *dev_get_drvdata(const struct device *d) { return d->driver_data; }
static inline void dev_set_drvdata(struct device *d, void *p) { d->driver_data = p; }
static inline const char *dev_name(const struct device *d) { return d->name ? d->name : "sdio"; }
struct firmware { size_t size; const u8 *data; void *priv; };
#define request_firmware(fw, name, dev) shim_request_firmware(fw, name)
#define request_firmware_nowait(mod, uevent, name, dev, gfp, ctx, cb) shim_request_firmware_nowait(name, ctx, cb)
#define release_firmware(fw) shim_release_firmware(fw)
struct dev_pm_ops { int (*suspend)(struct device *); int (*resume)(struct device *); };
#define SIMPLE_DEV_PM_OPS(name, s, r) const struct dev_pm_ops name = { s, r }
#define DEFINE_SIMPLE_DEV_PM_OPS SIMPLE_DEV_PM_OPS
enum led_brightness { LED_OFF = 0, LED_ON = 1, LED_HALF = 127, LED_FULL = 255 };
struct led_classdev { const char *name; int brightness; int max_brightness; void (*brightness_set)(struct led_classdev *, enum led_brightness); unsigned long flags; const char *default_trigger; };
static inline int of_get_mac_address(void *np, u8 *mac) { (void)np; (void)mac; return -ENODEV; }

/* ---- ethernet addresses ---- */
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_P_PAE 0x888E
#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_IPV6 0x86DD
struct ethhdr { u8 h_dest[6]; u8 h_source[6]; __be16 h_proto; } __packed;
static inline void ether_addr_copy(u8 *d, const u8 *s) { memcpy(d, s, 6); }
static inline bool ether_addr_equal(const u8 *a, const u8 *b) { return shim_memcmp(a, b, 6) == 0; }
static inline bool is_zero_ether_addr(const u8 *a) { return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]); }
static inline bool is_broadcast_ether_addr(const u8 *a) { return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff; }
static inline bool is_multicast_ether_addr(const u8 *a) { return a[0] & 0x01; }
static inline bool is_valid_ether_addr(const u8 *a) { return !is_multicast_ether_addr(a) && !is_zero_ether_addr(a); }
static inline void eth_broadcast_addr(u8 *a) { memset(a, 0xff, 6); }
static inline void eth_zero_addr(u8 *a) { memset(a, 0, 6); }
static inline void eth_random_addr(u8 *a) { shim_random_bytes(a, 6); a[0] &= 0xfe; a[0] |= 0x02; }
static inline void get_random_bytes(void *p, int n) { shim_random_bytes((u8 *)p, (unsigned)n); }
static inline u32 get_random_u32(void) { u32 v; shim_random_bytes((u8 *)&v, 4); return v; }
static inline u8 get_random_u8(void) { u8 v; shim_random_bytes(&v, 1); return v; }
static inline void get_random_mask_addr(u8 *dst, const u8 *addr, const u8 *mask)
	{ int i; eth_random_addr(dst); for (i = 0; i < 6; ++i) dst[i] = (dst[i] & ~mask[i]) | (addr[i] & mask[i]); }

/* ---- sk_buff (head..end = the buffer; data..tail = the payload; cb = the per-stack control block) ---- */
struct sk_buff {
	struct sk_buff *next, *prev;
	u8 *head, *data, *tail, *end;
	unsigned int len;
	char cb[48] __aligned(8);
	u16 queue_mapping;
	u16 protocol;
	u32 priority;
	void *dev;
};
struct sk_buff_head { struct sk_buff *next, *prev; u32 qlen; spinlock_t lock; };
#define alloc_skb(sz, gfp) shim_alloc_skb((unsigned)(sz))
#define dev_alloc_skb(sz) shim_alloc_skb((unsigned)(sz))
#define __dev_alloc_skb(sz, gfp) shim_alloc_skb((unsigned)(sz))
#define netdev_alloc_skb(d, sz) shim_alloc_skb((unsigned)(sz))
#define kfree_skb(skb) shim_free_skb(skb)
#define dev_kfree_skb(skb) shim_free_skb(skb)
#define dev_kfree_skb_any(skb) shim_free_skb(skb)
#define dev_kfree_skb_irq(skb) shim_free_skb(skb)
#define consume_skb(skb) shim_free_skb(skb)
static inline unsigned int skb_headroom(const struct sk_buff *s) { return (unsigned int)(s->data - s->head); }
static inline unsigned int skb_tailroom(const struct sk_buff *s) { return (unsigned int)(s->end - s->tail); }
static inline void *skb_put(struct sk_buff *s, unsigned int n) { void *p = s->tail; if (n > skb_tailroom(s)) shim_panic("skb_put"); s->tail += n; s->len += n; return p; }
static inline void *skb_put_zero(struct sk_buff *s, unsigned int n) { void *p = skb_put(s, n); memset(p, 0, n); return p; }
static inline void *skb_put_data(struct sk_buff *s, const void *d, unsigned int n) { void *p = skb_put(s, n); memcpy(p, d, n); return p; }
static inline void skb_put_u8(struct sk_buff *s, u8 v) { *(u8 *)skb_put(s, 1) = v; }
static inline void *skb_push(struct sk_buff *s, unsigned int n) { if (n > skb_headroom(s)) shim_panic("skb_push"); s->data -= n; s->len += n; return s->data; }
static inline void *skb_pull(struct sk_buff *s, unsigned int n) { if (n > s->len) return (void *)0; s->data += n; s->len -= n; return s->data; }
static inline void skb_reserve(struct sk_buff *s, int n) { s->data += n; s->tail += n; }
static inline void skb_trim(struct sk_buff *s, unsigned int len) { if (s->len > len) { s->len = len; s->tail = s->data + len; } }
static inline void __skb_set_length(struct sk_buff *s, unsigned int len) { s->len = len; s->tail = s->data + len; }
static inline u8 *skb_tail_pointer(const struct sk_buff *s) { return s->tail; }
static inline u16 skb_get_queue_mapping(const struct sk_buff *s) { return s->queue_mapping; }
static inline void skb_set_queue_mapping(struct sk_buff *s, u16 q) { s->queue_mapping = q; }
static inline void skb_copy_header(struct sk_buff *n, const struct sk_buff *o) { memcpy(n->cb, o->cb, sizeof(n->cb)); n->queue_mapping = o->queue_mapping; n->priority = o->priority; n->protocol = o->protocol; n->dev = o->dev; }
#define skb_copy(skb, gfp) shim_skb_copy(skb)
#define skb_clone(skb, gfp) shim_skb_copy(skb)
static inline void __skb_queue_head_init(struct sk_buff_head *q) { q->next = q->prev = (struct sk_buff *)q; q->qlen = 0; }
static inline void skb_queue_head_init(struct sk_buff_head *q) { __skb_queue_head_init(q); spin_lock_init(&q->lock); }
static inline int skb_queue_empty(const struct sk_buff_head *q) { return q->next == (const struct sk_buff *)q; }
static inline u32 skb_queue_len(const struct sk_buff_head *q) { return q->qlen; }
static inline struct sk_buff *skb_peek(const struct sk_buff_head *q) { struct sk_buff *s = q->next; return s == (const struct sk_buff *)q ? (struct sk_buff *)0 : s; }
static inline struct sk_buff *skb_peek_tail(const struct sk_buff_head *q) { struct sk_buff *s = q->prev; return s == (const struct sk_buff *)q ? (struct sk_buff *)0 : s; }
static inline void __skb_insert(struct sk_buff *n, struct sk_buff *prev, struct sk_buff *next, struct sk_buff_head *q)
	{ n->next = next; n->prev = prev; next->prev = n; prev->next = n; q->qlen++; }
static inline void __skb_queue_tail(struct sk_buff_head *q, struct sk_buff *n) { __skb_insert(n, q->prev, (struct sk_buff *)q, q); }
static inline void __skb_queue_head(struct sk_buff_head *q, struct sk_buff *n) { __skb_insert(n, (struct sk_buff *)q, q->next, q); }
static inline void __skb_unlink(struct sk_buff *s, struct sk_buff_head *q) { struct sk_buff *n = s->next, *p = s->prev; q->qlen--; n->prev = p; p->next = n; s->next = s->prev = (struct sk_buff *)0; }
static inline struct sk_buff *__skb_dequeue(struct sk_buff_head *q) { struct sk_buff *s = skb_peek(q); if (s) __skb_unlink(s, q); return s; }
static inline struct sk_buff *__skb_dequeue_tail(struct sk_buff_head *q) { struct sk_buff *s = skb_peek_tail(q); if (s) __skb_unlink(s, q); return s; }
static inline void skb_queue_tail(struct sk_buff_head *q, struct sk_buff *n) { spin_lock(&q->lock); __skb_queue_tail(q, n); spin_unlock(&q->lock); }
static inline void skb_queue_head(struct sk_buff_head *q, struct sk_buff *n) { spin_lock(&q->lock); __skb_queue_head(q, n); spin_unlock(&q->lock); }
static inline void skb_unlink(struct sk_buff *s, struct sk_buff_head *q) { spin_lock(&q->lock); __skb_unlink(s, q); spin_unlock(&q->lock); }
static inline struct sk_buff *skb_dequeue(struct sk_buff_head *q) { struct sk_buff *s; spin_lock(&q->lock); s = __skb_dequeue(q); spin_unlock(&q->lock); return s; }
static inline struct sk_buff *skb_dequeue_tail(struct sk_buff_head *q) { struct sk_buff *s; spin_lock(&q->lock); s = __skb_dequeue_tail(q); spin_unlock(&q->lock); return s; }
static inline void __skb_queue_purge(struct sk_buff_head *q) { struct sk_buff *s; while ((s = __skb_dequeue(q)) != (struct sk_buff *)0) shim_free_skb(s); }
static inline void skb_queue_purge(struct sk_buff_head *q) { struct sk_buff *s; while ((s = skb_dequeue(q)) != (struct sk_buff *)0) shim_free_skb(s); }
#define skb_queue_walk(q, skb) for (skb = (q)->next; skb != (struct sk_buff *)(q); skb = skb->next)
#define skb_queue_walk_safe(q, skb, tmp) for (skb = (q)->next, tmp = skb->next; skb != (struct sk_buff *)(q); skb = tmp, tmp = skb->next)
#define skb_queue_splice_tail_init(from, to) do { struct sk_buff *_s; while ((_s = __skb_dequeue(from)) != 0) __skb_queue_tail(to, _s); } while (0)

/* ---- SDIO: the Linux function API over the A64 SMHC1 host (shim_rt / a64sdio) ---- */
struct mmc_host { unsigned int max_req_size; unsigned int max_blk_size; unsigned int max_blk_count; u32 caps; };
struct mmc_card { struct mmc_host *host; };
struct sdio_func { struct mmc_card *card; unsigned int num; unsigned int cur_blksize; unsigned int max_blksize; struct device dev; void *drvdata; void (*irq_handler)(struct sdio_func *); };
struct sdio_device_id { u8 class; u16 vendor; u16 device; unsigned long driver_data; };
#define SDIO_ANY_ID (~0)
#define SDIO_DEVICE(vend, dev) .class = 0, .vendor = (vend), .device = (dev)
#define MMC_PM_KEEP_POWER 1
static inline void *sdio_get_drvdata(struct sdio_func *f) { return f->drvdata; }
static inline void sdio_set_drvdata(struct sdio_func *f, void *p) { f->drvdata = p; }
#define dev_to_sdio_func(d) container_of(d, struct sdio_func, dev)
static inline void sdio_claim_host(struct sdio_func *f) { shim_sdio_claim(); }
static inline void sdio_release_host(struct sdio_func *f) { shim_sdio_release(); }
static inline int sdio_enable_func(struct sdio_func *f) { return shim_sdio_enable_func(1); }
static inline int sdio_disable_func(struct sdio_func *f) { return shim_sdio_enable_func(0); }
static inline int sdio_set_block_size(struct sdio_func *f, unsigned int b) { f->cur_blksize = b; return shim_sdio_set_block_size(b); }
static inline unsigned int sdio_align_size(struct sdio_func *f, unsigned int sz) { return sz > 512 ? ALIGN(sz, 512) : ALIGN(sz, 4); }
static inline u8 sdio_readb(struct sdio_func *f, unsigned int addr, int *err) { return shim_sdio_readb(addr, err); }
static inline void sdio_writeb(struct sdio_func *f, u8 v, unsigned int addr, int *err) { shim_sdio_writeb(addr, v, err); }
static inline u16 sdio_readw(struct sdio_func *f, unsigned int addr, int *err) { u16 v = 0; int e = shim_sdio_read_incr(addr, &v, 2); if (err) *err = e; return v; }
static inline u32 sdio_readl(struct sdio_func *f, unsigned int addr, int *err) { u32 v = 0; int e = shim_sdio_read_incr(addr, &v, 4); if (err) *err = e; return v; }
static inline void sdio_writew(struct sdio_func *f, u16 v, unsigned int addr, int *err) { int e = shim_sdio_write_incr(addr, &v, 2); if (err) *err = e; }
static inline void sdio_writel(struct sdio_func *f, u32 v, unsigned int addr, int *err) { int e = shim_sdio_write_incr(addr, &v, 4); if (err) *err = e; }
static inline int sdio_memcpy_fromio(struct sdio_func *f, void *dst, unsigned int addr, int count) { return shim_sdio_read_incr(addr, dst, (unsigned)count); }
static inline int sdio_memcpy_toio(struct sdio_func *f, unsigned int addr, void *src, int count) { return shim_sdio_write_incr(addr, src, (unsigned)count); }
static inline int sdio_claim_irq(struct sdio_func *f, void (*h)(struct sdio_func *)) { f->irq_handler = h; return shim_sdio_claim_irq(); }
static inline int sdio_release_irq(struct sdio_func *f) { f->irq_handler = (void (*)(struct sdio_func *))0; return shim_sdio_release_irq(); }
static inline int sdio_set_host_pm_flags(struct sdio_func *f, unsigned int flags) { return 0; }
static inline bool mmc_card_uhs(struct mmc_card *c) { return false; }

/* ---- misc ---- */
#define current ((void *)shim_current_thread())
#define in_interrupt() 0
#define in_atomic() 0
#define __CHECKER__ 0
#define SET_IEEE80211_DEV(hw, dev) ((void)0)
#define wait_event_timeout(wq, cond, t) ({ unsigned long _l = (unsigned long)(t); while (!(cond) && _l--) shim_msleep(1); (cond) ? 1 : 0; })
struct seq_file;
struct dentry;
struct ieee80211_ops;

#endif

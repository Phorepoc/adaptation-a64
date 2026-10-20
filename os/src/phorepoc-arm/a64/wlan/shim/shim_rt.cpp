// shim_rt.cpp -- OURS (2026-10-10, PinePhone WiFi: the chip core): the runtime under the rtw88 Linux shim
// (shim_rt.h), on nkern. The whole driver runs on ONE kernel thread, the WLAN DFC queue (A64WlanDfcQ, created by
// a64wlan.cpp): every work item, delayed work, timer callback and the SDIO "interrupt" poll is a DFC on that queue,
// exactly as Linux serialises them on its workqueues + the SDIO irq thread -- so the spin locks are counters (nothing
// to exclude), the mutexes are real DMutexes (sleeping, for the later user-side callers), the completions are polled.
// Time: jiffies = milliseconds (HZ = 1000) from the nanokernel tick; ktime from the fast counter. Memory: the kernel
// heap. Logging: the kernel seam UART (phlink), one line per call. Firmware: handed in by a64wlan.cpp
// (shim_firmware_provide) and delivered to the driver's request_firmware_nowait callback.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <nkern.h>
#include "shim_rt.h"

extern "C" void ekseam_puts(const char*);

// the Linux-side objects as the shim headers lay them out (kept in step with shim_core.h)
struct mutex { void* k; };
struct spinlock { void* k; };
struct completion { void* k; };
struct work_struct { void (*func)(struct work_struct*); void* k; };
struct delayed_work { struct work_struct work; void* t; };
struct timer_list { void (*function)(struct timer_list*); void* k; unsigned long expires; };
struct sk_buff { struct sk_buff* next; struct sk_buff* prev; TUint8* head; TUint8* data; TUint8* tail; TUint8* end; unsigned int len;
	char cb[48] __attribute__((aligned(8))); TUint16 queue_mapping; TUint16 protocol; TUint32 priority; void* dev; };
struct firmware { size_t size; const TUint8* data; void* priv; };

extern "C" TDfcQue* A64WlanDfcQ();			// a64wlan.cpp: the driver's thread
static const TInt KDfcPriority = 3;

// ---------------------------------------------------------------- memory, strings
extern "C" void* shim_alloc(size_t n)			{ return Kern::AllocZ((TInt)(n ? n : 1)); }
extern "C" void* shim_zalloc(size_t n)			{ return Kern::AllocZ((TInt)(n ? n : 1)); }
extern "C" void shim_free(void* p)				{ if (p) Kern::Free(p); }
extern "C" void* shim_memdup(const void* s, size_t n) { void* p = Kern::Alloc((TInt)(n ? n : 1)); if (p && n) memcpy(p, s, n); return p; }
extern "C" int shim_memcmp(const void* a, const void* b, size_t n)
	{ const TUint8* x = (const TUint8*)a; const TUint8* y = (const TUint8*)b; for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1; return 0; }
// no-tree-loop-distribute-patterns: at -O2 GCC turns the loop into a call to strlen(), which the kernel has not
extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) size_t shim_strlen(const char* s)	{ size_t n = 0; while (s[n]) ++n; return n; }
extern "C" int shim_strcmp(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return (TUint8)*a - (TUint8)*b; }
extern "C" int shim_strncmp(const char* a, const char* b, size_t n) { for (size_t i = 0; i < n; ++i) { if (a[i] != b[i] || !a[i]) return (TUint8)a[i] - (TUint8)b[i]; } return 0; }
extern "C" char* shim_strncpy(char* d, const char* s, size_t n) { size_t i = 0; for (; i < n && s[i]; ++i) d[i] = s[i]; for (; i < n; ++i) d[i] = 0; return d; }

// a small printf: %[-0][width][.prec](hh|h|l|ll|z)(d i u x X c s p %) + Linux's %pM (a MAC) and %*ph (hex bytes)
struct TOut { char* p; char* end; TInt n; void Put(char c) { if (p < end) *p++ = c; ++n; } };
static void OutPad(TOut& o, TInt aCount, char aPad) { while (aCount-- > 0) o.Put(aPad); }
static void OutNum(TOut& o, TUint64 aVal, TInt aBase, TBool aUpper, TBool aNeg, TInt aWidth, TInt aPrec, TBool aLeft, TBool aZero)
	{
	char buf[24]; TInt len = 0;
	if (aVal == 0) buf[len++] = '0';
	while (aVal) { TInt d = (TInt)(aVal % (TUint)aBase); buf[len++] = (char)(d < 10 ? '0' + d : (aUpper ? 'A' : 'a') + d - 10); aVal /= (TUint)aBase; }
	TInt digits = len > aPrec ? len : aPrec;
	TInt total = digits + (aNeg ? 1 : 0);
	if (!aLeft && !aZero) OutPad(o, aWidth - total, ' ');
	if (aNeg) o.Put('-');
	if (!aLeft && aZero) OutPad(o, aWidth - total, '0');
	OutPad(o, aPrec - len, '0');
	while (len) o.Put(buf[--len]);
	if (aLeft) OutPad(o, aWidth - total, ' ');
	}
extern "C" int shim_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap)
	{
	TOut o; o.p = buf; o.end = buf + (n ? n - 1 : 0); o.n = 0;
	for (const char* f = fmt; *f; ++f)
		{
		if (*f != '%') { o.Put(*f); continue; }
		++f;
		TBool left = EFalse, zero = EFalse; TInt width = 0, prec = -1, lng = 0;
		for (;; ++f) { if (*f == '-') left = ETrue; else if (*f == '0') zero = ETrue; else if (*f == '+' || *f == ' ' || *f == '#') {} else break; }
		if (*f == '*') { width = va_arg(ap, int); ++f; } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		if (*f == '.') { ++f; prec = 0; if (*f == '*') { prec = va_arg(ap, int); ++f; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
		while (*f == 'h') { --lng; ++f; }
		while (*f == 'l') { ++lng; ++f; }
		if (*f == 'z' || *f == 't') { lng = 1; ++f; }
		switch (*f)
			{
			case 'd': case 'i':
				{
				TInt64 v = lng >= 2 ? va_arg(ap, long long) : (TInt64)va_arg(ap, int);
				if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
				OutNum(o, (TUint64)(v < 0 ? -v : v), 10, EFalse, v < 0, width, prec, left, zero); break;
				}
			case 'u': case 'x': case 'X':
				{
				TUint64 v = lng >= 2 ? va_arg(ap, unsigned long long) : (TUint64)va_arg(ap, unsigned int);
				if (lng == -1) v = (unsigned short)v; else if (lng == -2) v = (unsigned char)v;
				OutNum(o, v, *f == 'u' ? 10 : 16, *f == 'X', EFalse, width, prec, left, zero); break;
				}
			case 'c': o.Put((char)va_arg(ap, int)); break;
			case 's':
				{
				const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
				TInt len = 0; while (s[len] && (prec < 0 || len < prec)) ++len;
				if (!left) OutPad(o, width - len, ' ');
				for (TInt i = 0; i < len; ++i) o.Put(s[i]);
				if (left) OutPad(o, width - len, ' ');
				break;
				}
			case 'p':
				{
				const TUint8* p = va_arg(ap, const TUint8*);
				if (f[1] == 'M') { ++f; for (TInt i = 0; i < 6; ++i) { if (i) o.Put(':'); OutNum(o, p[i], 16, EFalse, EFalse, 2, 2, EFalse, ETrue); } }
				else if (f[1] == 'h') { ++f; TInt len = width > 0 ? width : 1; for (TInt i = 0; i < len; ++i) { if (i) o.Put(' '); OutNum(o, p[i], 16, EFalse, EFalse, 2, 2, EFalse, ETrue); } }
				else { o.Put('0'); o.Put('x'); OutNum(o, (TUint32)(TLinAddr)p, 16, EFalse, EFalse, 8, 8, EFalse, ETrue); }
				break;
				}
			case '%': o.Put('%'); break;
			case 0: --f; break;
			default: o.Put('%'); o.Put(*f); break;
			}
		}
	if (n) *o.p = 0;
	return o.n;
	}
extern "C" int shim_snprintf(char* buf, size_t n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = shim_vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
extern "C" void shim_log(const char* fmt, ...)
	{
	char buf[256]; va_list ap; va_start(ap, fmt); int r = shim_vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	if (r <= 0) return;
	if (r >= (int)sizeof(buf)) r = sizeof(buf) - 1;
	if (buf[r - 1] != '\n') { if (r < (int)sizeof(buf) - 1) { buf[r] = '\n'; buf[r + 1] = 0; } else buf[r - 1] = '\n'; }
	ekseam_puts(buf);
	}
extern "C" void shim_hexdump(const char* prefix, const void* buf, size_t len)
	{
	const TUint8* p = (const TUint8*)buf;
	for (size_t off = 0; off < len && off < 256; off += 16)
		{
		char line[96]; TInt n = shim_snprintf(line, sizeof(line), "%s%04x:", prefix ? prefix : "", (unsigned)off);
		for (size_t i = off; i < len && i < off + 16; ++i) n += shim_snprintf(line + n, sizeof(line) - n, " %02x", p[i]);
		shim_log("%s", line);
		}
	}
extern "C" void shim_panic(const char* what) { shim_log("rtw88: PANIC %s", what); Kern::Fault("rtw88", 0); }
extern "C" void shim_random_bytes(TUint8* p, unsigned n)
	{
	static TUint32 s = 0;
	if (!s) s = NKern::FastCounter() ^ 0x9E3779B9u;
	for (unsigned i = 0; i < n; ++i) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; p[i] = (TUint8)s; }
	}

// ---------------------------------------------------------------- sk_buff
extern "C" struct sk_buff* shim_alloc_skb(unsigned size)
	{
	// + 512 hidden tail slack (2026-10-18, boot 987 kernel data abort in memcpy at a page edge): rtw88's
	// rtw_sdio_write_port sends sdio_align_size(len) bytes from skb->data -- above 512 that is a 512 multiple, up
	// to 511 bytes past the data. Linux skbs always have tailroom + skb_shared_info there; ours ended exactly at size,
	// so the reserved-page download at association read past the kernel heap's committed end when it sat there.
	const unsigned total = sizeof(sk_buff) + 8 + size + 512;
	sk_buff* s = (sk_buff*)Kern::AllocZ((TInt)total);
	if (!s) return 0;
	s->head = (TUint8*)(((TLinAddr)(s + 1) + 7) & ~7u);
	s->data = s->tail = s->head; s->end = s->head + size; s->len = 0;
	return s;
	}
extern "C" void shim_free_skb(struct sk_buff* skb) { if (skb) Kern::Free(skb); }
extern "C" struct sk_buff* shim_skb_copy(const struct sk_buff* o)
	{
	sk_buff* s = shim_alloc_skb((unsigned)(o->end - o->head));
	if (!s) return 0;
	s->data = s->head + (o->data - o->head); s->tail = s->data + o->len; s->len = o->len;
	memcpy(s->data, o->data, o->len); memcpy(s->cb, o->cb, sizeof(s->cb));
	s->queue_mapping = o->queue_mapping; s->protocol = o->protocol; s->priority = o->priority; s->dev = o->dev;
	return s;
	}

// ---------------------------------------------------------------- locks
_LIT(KMutexName, "rtw88");
extern "C" void shim_mutex_init(struct mutex* m) { m->k = 0; }
extern "C" void shim_mutex_destroy(struct mutex* m) { if (m->k) { ((DMutex*)m->k)->Close(0); m->k = 0; } }
static DMutex* MutexOf(struct mutex* m)
	{
	if (!m->k) { DMutex* k = 0; if (Kern::MutexCreate(k, KMutexName, KMutexOrdGeneral0) == KErrNone) m->k = k; }
	return (DMutex*)m->k;
	}
extern "C" void shim_mutex_lock(struct mutex* m) { DMutex* k = MutexOf(m); if (k) Kern::MutexWait(*k); }
extern "C" void shim_mutex_unlock(struct mutex* m) { DMutex* k = (DMutex*)m->k; if (k) Kern::MutexSignal(*k); }
extern "C" int shim_mutex_trylock(struct mutex* m) { shim_mutex_lock(m); return 1; }
extern "C" int shim_mutex_is_locked(struct mutex* m) { DMutex* k = (DMutex*)m->k; return k && k->iHoldCount > 0; }
extern "C" void shim_spin_init(struct spinlock* l) { l->k = 0; }
extern "C" void shim_spin_lock(struct spinlock* l) { l->k = (void*)((TLinAddr)l->k + 1); }
extern "C" void shim_spin_unlock(struct spinlock* l) { l->k = (void*)((TLinAddr)l->k - 1); }

// completion: a counter polled at the tick (used for the firmware load, LPS leave checks, scan density)
struct TCompletion { TInt iDone; TBool iAll; };
extern "C" void shim_completion_init(struct completion* c) { TCompletion* k = new TCompletion; if (k) { k->iDone = 0; k->iAll = EFalse; } c->k = k; }
extern "C" void shim_completion_reinit(struct completion* c) { if (!c->k) shim_completion_init(c); else { ((TCompletion*)c->k)->iDone = 0; ((TCompletion*)c->k)->iAll = EFalse; } }
extern "C" void shim_complete(struct completion* c) { if (c->k) ((TCompletion*)c->k)->iDone++; }
extern "C" void shim_complete_all(struct completion* c) { if (c->k) { ((TCompletion*)c->k)->iDone++; ((TCompletion*)c->k)->iAll = ETrue; } }
extern "C" unsigned long shim_wait_for_completion(struct completion* c, unsigned long timeout_jiffies)
	{
	TCompletion* k = (TCompletion*)c->k;
	if (!k) return 0;
	const TUint32 start = NKern::TickCount();
	const TInt ticks = timeout_jiffies ? NKern::TimerTicks((TInt)timeout_jiffies) : 0x7FFFFFFF;
	for (;;)
		{
		if (k->iDone > 0) { if (!k->iAll) k->iDone--; const TInt left = ticks - (TInt)(NKern::TickCount() - start); return left > 1 ? (unsigned long)left : 1; }
		if ((TInt)(NKern::TickCount() - start) >= ticks) return 0;
		NKern::Sleep(1);
		}
	}

// ---------------------------------------------------------------- work, delayed work, timers (DFCs on the WLAN queue)
static void WorkDfc(TAny* aPtr) { work_struct* w = (work_struct*)aPtr; if (w->func) w->func(w); }
static TDfc* DfcOf(work_struct* w)
	{
	if (!w->k) w->k = new TDfc(WorkDfc, w, A64WlanDfcQ(), KDfcPriority);
	return (TDfc*)w->k;
	}
extern "C" void shim_work_init(struct work_struct* w, void (*fn)(struct work_struct*)) { w->func = fn; if (!w->k) DfcOf(w); }
extern "C" int shim_queue_work(struct work_struct* w) { TDfc* d = DfcOf(w); if (!d || d->Queued()) return 0; d->Enque(); return 1; }
extern "C" int shim_cancel_work(struct work_struct* w) { TDfc* d = (TDfc*)w->k; return d ? d->Cancel() : 0; }
extern "C" void shim_flush_work(struct work_struct* w) { TDfc* d = (TDfc*)w->k; if (d && d->Cancel()) WorkDfc(w); }	// one thread: run it now

struct TDelayed { NTimer* iTimer; };
static void DelayedDfc(TAny* aPtr) { delayed_work* dw = (delayed_work*)aPtr; if (dw->work.func) dw->work.func(&dw->work); }
static NTimer* TimerOfDw(delayed_work* dw)
	{
	if (!dw->t) dw->t = new NTimer(DelayedDfc, dw, A64WlanDfcQ(), KDfcPriority);
	return (NTimer*)dw->t;
	}
extern "C" void shim_delayed_work_init(struct delayed_work* dw, void (*fn)(struct work_struct*)) { dw->work.func = fn; dw->work.k = 0; dw->t = 0; }
extern "C" int shim_queue_delayed_work(struct delayed_work* dw, unsigned long delay_jiffies)
	{
	NTimer* t = TimerOfDw(dw);
	if (!t) return 0;
	t->Cancel();
	const TInt ticks = delay_jiffies ? NKern::TimerTicks((TInt)delay_jiffies) : 0;
	t->OneShot(ticks > 0 ? ticks : 1);							// delay 0 = the next tick (Linux: "soon", never inline)
	return 1;
	}
extern "C" int shim_cancel_delayed_work(struct delayed_work* dw) { NTimer* t = (NTimer*)dw->t; return t ? t->Cancel() : 0; }
extern "C" int shim_delayed_work_pending(struct delayed_work* dw) { NTimer* t = (NTimer*)dw->t; return t ? t->IsPending() : 0; }

static void TimerDfc(TAny* aPtr) { timer_list* t = (timer_list*)aPtr; if (t->function) t->function(t); }
extern "C" void shim_timer_setup(struct timer_list* t, void (*fn)(struct timer_list*)) { t->function = fn; t->expires = 0; if (!t->k) t->k = new NTimer(TimerDfc, t, A64WlanDfcQ(), KDfcPriority); }
extern "C" int shim_mod_timer(struct timer_list* t, unsigned long expires_jiffies)
	{
	NTimer* n = (NTimer*)t->k; if (!n) return 0;
	const TBool was = n->Cancel();
	t->expires = expires_jiffies;
	const long delta = (long)(expires_jiffies - shim_jiffies());
	TInt ticks = delta > 0 ? NKern::TimerTicks((TInt)delta) : 1;
	n->OneShot(ticks > 0 ? ticks : 1);
	return was;
	}
extern "C" int shim_del_timer(struct timer_list* t) { NTimer* n = (NTimer*)t->k; return n ? n->Cancel() : 0; }
extern "C" int shim_timer_pending(struct timer_list* t) { NTimer* n = (NTimer*)t->k; return n ? n->IsPending() : 0; }

// ---------------------------------------------------------------- time
extern "C" void shim_udelay(unsigned us) { Kern::NanoWait(us * 1000u); }
extern "C" void shim_usleep(unsigned us) { if (us < 1000) Kern::NanoWait(us * 1000u); else NKern::Sleep(NKern::TimerTicks((TInt)((us + 999) / 1000))); }
extern "C" void shim_msleep(unsigned ms) { NKern::Sleep(NKern::TimerTicks((TInt)(ms ? ms : 1))); }
extern "C" unsigned long shim_jiffies(void)
	{
	static TUint32 periodUs = 0;
	if (!periodUs) periodUs = (TUint32)NKern::TickPeriod();
	return (unsigned long)(((TUint64)NKern::TickCount() * periodUs) / 1000u);
	}
extern "C" TInt64 shim_ktime_ns(void)
	{
	static TUint32 last = 0; static TUint64 high = 0; static TUint32 freq = 0;
	if (!freq) freq = (TUint32)NKern::FastCounterFrequency();
	const TUint32 now = NKern::FastCounter();
	if (now < last) high += 0x100000000ull;
	last = now;
	return (TInt64)(((high | now) * 1000000000ull) / freq);
	}
extern "C" void* shim_current_thread(void) { return NKern::CurrentThread(); }

// ---------------------------------------------------------------- firmware
// the blobs the Symbian side hands in before the probe (a64wlan.cpp: the normal and the WoWLAN image out of the ROM);
// a request is answered at once -- with NULL when the name is unknown, as Linux answers a missing file
struct TFwBlob { const char* iName; const TUint8* iData; size_t iSize; };
static TFwBlob FwBlobs[4];
static TInt FwBlobCount;
static const TFwBlob* FwFind(const char* aWanted)
	{
	const char* base = aWanted; for (const char* p = aWanted; *p; ++p) if (*p == '/') base = p + 1;		// "rtw88/rtw8703b_fw.bin"
	for (TInt i = 0; i < FwBlobCount; ++i) if (shim_strcmp(base, FwBlobs[i].iName) == 0) return &FwBlobs[i];
	return 0;
	}
extern "C" void shim_firmware_provide(const char* aName, const TUint8* aData, size_t aSize)
	{
	if (FwBlobCount < 4) { FwBlobs[FwBlobCount].iName = aName; FwBlobs[FwBlobCount].iData = aData; FwBlobs[FwBlobCount].iSize = aSize; ++FwBlobCount; }
	}
extern "C" int shim_request_firmware(const struct firmware** fw, const char* name)
	{
	*fw = 0;
	const TFwBlob* b = FwFind(name);
	if (!b) return -2;
	firmware* f = (firmware*)Kern::AllocZ(sizeof(firmware)); if (!f) return -12;
	f->data = b->iData; f->size = b->iSize; *fw = f; return 0;
	}
extern "C" int shim_request_firmware_nowait(const char* name, void* ctx, void (*cb)(const struct firmware*, void*))
	{
	const firmware* fw = 0;
	shim_request_firmware(&fw, name);
	shim_log("rtw88: firmware \"%s\": %s (%u bytes)", name, fw ? "delivered" : "NOT AVAILABLE", fw ? (unsigned)fw->size : 0u);
	cb(fw, ctx);
	return 0;
	}
extern "C" void shim_release_firmware(const struct firmware* fw) { if (fw) Kern::Free((TAny*)fw); }

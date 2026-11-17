/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_libc.c -- the few C library calls the FreeBSD USB stack makes, for the Symbian kernel (no libc there).
 * printf / snprintf are renamed at compile time (-Dprintf=phe_usb_printf -Dsnprintf=phe_usb_snprintf) so they never
 * meet another component's; output goes to the kernel's debug serial (ekseam_puts). The string functions are weak:
 * the kernel's own win where it has them. */
#include <stdarg.h>
#include <stddef.h>

void ekseam_puts(const char *);

static int
fmt_num(char *o, size_t n, size_t at, unsigned long long v, int base, int width, int zero, int neg, int upper)
{
	char t[24];
	int k = 0;
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	do { t[k++] = dig[v % base]; v /= base; } while (v && k < 23);
	if (neg)
		t[k++] = '-';
	while (k < width && k < 23)
		t[k++] = zero ? '0' : ' ';
	if (neg && zero && t[k - 1] != '-') {	/* -0005: the sign goes in front of the zeros */
		int i;
		for (i = 0; i < k; i++)
			if (t[i] == '-')
				t[i] = '0';
		t[k - 1] = '-';
	}
	while (k > 0) {
		if (at + 1 < n)
			o[at] = t[k - 1];
		at++;
		k--;
	}
	return (int)at;
}

int
phe_usb_vsnprintf(char *o, size_t n, const char *f, va_list ap)
{
	size_t at = 0;

	for (; *f; f++) {
		int width = 0, zero = 0, lng = 0;
		if (*f != '%') {
			if (at + 1 < n)
				o[at] = *f;
			at++;
			continue;
		}
		f++;
		while (*f == '-' || *f == '#' || *f == '+' || *f == ' ')
			f++;
		if (*f == '0') {
			zero = 1;
			f++;
		}
		if (*f == '*') {
			width = va_arg(ap, int);
			f++;
		}
		while (*f >= '0' && *f <= '9')
			width = width * 10 + (*f++ - '0');
		if (*f == '.') {		/* precision: skipped */
			f++;
			if (*f == '*') {
				(void)va_arg(ap, int);
				f++;
			}
			while (*f >= '0' && *f <= '9')
				f++;
		}
		while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'q') {
			if (*f == 'l' || *f == 'q' || *f == 'j')
				lng++;
			f++;
		}
		switch (*f) {
		case 'd': case 'i': {
			long long v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
			at = fmt_num(o, n, at, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, width, zero, v < 0, 0);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': case 'p': {
			unsigned long long v;
			if (*f == 'p')
				v = (unsigned long)va_arg(ap, void *);
			else
				v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
			at = fmt_num(o, n, at, v, *f == 'u' ? 10 : *f == 'o' ? 8 : 16, width, zero, 0, *f == 'X');
			break;
		}
		case 'c':
			if (at + 1 < n)
				o[at] = (char)va_arg(ap, int);
			at++;
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			while (*s) {
				if (at + 1 < n)
					o[at] = *s;
				at++;
				s++;
			}
			break;
		}
		case '%':
			if (at + 1 < n)
				o[at] = '%';
			at++;
			break;
		default:
			if (!*f)
				f--;
			break;
		}
	}
	if (n)
		o[at < n ? at : n - 1] = 0;
	return (int)at;
}

int
phe_usb_snprintf(char *o, size_t n, const char *f, ...)
{
	va_list ap;
	int r;

	va_start(ap, f);
	r = phe_usb_vsnprintf(o, n, f, ap);
	va_end(ap);
	return r;
}

int
phe_usb_printf(const char *f, ...)
{
	char b[256];
	va_list ap;
	int r;

	va_start(ap, f);
	r = phe_usb_vsnprintf(b, sizeof(b), f, ap);
	va_end(ap);
	ekseam_puts(b);
	return r;
}

__attribute__((weak)) size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
__attribute__((weak)) int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
__attribute__((weak)) int strncmp(const char *a, const char *b, size_t n)
{ for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; } return 0; }
__attribute__((weak)) int memcmp(const void *x, const void *y, size_t n)
{ const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }

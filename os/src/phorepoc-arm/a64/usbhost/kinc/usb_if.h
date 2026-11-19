/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */
/* stand-in for the generated usb_if.h (FreeBSD usb_if.m: the usb_handle_request device method) */
#ifndef PHE_USB_IF_H
#define PHE_USB_IF_H
typedef int usb_handle_request_t(device_t dev, const void *req, void **pptr, uint16_t *plen, uint16_t offset, uint8_t *pstate);
#endif

/*
 * PROJECT:     ReactOS LinuxKPI
 * PURPOSE:     Private IOCTLs that let the LinuxKPI host feed keyboard/mouse input from Linux input
 *              drivers (usbhid, ...) into the ReactOS class drivers.
 */
#ifndef _LKPI_INPUT_H
#define _LKPI_INPUT_H

/* Input: an array of KEYBOARD_INPUT_DATA, sent to \Device\KeyboardClass0 */
#define IOCTL_LKPI_KEYBOARD_INJECT CTL_CODE(FILE_DEVICE_KEYBOARD, 0xF00, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Input: an array of MOUSE_INPUT_DATA, sent to \Device\PointerClass0 */
#define IOCTL_LKPI_MOUSE_INJECT    CTL_CODE(FILE_DEVICE_MOUSE, 0xF00, METHOD_BUFFERED, FILE_ANY_ACCESS)

#endif

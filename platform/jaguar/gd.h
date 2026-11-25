/* RetroHQ's GameDrive BIOS (GDBIOS), as its gdbios_bindings.s exports it. The bindings have no
 * licence, so they are not in this repository: the build assembles them from a copy named by
 * U7_GDBIOS_DIR (see platform/jaguar/README.md). rmac gives their symbols a leading underscore,
 * which m68k-elf C names do not have, hence the asm names. */
#ifndef JAGUAR_GD_H
#define JAGUAR_GD_H

#include <stdint.h>

#define GD_FOPEN_READ 0x01
#define GD_FOPEN_WRITE 0x02
#define GD_FOPEN_OPEN_EXISTING 0x00
#define GD_FOPEN_CREATE_NEW 0x04
#define GD_FOPEN_CREATE_ALWAYS 0x08
#define GD_FOPEN_OPEN_ALWAYS 0x10
#define GD_FREAD_CPU 0
#define GD_FSEEK_SET 0
#define GD_FSEEK_CUR 1
#define GD_FSEEK_END 2
#define GD_FINFO_SHORT_NAME 0

#ifdef __cplusplus
extern "C" {
#endif
/* buffer: 4 KB, long aligned; 0 when installed */
uint16_t GD_Install(void *buffer) __asm__("_GD_Install");
uint32_t GD_HWVersion(void) __asm__("_GD_HWVersion");
uint16_t GD_BIOSVersion(void) __asm__("_GD_BIOSVersion");
/* a handle, or a negative number (as a u16, 0x8000 up) */
uint16_t GD_FOpen(const char *name, uint16_t mode) __asm__("_GD_FOpen");
uint16_t GD_FRead(uint16_t file, void *buffer, uint32_t size, uint16_t flags) __asm__("_GD_FRead");
uint16_t GD_FWrite(uint16_t file, const void *buffer, uint32_t size) __asm__("_GD_FWrite");
uint16_t GD_FSeek(uint16_t file, uint16_t flags, int32_t offset) __asm__("_GD_FSeek");
uint16_t GD_FClose(uint16_t file) __asm__("_GD_FClose");
uint32_t GD_FTell(uint16_t file) __asm__("_GD_FTell");
uint32_t GD_FSize(uint16_t file) __asm__("_GD_FSize");
/* info: 22 bytes, as files.c's gd_info (size, date, time, attributes, 8.3 name) */
uint16_t GD_FInfo(const char *name, void *info, uint16_t flags) __asm__("_GD_FInfo");
uint16_t GD_DOpen(const char *name) __asm__("_GD_DOpen");
uint16_t GD_DRead(uint16_t dir, void *info, uint16_t flags) __asm__("_GD_DRead");
uint16_t GD_DClose(uint16_t dir) __asm__("_GD_DClose");
uint16_t GD_CardIn(void) __asm__("_GD_CardIn");
void GD_DebugString(const char *text) __asm__("_GD_DebugString");
#ifdef __cplusplus
}
#endif

#endif

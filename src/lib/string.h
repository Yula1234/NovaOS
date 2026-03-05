#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Freestanding libc subset used by the kernel; implemented in src/lib/string.cpp. */

void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);

#ifdef __cplusplus
}
#endif

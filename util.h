#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdint.h>
#include <stdio.h>

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

void hexdump(FILE *fp, void *data, size_t size);

uint16_t cksum16(uint16_t *data, uint16_t size, uint32_t init);
uint16_t hton16(uint16_t);
uint16_t ntoh16(uint16_t);
uint32_t hton32(uint32_t);
uint32_t ntoh32(uint32_t);

void maskset(uint32_t *mask, size_t size, size_t offset, size_t len);
int maskchk(uint32_t *mask, size_t size, size_t offset, size_t len);
void maskclr(uint32_t *mask, size_t size);
void maskdbg(uint32_t *mask, size_t size);

#endif

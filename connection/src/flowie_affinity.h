#ifndef FLOWIE_AFFINITY_H
#define FLOWIE_AFFINITY_H

#include <stdint.h>

/* Bind the calling owner thread; never change the caller of server_start. */
int flowie_affinity_bind(uint32_t cpu);

#endif

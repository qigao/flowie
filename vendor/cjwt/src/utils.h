/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __UTILS_H__
#define __UTILS_H__

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct section {
    const char *data;
    size_t len;
};

struct split_jwt {
    size_t count;
    struct section sections[5];
};

int split(const char *full, size_t len, struct split_jwt *split);

char *cjwt_strdup(const char *s);

void *b64url_decode_with_alloc(const uint8_t *src, size_t len, size_t *out_len);
char *b64url_encode_with_alloc(const uint8_t *src, size_t len, size_t *out_len);

#endif


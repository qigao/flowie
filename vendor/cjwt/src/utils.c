/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <base64_utils.h>

#include "utils.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
int split(const char *full, size_t len, struct split_jwt *split)
{
    /* Trim trailing whitespace from the full input to avoid it being included in the last section */
    while (len > 0 && (full[len - 1] == '\r' || full[len - 1] == '\n' || full[len - 1] == ' ' || full[len - 1] == '\t')) {
        len--;
    }

    size_t dots[6] = { 0, len, len, len, len, len };

    memset(split, 0, sizeof(struct split_jwt));

    split->count = 1;
    for (size_t i = 0; i < len; i++) {
        if ('.' == full[i]) {
            if (5 <= split->count) {
                /* Too many sections */
                return -1;
            }
            dots[split->count] = i;
            split->count++;
        }
    }

    if (1 == split->count) {
        return -1;
    }

    split->sections[0].data = full;
    split->sections[0].len  = dots[1];

    for (size_t i = 1; i < split->count; i++) {
        split->sections[i].data = &full[dots[i] + 1];
        split->sections[i].len  = dots[i + 1] - dots[i] - 1;
    }

    return (int)split->count;
}


char *cjwt_strdup(const char *s)
{
    char *rv = NULL;

    if (s) {
        size_t len;

        len = strlen(s) + 1;
        rv  = (char *) malloc(len);
        if (rv) {
            memcpy(rv, s, len);
        }
    }

    return rv;
}




/*----------------------------------------------------------------------------*/
/*                               Internal functions                             */
/*----------------------------------------------------------------------------*/
void *b64url_decode_with_alloc(const uint8_t *src, size_t len, size_t *out_len)
{
    if (!src || !out_len) return NULL;

    // Normalize base64url to standard base64
    size_t norm_len = (len + 3) & ~3;
    char *norm = (char *)malloc(norm_len + 1);
    if (!norm) return NULL;

    size_t actual_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '-' ) norm[actual_len++] = '+';
        else if (src[i] == '_') norm[actual_len++] = '/';
        else if (src[i] == '\r' || src[i] == '\n' || src[i] == ' ' || src[i] == '\t') continue;
        else norm[actual_len++] = (char)src[i];
    }
    
    // Normalize len to the padding boundary
    size_t norm_len_padded = (actual_len + 3) & ~3;
    
    // Add padding if missing
    for (size_t i = actual_len; i < norm_len_padded; i++) {
        norm[i] = '=';
    }
    norm[norm_len_padded] = '\0';

    uint8_t *out = NULL;
    if (tn_base64_decode(norm, &out, out_len) != 0) {
        free(norm);
        return NULL;
    }

    free(norm);
    return out;
}

char *b64url_encode_with_alloc(const uint8_t *src, size_t len, size_t *out_len)
{
    if (!src) return NULL;

    char *out = NULL;
    if (tn_base64_encode(src, len, &out) != 0) return NULL;
    size_t encoded_len = strlen(out);

    // Convert to base64url
    for (size_t i = 0; i < encoded_len; i++) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }

    // Remove padding
    while (encoded_len > 0 && out[encoded_len - 1] == '=') {
        encoded_len--;
    }
    out[encoded_len] = '\0';

    if (out_len) {
        *out_len = encoded_len;
    }

    return out;
}



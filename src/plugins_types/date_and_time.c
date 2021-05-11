/**
 * @file date_and_time.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief ietf-yang-types date-and-time type plugin.
 *
 * Copyright (c) 2019-2021 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE /* asprintf, strdup */
#include <sys/cdefs.h>

#include "plugins_types.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libyang.h"

#include "common.h"
#include "compat.h"

/**
 * @page howtoDataLYB LYB Binary Format
 * @subsection howtoDataLYBTypesDateAndTime date-and-time (ietf-yang-types)
 *
 * | Size (B) | Mandatory | Type | Meaning |
 * | :------  | :-------: | :--: | :-----: |
 * | 8        | yes | `time_t *` | UNIX timestamp |
 * | string length | no | `char *` | string with the fraction digits of a second |
 */

static void lyplg_type_free_date_and_time(const struct ly_ctx *ctx, struct lyd_value *value);

/**
 * @brief Stored value structure for date-and-time
 */
struct lyd_value_date_and_time {
    time_t time;        /**< UNIX timestamp */
    char *fractions_s;  /**< fractions of a second */
};

/**
 * @brief Convert date-and-time from string to UNIX timestamp and fractions of a second.
 *
 * @param[in] value Valid string value.
 * @param[out] time UNIX timestamp.
 * @param[out] fractions_s Fractions of a second, set to NULL if none.
 * @return LY_ERR value.
 */
static LY_ERR
dat_str2time(const char *value, time_t *time, char **fractions_s)
{
    struct tm tm = {0};
    uint32_t i, frac_len;
    const char *frac;
    int64_t shift, shift_m;
    time_t t;

    tm.tm_year = atoi(&value[0]) - 1900;
    tm.tm_mon = atoi(&value[5]) - 1;
    tm.tm_mday = atoi(&value[8]);
    tm.tm_hour = atoi(&value[11]);
    tm.tm_min = atoi(&value[14]);
    tm.tm_sec = atoi(&value[17]);

    t = timegm(&tm);
    i = 19;

    /* fractions of a second */
    if (value[i] == '.') {
        ++i;
        frac = &value[i];
        for (frac_len = 0; isdigit(frac[frac_len]); ++frac_len) {}

        i += frac_len;

        /* skip trailing zeros */
        for ( ; frac_len && (frac[frac_len - 1] == '0'); --frac_len) {}

        if (!frac_len) {
            /* only zeros, ignore */
            frac = NULL;
        }
    } else {
        frac = NULL;
    }

    /* apply offset */
    if ((value[i] == 'Z') || (value[i] == 'z')) {
        /* zero shift */
        shift = 0;
    } else {
        shift = strtol(&value[i], NULL, 10);
        shift = shift * 60 * 60; /* convert from hours to seconds */
        shift_m = strtol(&value[i + 4], NULL, 10) * 60; /* includes conversion from minutes to seconds */
        /* correct sign */
        if (shift < 0) {
            shift_m *= -1;
        }
        /* connect hours and minutes of the shift */
        shift = shift + shift_m;
    }

    /* we have to shift to the opposite way to correct the time */
    t -= shift;

    *time = t;
    if (frac) {
        *fractions_s = strndup(frac, frac_len);
        LY_CHECK_RET(!*fractions_s, LY_EMEM);
    } else {
        *fractions_s = NULL;
    }
    return LY_SUCCESS;
}

/**
 * @brief Convert UNIX timestamp and fractions of a second into canonical date-and-time string value.
 *
 * @param[in] time UNIX timestamp.
 * @param[in] fractions_s Fractions of a second, if any.
 * @param[out] str Canonical string value.
 * @return LY_ERR value.
 */
static LY_ERR
dat_time2str(time_t time, const char *fractions_s, char **str)
{
    struct tm tm;
    char zoneshift[7];
    int32_t zonediff_h, zonediff_m;

    /* initialize the local timezone */
    tzset();

    /* convert */
    if (!localtime_r(&time, &tm)) {
        return LY_ESYS;
    }

    /* get timezone offset */
    if (tm.tm_gmtoff == 0) {
        /* time is Zulu (UTC) */
        zonediff_h = 0;
        zonediff_m = 0;
    } else {
        /* timezone offset */
        zonediff_h = tm.tm_gmtoff / 60 / 60;
        zonediff_m = tm.tm_gmtoff / 60 % 60;
    }
    sprintf(zoneshift, "%+03d:%02d", zonediff_h, zonediff_m);

    /* print */
    if (asprintf(str, "%04d-%02d-%02dT%02d:%02d:%02d%s%s%s",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
            fractions_s ? "." : "", fractions_s ? fractions_s : "", zoneshift) == -1) {
        return LY_EMEM;
    }

    return LY_SUCCESS;
}

/**
 * @brief Implementation of ::lyplg_type_store_clb for ietf-yang-types date-and-time type.
 */
static LY_ERR
lyplg_type_store_date_and_time(const struct ly_ctx *ctx, const struct lysc_type *type, const void *value, size_t value_len,
        uint32_t options, LY_VALUE_FORMAT format, void *UNUSED(prefix_data), uint32_t hints,
        const struct lysc_node *UNUSED(ctx_node), struct lyd_value *storage, struct lys_glob_unres *UNUSED(unres),
        struct ly_err_item **err)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysc_type_str *type_dat = (struct lysc_type_str *)type;
    struct lyd_value_date_and_time *val;
    uint32_t i;
    char c;

    /* init storage */
    memset(storage, 0, sizeof *storage);
    LYPLG_TYPE_VAL_INLINE_PREPARE(storage, val);
    LY_CHECK_ERR_GOTO(!val, ret = LY_EMEM, cleanup);
    storage->realtype = type;

    if (format == LY_VALUE_LYB) {
        /* validation */
        if (value_len < 8) {
            ret = ly_err_new(err, LY_EVALID, LYVE_DATA, NULL, NULL, "Invalid LYB date-and-time value size %zu "
                    "(expected at least 8).", value_len);
            goto cleanup;
        }
        for (i = 8; i < value_len; ++i) {
            c = ((char *)value)[i];
            if (!isdigit(c)) {
                ret = ly_err_new(err, LY_EVALID, LYVE_DATA, NULL, NULL, "Invalid LYB date-and-time character '%c' "
                        "(expected a digit).", c);
                goto cleanup;
            }
        }

        /* store timestamp */
        memcpy(&val->time, value, sizeof val->time);

        /* store fractions of second */
        if (value_len > 8) {
            val->fractions_s = strndup(((char *)value) + 8, value_len - 8);
            LY_CHECK_ERR_GOTO(!val->fractions_s, ret = LY_EMEM, cleanup);
        }

        /* success */
        goto cleanup;
    }

    /* check hints */
    ret = lyplg_type_check_hints(hints, value, value_len, type->basetype, NULL, err);
    LY_CHECK_GOTO(ret, cleanup);

    /* length restriction, there can be only ASCII chars */
    if (type_dat->length) {
        ret = lyplg_type_validate_range(LY_TYPE_STRING, type_dat->length, value_len, value, value_len, err);
        LY_CHECK_GOTO(ret, cleanup);
    }

    /* date-and-time pattern */
    ret = lyplg_type_validate_patterns(type_dat->patterns, value, value_len, err);
    LY_CHECK_GOTO(ret, cleanup);

    /* pattern validation succeeded, convert to UNIX time and fractions of second */
    ret = dat_str2time(value, &val->time, &val->fractions_s);
    LY_CHECK_GOTO(ret, cleanup);

    if (format == LY_VALUE_CANON) {
        /* store canonical value */
        if (options & LYPLG_TYPE_STORE_DYNAMIC) {
            ret = lydict_insert_zc(ctx, (char *)value, &storage->_canonical);
            options &= ~LYPLG_TYPE_STORE_DYNAMIC;
            LY_CHECK_GOTO(ret, cleanup);
        } else {
            ret = lydict_insert(ctx, value, value_len, &storage->_canonical);
            LY_CHECK_GOTO(ret, cleanup);
        }
    }

cleanup:
    if (options & LYPLG_TYPE_STORE_DYNAMIC) {
        free((void *)value);
    }

    if (ret) {
        lyplg_type_free_date_and_time(ctx, storage);
    }
    return ret;
}

/**
 * @brief Implementation of ::lyplg_type_compare_clb for ietf-yang-types date-and-time type.
 */
static LY_ERR
lyplg_type_compare_date_and_time(const struct lyd_value *val1, const struct lyd_value *val2)
{
    struct lyd_value_date_and_time *v1, *v2;

    if (val1->realtype != val2->realtype) {
        return LY_ENOT;
    }

    LYD_VALUE_GET(val1, v1);
    LYD_VALUE_GET(val2, v2);

    /* compare timestamp */
    if (v1->time != v2->time) {
        return LY_ENOT;
    }

    /* compare second fractions */
    if ((!v1->fractions_s && !v2->fractions_s) ||
            (v1->fractions_s && v2->fractions_s && !strcmp(v1->fractions_s, v2->fractions_s))) {
        return LY_SUCCESS;
    }
    return LY_ENOT;
}

/**
 * @brief Implementation of ::lyplg_type_print_clb for ietf-yang-types date-and-time type.
 */
static const void *
lyplg_type_print_date_and_time(const struct ly_ctx *ctx, const struct lyd_value *value, LY_VALUE_FORMAT format,
        void *UNUSED(prefix_data), ly_bool *dynamic, size_t *value_len)
{
    struct lyd_value_date_and_time *val;
    char *ret;

    LYD_VALUE_GET(value, val);

    if (format == LY_VALUE_LYB) {
        if (val->fractions_s) {
            ret = malloc(8 + strlen(val->fractions_s));
            LY_CHECK_ERR_RET(!ret, LOGMEM(ctx), NULL);

            *dynamic = 1;
            if (value_len) {
                *value_len = 8 + strlen(val->fractions_s);
            }
            memcpy(ret, &val->time, sizeof val->time);
            memcpy(ret + 8, val->fractions_s, strlen(val->fractions_s));
        } else {
            *dynamic = 0;
            if (value_len) {
                *value_len = 8;
            }
            ret = (char *)&val->time;
        }
        return ret;
    }

    /* generate canonical value if not already */
    if (!value->_canonical) {
        /* get the canonical value */
        if (dat_time2str(val->time, val->fractions_s, &ret)) {
            return NULL;
        }

        /* store it */
        if (lydict_insert_zc(ctx, ret, (const char **)&value->_canonical)) {
            LOGMEM(ctx);
            return NULL;
        }
    }

    /* use the cached canonical value */
    if (dynamic) {
        *dynamic = 0;
    }
    if (value_len) {
        *value_len = strlen(value->_canonical);
    }
    return value->_canonical;
}

/**
 * @brief Implementation of ::lyplg_type_dup_clb for ietf-yang-types date-and-time type.
 */
static LY_ERR
lyplg_type_dup_date_and_time(const struct ly_ctx *ctx, const struct lyd_value *original, struct lyd_value *dup)
{
    LY_ERR ret;
    struct lyd_value_date_and_time *orig_val, *dup_val;

    memset(dup, 0, sizeof *dup);

    /* optional canonical value */
    ret = lydict_insert(ctx, original->_canonical, ly_strlen(original->_canonical), &dup->_canonical);
    LY_CHECK_GOTO(ret, error);

    /* allocate value */
    LYPLG_TYPE_VAL_INLINE_PREPARE(dup, dup_val);
    LY_CHECK_ERR_GOTO(!dup_val, ret = LY_EMEM, error);

    LYD_VALUE_GET(original, orig_val);

    /* copy timestamp */
    dup_val->time = orig_val->time;

    /* duplicate second fractions */
    if (orig_val->fractions_s) {
        dup_val->fractions_s = strdup(orig_val->fractions_s);
        LY_CHECK_ERR_GOTO(!dup_val->fractions_s, ret = LY_EMEM, error);
    }

    dup->realtype = original->realtype;
    return LY_SUCCESS;

error:
    lyplg_type_free_date_and_time(ctx, dup);
    return ret;
}

/**
 * @brief Implementation of ::lyplg_type_free_clb for ietf-yang-types date-and-time type.
 */
static void
lyplg_type_free_date_and_time(const struct ly_ctx *ctx, struct lyd_value *value)
{
    struct lyd_value_date_and_time *val;

    lydict_remove(ctx, value->_canonical);
    LYD_VALUE_GET(value, val);
    if (val) {
        free(val->fractions_s);
        LYPLG_TYPE_VAL_INLINE_DESTROY(val);
    }
}

/**
 * @brief Plugin information for date-and-time type implementation.
 *
 * Note that external plugins are supposed to use:
 *
 *   LYPLG_TYPES = {
 */
const struct lyplg_type_record plugins_date_and_time[] = {
    {
        .module = "ietf-yang-types",
        .revision = "2013-07-15",
        .name = "date-and-time",

        .plugin.id = "libyang 2 - date-and-time, version 1",
        .plugin.store = lyplg_type_store_date_and_time,
        .plugin.validate = NULL,
        .plugin.compare = lyplg_type_compare_date_and_time,
        .plugin.sort = NULL,
        .plugin.print = lyplg_type_print_date_and_time,
        .plugin.duplicate = lyplg_type_dup_date_and_time,
        .plugin.free = lyplg_type_free_date_and_time
    },
    {0}
};
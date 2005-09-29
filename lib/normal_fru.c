/*
 * normal_fru.c
 *
 * "normal" (IPMI-specified) fru handling
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 * Note that this file was originally written by Thomas Kanngieser
 * <thomas.kanngieser@fci.com> of FORCE Computers, but I've pretty
 * much gutted it and rewritten it, nothing really remained the same.
 * Thomas' code was helpful, though and many thanks go to him.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <OpenIPMI/ipmiif.h>
#include <OpenIPMI/ipmi_fru.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_msgbits.h>

#include <OpenIPMI/internal/locked_list.h>
#include <OpenIPMI/internal/ipmi_domain.h>
#include <OpenIPMI/internal/ipmi_int.h>
#include <OpenIPMI/internal/ipmi_utils.h>
#include <OpenIPMI/internal/ipmi_oem.h>
#include <OpenIPMI/internal/ipmi_fru.h>

#define IPMI_LANG_CODE_ENGLISH	25

/***********************************************************************
 *
 * Normal fru info.
 *
 **********************************************************************/

/* Records used to hold the FRU. */
typedef struct ipmi_fru_record_s ipmi_fru_record_t;

typedef struct fru_string_s
{
    enum ipmi_str_type_e type;
    unsigned short       length;
    char                 *str;

    /* The raw offset from the start of the area, and the raw length
       of this string.  This is the offset and length in the raw FRU
       data. */
    unsigned short       offset;
    unsigned short       raw_len;
    unsigned char        *raw_data;

    /* Has this value been changed locally since it has been read?
       Use to know that this needs to be written. */
    char                 changed;
} fru_string_t;

typedef struct fru_variable_s
{
    unsigned short len;
    unsigned short next;
    fru_string_t   *strings;
} fru_variable_t;

typedef struct fru_area_info_s {
    unsigned short num_fixed_fields;
    unsigned short field_start;
    unsigned short empty_length;
    fru_variable_t *(*get_fields)(ipmi_fru_record_t *rec);
    void (*free)(ipmi_fru_record_t *rec);
    unsigned short extra_len;
    int (*decode)(ipmi_fru_t        *fru,
		  unsigned char     *data,
		  unsigned int      data_len,
		  ipmi_fru_record_t **rrec);
    int (*encode)(ipmi_fru_t *fru, unsigned char *data);
    void (*setup_new)(ipmi_fru_record_t *rec);
} fru_area_info_t;

/* Forward declaration */
static fru_area_info_t fru_area_info[IPMI_FRU_FTR_NUMBER];

struct ipmi_fru_record_s
{
    fru_area_info_t       *handlers;
    void                  *data;

    /* Where does this area start in the FRU and how much memory is
       available? */
    unsigned int          offset;
    unsigned int          length;

    /* How much of the area is currently used? */
    unsigned int          used_length;

    /* Length of the used length in the  */
    unsigned int          orig_used_length;

    /* Has this value been changed locally since it has been read?
       Use to know that something in the record needs to be written,
       the header needs to be rewritten, and the checksum needs to be
       recalculated. */
    char                  changed;

    /* Does the whole area require a rewrite?  This would be true if
       the position changed or the length was increased. */
    char                 rewrite;
};

static void fru_record_destroy(ipmi_fru_record_t *rec);


typedef struct normal_fru_rec_data_s
{
    int               version;

    /* Has an offset changed (thus causing the header to need to be
       rewritten)? */
    int               header_changed;

    ipmi_fru_record_t *recs[IPMI_FRU_FTR_NUMBER];
} normal_fru_rec_data_t;

static ipmi_fru_record_t **
normal_fru_get_recs(ipmi_fru_t *fru)
{
    normal_fru_rec_data_t *info = _ipmi_fru_get_rec_data(fru);
    return info->recs;
}

/***********************************************************************
 *
 * Normal fru data formatting.
 *
 **********************************************************************/

static unsigned char
checksum(unsigned char *data, unsigned int length)
{
    unsigned char sum = 0;

    while (length) {
	sum += *data;
	data++;
	length--;
    }

    return sum;
}

/* 820476000 is seconds between 1970.01.01 00:00:00 and 1996.01.01 00:00:00 */
#define FRU_TIME_TO_UNIX_TIME(t) (((t) * 60) + 820476000)
#define UNIX_TIME_TO_FRU_TIME(t) ((((t) - 820476000) + 30) / 60)

static int
read_fru_time(unsigned char **data,
	      unsigned int  *len,
	      time_t        *time)
{
    unsigned int  t;
    unsigned char *d = *data;

    if (*len < 3)
	return EBADF;

    t = *d++;
    t += *d++ * 256;
    t += *d++ * 256 * 256;

    *len -= 3;
    *data += 3;

    *time = FRU_TIME_TO_UNIX_TIME(t);
    return 0;
}

static void
write_fru_time(unsigned char *d, time_t time)
{
    unsigned int t;

    t = UNIX_TIME_TO_FRU_TIME(time);

    *d++ = t & 0xff;
    t >>= 8;
    *d++ = t & 0xff;
    t >>= 8;
    *d++ = t & 0xff;
    t >>= 8;
}

static int
fru_encode_fields(ipmi_fru_t        *fru,
		  ipmi_fru_record_t *rec,
		  fru_variable_t    *v,
		  unsigned char     *data,
		  unsigned int      offset)
{
    int i;
    int rv;

    for (i=0; i<v->next; i++) {
	fru_string_t *s = v->strings + i;
	int          len;

	if (offset != s->offset) {
	    /* Bug in the FRU code.  Return a unique error code so it
	       can be identified, but don't pass it to the user. */
	    return EBADF;
	}

	if (s->raw_data) {
	    memcpy(data+offset, s->raw_data, s->raw_len);
	    len = s->raw_len;
	} else if (s->str) {
	    len = IPMI_MAX_STR_LEN;
	    ipmi_set_device_string(s->str, s->type, s->length,
				   data+offset, 1, &len);
	} else {
	    data[offset] = 0xc0;
	    len = 1;
	}
	if (s->changed && !rec->rewrite) {
	    rv = _ipmi_fru_new_update_record(fru, offset+rec->offset, len);
	    if (rv)
		return rv;
	}
	offset += len;
    }
    /* Now the end marker */
    data[offset] = 0xc1;
    /* If the record changed, put out the end marker */
    if (rec->changed && !rec->rewrite) {
	rv = _ipmi_fru_new_update_record(fru, offset+rec->offset, 1);
	if (rv)
	    return rv;
    }
    offset++;
    /* We are not adding the checksum, so remove it from the check */
    if (offset != (rec->used_length-1)) {
	return EBADF;
    }
    return 0;
}

/***********************************************************************
 *
 * Custom field handling for FRUs.  This is a variable-length array
 * of strings.
 *
 **********************************************************************/

static int
fru_setup_min_field(ipmi_fru_record_t *rec, int area, int changed)
{
    int            i;
    unsigned int   min;
    unsigned int   start_offset;
    fru_variable_t *v;

    if (!fru_area_info[area].get_fields)
	return 0;

    v = fru_area_info[area].get_fields(rec);
    min = fru_area_info[area].num_fixed_fields;
    start_offset = fru_area_info[area].field_start;

    if (min == 0)
	return 0;

    v->strings = ipmi_mem_alloc(min * sizeof(fru_string_t));
    if (!v->strings)
	return ENOMEM;
    memset(v->strings, 0, min * sizeof(fru_string_t));
    for (i=0; i<min; i++) {
	v->strings[i].changed = changed;
	v->strings[i].offset = start_offset;
	start_offset++;
	v->strings[i].raw_len = 1;
    }
    v->len = min;
    v->next = min;
    return 0;
}

static int
fru_string_set(enum ipmi_str_type_e type,
	       char                 *str,
	       unsigned int         len,
	       ipmi_fru_record_t    *rec,
	       fru_variable_t       *vals,
	       unsigned int         num,
	       int                  is_custom)
{
    char         *newval;
    fru_string_t *val = vals->strings + num;
    unsigned char tstr[IPMI_MAX_STR_LEN+1];
    int           raw_len = sizeof(tstr);
    int           raw_diff;
    int           i;

    if (str) {
	/* First calculate if it will fit into the record area. */

	/* Truncate if too long. */
	if (len > 63)
	    len = 63;
	ipmi_set_device_string(str, type, len, tstr, 1, &raw_len);
	raw_diff = raw_len - val->raw_len;
	if ((raw_diff > 0) && (rec->used_length+raw_diff > rec->length))
	    return ENOSPC;
	if (len == 0)
	    newval = ipmi_mem_alloc(1);
	else
	    newval = ipmi_mem_alloc(len);
	if (!newval)
	    return ENOMEM;
	memcpy(newval, str, len);
    } else {
	newval = NULL;
	len = 0;
	raw_diff = 1 - val->raw_len;
    }

    if (val->str)
	ipmi_mem_free(val->str);
    if (val->raw_data) {
	ipmi_mem_free(val->raw_data);
	val->raw_data = NULL;
    }

    if (!is_custom || newval) {
	/* Either it's not a custom value (and thus is always there)
	   or there is a value to put in.  Modify the length and
	   reduce the offset of all the following strings. */
	val->str = newval;
	val->length = len;
	val->type = type;
	val->raw_len += raw_diff;
	val->changed = 1;
	if (raw_diff) {
	    for (i=num+1; i<vals->next; i++) {
		vals->strings[i].offset += raw_diff;
		vals->strings[i].changed = 1;
	    }
	}
    } else {
	/* A custom value that is being cleared.  Nuke it by moving
	   all the strings following this back. */
	raw_diff = -val->raw_len;
	vals->next--;
	for (i=num; i<vals->next; i++) {
	    vals->strings[i] = vals->strings[i+1];
	    vals->strings[i].offset += raw_diff;
	    vals->strings[i].changed = 1;
	}
    }

    rec->used_length += raw_diff;
    rec->changed |= 1;

    return 0;
}

static int
fru_decode_string(ipmi_fru_t     *fru,
		  unsigned char  *start_pos,
		  unsigned char  **in,
		  unsigned int   *in_len,
		  int            lang_code,
		  int            force_english,
		  fru_variable_t *strs,
		  unsigned int   num)
{
    unsigned char str[IPMI_MAX_STR_LEN+1];
    int           force_unicode;
    fru_string_t  *out = strs->strings + num;
    unsigned char *in_start;

    out->offset = *in - start_pos;
    in_start = *in;
    force_unicode = !force_english && (lang_code != IPMI_LANG_CODE_ENGLISH);
    out->length = ipmi_get_device_string(in, *in_len, str,
					 IPMI_STR_FRU_SEMANTICS, force_unicode,
					 &out->type, sizeof(str));
    out->raw_len = *in - in_start;
    *in_len -= out->raw_len;
    out->raw_data = ipmi_mem_alloc(out->raw_len);
    if (!out->raw_data)
	return ENOMEM;
    memcpy(out->raw_data, in_start, out->raw_len);

    if (out->length != 0) {
	out->str = ipmi_mem_alloc(out->length);
	if (!out->str) {
	    ipmi_mem_free(out->raw_data);
	    return ENOMEM;
	}
	memcpy(out->str, str, out->length);
    } else {
	out->str = ipmi_mem_alloc(1);
	if (!out->str) {
	    ipmi_mem_free(out->raw_data);
	    return ENOMEM;
	}
    }
    return 0;
}

static int
fru_string_to_out(char *out, unsigned int *length, fru_string_t *in)
{
    int clen;

    if (!in->str)
	return ENOSYS;

    if (in->length > *length)
	clen = *length;
    else
	clen = in->length;
    memcpy(out, in->str, clen);

    if (in->type == IPMI_ASCII_STR) {
	/* NIL terminate the ASCII string. */
	if (clen == *length)
	    clen--;

	out[clen] = '\0';
    }

    *length = clen;

    return 0;
}

static void
fru_free_string(fru_string_t *str)
{
    if (str->str)
	ipmi_mem_free(str->str);
    if (str->raw_data)
	ipmi_mem_free(str->raw_data);
}


static int
fru_variable_string_set(ipmi_fru_record_t    *rec,
			fru_variable_t       *val,
			unsigned int         first_custom,
			unsigned int         num,
			enum ipmi_str_type_e type,
			char                 *str,
			unsigned int         len,
			int                  is_custom)
{
    int rv;

    if (is_custom) {
	/* Renumber to get the custom fields.  We do this a little
	   strangly to avoid overflows if the user passes in MAX_INT
	   for the num. */
	if (num > val->next - first_custom)
	    num = val->next;
	else
	    num += first_custom;
    }
    if (num >= val->next) {
	if (len == 0) {
	    /* Don't expand if we are deleting an invalid field,
	       return an error. */
	    return EINVAL;
	}
	num = val->next;
	/* If not enough room, expand the array by a set amount (to
	   keep from thrashing memory when adding lots of things). */
	if (val->next >= val->len) {
	    fru_string_t *newval;
	    unsigned int alloc_num = val->len + 16;

	    newval = ipmi_mem_alloc(sizeof(fru_string_t) * alloc_num);
	    if (!newval)
		return ENOMEM;
	    memset(newval, 0, sizeof(fru_string_t) * alloc_num);
	    if (val->strings) {
		memcpy(newval, val->strings, sizeof(fru_string_t) * val->next);
		ipmi_mem_free(val->strings);
	    }
	    val->strings = newval;
	    val->len = alloc_num;
	}
	val->strings[num].str = NULL;
	val->strings[num].raw_data = NULL;
	/* Subtract 2 below because of the end marker and the checksum. */
	val->strings[num].offset = rec->used_length-2;
	val->strings[num].length = 0;
	val->strings[num].raw_len = 0;
	val->next++;
    }

    rv = fru_string_set(type, str, len, rec, val, num, is_custom);
    return rv;
}

static int
fru_decode_variable_string(ipmi_fru_t     *fru,
			   unsigned char  *start_pos,
			   unsigned char  **in,
			   unsigned int   *in_len,
			   int            lang_code,
			   fru_variable_t *v)
{
    int err;

    if (v->next == v->len) {
#define FRU_STR_ALLOC_INCREMENT	5
	fru_string_t *n;
	int          n_len = v->len + FRU_STR_ALLOC_INCREMENT;

	n = ipmi_mem_alloc(sizeof(fru_string_t) * n_len);
	if (!n)
	    return ENOMEM;

	if (v->strings) {
	    memcpy(n, v->strings, sizeof(fru_string_t) * v->len);
	    ipmi_mem_free(v->strings);
	}
	memset(n + v->len, 0,
	       sizeof(fru_string_t) * FRU_STR_ALLOC_INCREMENT);
	v->strings = n;
	v->len = n_len;
    }

    err = fru_decode_string(fru, start_pos, in, in_len, lang_code, 0,
			    v, v->next);
    if (!err)
	v->next++;
    return err;
}

static int
fru_variable_string_to_out(fru_variable_t *in,
			   unsigned int   num,
			   char           *out,
			   unsigned int   *length)
{
    if (num >= in->next)
	return E2BIG;

    return fru_string_to_out(out, length, &in->strings[num]);
}

static int
fru_variable_string_length(fru_variable_t *in,
			   unsigned int   num,
			   unsigned int   *length)
{
    if (num >= in->next)
	return E2BIG;

    if (in->strings[num].type == IPMI_ASCII_STR)
	*length = in->strings[num].length + 1;
    else
	*length = in->strings[num].length;
    return 0;
}

static int
fru_variable_string_type(fru_variable_t       *in,
			 unsigned int         num,
			 enum ipmi_str_type_e *type)
{
    if (num >= in->next)
	return E2BIG;

    *type = in->strings[num].type;
    return 0;
}

static void
fru_free_variable_string(fru_variable_t *v)
{
    int i;

    for (i=0; i<v->next; i++)
	fru_free_string(&v->strings[i]);

    if (v->strings)
	ipmi_mem_free(v->strings);
}


/***********************************************************************
 *
 * Here is the basic FRU handling.
 *
 **********************************************************************/

static ipmi_fru_record_t *
fru_record_alloc(int area)
{
    ipmi_fru_record_t *rec;
    unsigned short    extra_len = fru_area_info[area].extra_len;

    rec = ipmi_mem_alloc(sizeof(ipmi_fru_record_t) + extra_len);
    if (!rec)
	return NULL;

    memset(rec, 0, sizeof(ipmi_fru_record_t)+extra_len);

    rec->handlers = fru_area_info + area;
    rec->data = ((char *) rec) + sizeof(ipmi_fru_record_t);

    if (fru_area_info[area].setup_new)
	fru_area_info[area].setup_new(rec);

    return rec;
}

static void *
fru_record_get_data(ipmi_fru_record_t *rec)
{
    return rec->data;
}

static void
fru_record_free(ipmi_fru_record_t *rec)
{
    ipmi_mem_free(rec);
}


/***********************************************************************
 *
 * Various macros for common handling.
 *
 **********************************************************************/

#define HANDLE_STR_DECODE(ucname, fname, force_english) \
    err = fru_decode_string(fru, orig_data, &data, &data_len, u->lang_code, \
			    force_english, &u->fields,		\
			    ucname ## _ ## fname);		\
    if (err)							\
	goto out_err

#define HANDLE_CUSTOM_DECODE(ucname) \
do {									\
    while ((data_len > 0) && (*data != 0xc1)) {				\
	err = fru_decode_variable_string(fru, orig_data, &data, &data_len, \
					 u->lang_code,			\
					 &u->fields);			\
	if (err)							\
	    goto out_err;						\
    }									\
} while (0)

#define GET_DATA_PREFIX(lcname, ucname) \
    ipmi_fru_ ## lcname ## _area_t *u;				\
    ipmi_fru_record_t              **recs;			\
    ipmi_fru_record_t              *rec;			\
    if (!_ipmi_fru_is_normal_fru(fru))				\
	return ENOSYS;						\
    _ipmi_fru_lock(fru);					\
    recs = normal_fru_get_recs(fru);				\
    rec = recs[IPMI_FRU_FTR_## ucname ## _AREA];		\
    if (!rec) {							\
	_ipmi_fru_unlock(fru);					\
	return ENOSYS;						\
    }								\
    u = fru_record_get_data(rec);

#define GET_DATA_STR(lcname, ucname, fname) \
int									\
ipmi_fru_get_ ## lcname ## _ ## fname ## _len(ipmi_fru_t   *fru,	\
					      unsigned int *length)	\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_length(&u->fields,				\
				    ucname ## _ ## fname,		\
                                    length);				\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}									\
int									\
ipmi_fru_get_ ## lcname ## _ ## fname ## _type(ipmi_fru_t           *fru,\
					       enum ipmi_str_type_e *type)\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_type(&u->fields,				\
				  ucname ## _ ## fname,			\
                                  type);				\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}									\
int									\
ipmi_fru_get_ ## lcname ## _ ## fname(ipmi_fru_t	*fru,		\
				      char              *str,		\
				      unsigned int      *strlen)	\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_to_out(&u->fields,				\
				    ucname ## _ ## fname,		\
                                    str, strlen);			\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}									\
int									\
ipmi_fru_set_ ## lcname ## _ ## fname(ipmi_fru_t	   *fru,	\
				      enum ipmi_str_type_e type,	\
				      char                 *str,	\
				      unsigned int         len)		\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_set(rec,					\
				 &u->fields,				\
				 0, ucname ## _ ## fname,		\
                                 type, str, len, 0);			\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}

#define GET_CUSTOM_STR(lcname, ucname) \
int									\
ipmi_fru_get_ ## lcname ## _ ## custom ## _len(ipmi_fru_t   *fru,	\
					       unsigned int num,	\
					       unsigned int *length)	\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_length(&u->fields,				\
				    ucname ## _ ## custom_start + num,	\
                                    length);				\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}									\
int									\
ipmi_fru_get_ ## lcname ## _ ## custom ## _type(ipmi_fru_t   *fru,	\
					        unsigned int num,	\
					        enum ipmi_str_type_e *type) \
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_type(&u->fields,				\
				  ucname ## _ ## custom_start + num,	\
                                  type);				\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}									\
int									\
ipmi_fru_get_ ## lcname ## _ ## custom(ipmi_fru_t	 *fru,		\
				       unsigned int      num,		\
				       char              *str,		\
				       unsigned int      *strlen)	\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_to_out(&u->fields,				\
				    ucname ## _ ## custom_start + num,	\
                                    str, strlen);			\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}									\
int									\
ipmi_fru_set_ ## lcname ## _ ## custom(ipmi_fru_t	    *fru,	\
				       unsigned int         num,	\
				       enum ipmi_str_type_e type,	\
				       char                 *str,	\
				       unsigned int         len)	\
{									\
    int rv;								\
    GET_DATA_PREFIX(lcname, ucname);					\
    rv = fru_variable_string_set(rec,					\
				 &u->fields,				\
				 ucname ## _ ## custom_start, num,	\
                                 type, str, len, 1);			\
    _ipmi_fru_unlock(fru);						\
    return rv;								\
}

/***********************************************************************
 *
 * Handling for FRU internal use areas.
 *
 **********************************************************************/

typedef struct ipmi_fru_internal_use_area_s
{
    /* version bit 7-4 reserved (0000), bit 3-0 == 0001 */
    unsigned char  version;
    unsigned short length;
    unsigned char  *data;
} ipmi_fru_internal_use_area_t;


static void
internal_use_area_free(ipmi_fru_record_t *rec)
{
    ipmi_fru_internal_use_area_t *u = fru_record_get_data(rec);

    ipmi_mem_free(u->data);
    fru_record_free(rec);
}

static void
internal_use_area_setup(ipmi_fru_record_t *rec)
{
    ipmi_fru_internal_use_area_t *u = fru_record_get_data(rec);

    u->version = 1;
}

static int
fru_decode_internal_use_area(ipmi_fru_t        *fru,
			     unsigned char     *data,
			     unsigned int      data_len,
			     ipmi_fru_record_t **rrec)
{
    ipmi_fru_internal_use_area_t *u;
    ipmi_fru_record_t            *rec;

    rec = fru_record_alloc(IPMI_FRU_FTR_INTERNAL_USE_AREA);
    if (!rec)
	return ENOMEM;

    rec->length = data_len;
    rec->used_length = data_len;
    rec->orig_used_length = data_len;

    u = fru_record_get_data(rec);

    u->version = *data;
    u->length = data_len-1;
    u->data = ipmi_mem_alloc(u->length);
    if (!u->data) {
	ipmi_mem_free(rec);
	return ENOMEM;
    }

    memcpy(u->data, data+1, u->length);

    *rrec = rec;

    return 0;
}

int 
ipmi_fru_get_internal_use_version(ipmi_fru_t    *fru,
				  unsigned char *version)
{
    GET_DATA_PREFIX(internal_use, INTERNAL_USE);

    *version = u->version;

    _ipmi_fru_unlock(fru);

    return 0;
}

static int
ipmi_fru_set_internal_use_version(ipmi_fru_t *fru, unsigned char data)
{
    return EPERM;
}

int 
ipmi_fru_get_internal_use_len(ipmi_fru_t   *fru,
			      unsigned int *length)
{
    GET_DATA_PREFIX(internal_use, INTERNAL_USE);

    *length = u->length;

    _ipmi_fru_unlock(fru);

    return 0;
}


int 
ipmi_fru_get_internal_use(ipmi_fru_t    *fru,
			  unsigned char *data,
			  unsigned int  *max_len)
{
    int l;
    GET_DATA_PREFIX(internal_use, INTERNAL_USE);

    l = *max_len;

    if (l > u->length)
	l = u->length;

    memcpy(data, u->data, l);

    *max_len = l;

    _ipmi_fru_unlock(fru);

    return 0;
}

int
ipmi_fru_set_internal_use(ipmi_fru_t *fru, unsigned char *data,
			  unsigned int len)
{
    unsigned char *new_val;

    GET_DATA_PREFIX(internal_use, INTERNAL_USE);

    if (len > rec->length-1) {
	_ipmi_fru_unlock(fru);
	return E2BIG;
    }

    new_val = ipmi_mem_alloc(len);
    if (!new_val) {
	_ipmi_fru_unlock(fru);
	return ENOMEM;
    }
    if (u->data)
	ipmi_mem_free(u->data);
    u->data = new_val;
    memcpy(u->data, data, len);
    u->length = len;
    rec->changed = 1;
    rec->used_length = len + 1;
    rec->orig_used_length = rec->used_length;
    
    _ipmi_fru_unlock(fru);

    return 0;
}

static int
fru_encode_internal_use_area(ipmi_fru_t *fru, unsigned char *data)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    ipmi_fru_record_t *rec = recs[IPMI_FRU_FTR_INTERNAL_USE_AREA];
    ipmi_fru_internal_use_area_t *u;
    int               rv;

    if (!rec)
	return 0;

    u = fru_record_get_data(rec);
    data += rec->offset;
    memset(data, 0, rec->length);
    data[0] = 1; /* Version */
    memcpy(data+1, u->data, u->length);
    if (rec->changed && !rec->rewrite) {
	rv = _ipmi_fru_new_update_record(fru, rec->offset, u->length+1);
	if (rv)
	    return rv;
    }
    return 0;
}

/***********************************************************************
 *
 * Handling for FRU chassis info areas
 *
 **********************************************************************/

#define CHASSIS_INFO_part_number	0
#define CHASSIS_INFO_serial_number	1
#define CHASSIS_INFO_custom_start	2

typedef struct ipmi_fru_chassis_info_area_s
{
    /* version bit 7-4 reserved (0000), bit 3-0 == 0001 */
    unsigned char  version;
    unsigned char  type;  /* chassis type CT_xxxx */
    unsigned char  lang_code;
    fru_variable_t fields;
} ipmi_fru_chassis_info_area_t;

static void
chassis_info_area_free(ipmi_fru_record_t *rec)
{
    ipmi_fru_chassis_info_area_t *u = fru_record_get_data(rec);

    fru_free_variable_string(&u->fields);
    fru_record_free(rec);
}

static void
chassis_info_area_setup(ipmi_fru_record_t *rec)
{
    ipmi_fru_internal_use_area_t *u = fru_record_get_data(rec);

    u->version = 1;
}

static fru_variable_t *
chassis_info_get_fields(ipmi_fru_record_t *rec)
{
    ipmi_fru_chassis_info_area_t *u;
    u = fru_record_get_data(rec);
    return &u->fields;
}

static int
fru_decode_chassis_info_area(ipmi_fru_t        *fru,
			     unsigned char     *data,
			     unsigned int      data_len,
			     ipmi_fru_record_t **rrec)
{
    ipmi_fru_chassis_info_area_t *u;
    ipmi_fru_record_t            *rec;
    int                          err;
    unsigned char                version;
    unsigned char                length;
    unsigned char                *orig_data = data;

    version = *data;
    length = (*(data+1)) * 8;
    if ((length == 0) || (length > data_len)) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%snormal_fru.c(fru_decode_chassis_info_area):"
		 " FRU string goes past data length",
		 _ipmi_fru_get_iname(fru));
	return EBADF;
    }

    if (checksum(data, length) != 0) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%snormal_fru.c(fru_decode_chassis_info_area):"
		 " FRU string checksum failed",
		 _ipmi_fru_get_iname(fru));
	return EBADF;
    }

    data_len--; /* remove the checksum */

    rec = fru_record_alloc(IPMI_FRU_FTR_CHASSIS_INFO_AREA);
    if (!rec)
	return ENOMEM;

    err = fru_setup_min_field(rec, IPMI_FRU_FTR_CHASSIS_INFO_AREA, 0);
    if (err)
	goto out_err;

    rec->length = length; /* add 1 for the checksum */

    u = fru_record_get_data(rec);

    u->version = version;
    data += 2;
    data_len -= 2;
    u->type = *data;
    data++;
    data_len--;
    u->lang_code = IPMI_LANG_CODE_ENGLISH;
    HANDLE_STR_DECODE(CHASSIS_INFO, part_number, 1);
    HANDLE_STR_DECODE(CHASSIS_INFO, serial_number, 1);
    HANDLE_CUSTOM_DECODE(CHASSIS_INFO);
    rec->used_length = data - orig_data + 2; /* add 1 for the checksum, 1 for term */
    rec->orig_used_length = rec->used_length;

    *rrec = rec;

    return 0;

 out_err:
    chassis_info_area_free(rec);
    return err;
}

int 
ipmi_fru_get_chassis_info_version(ipmi_fru_t    *fru,
				  unsigned char *version)
{
    GET_DATA_PREFIX(chassis_info, CHASSIS_INFO);
    
    *version = u->version;

    _ipmi_fru_unlock(fru);

    return 0;
}

static int
ipmi_fru_set_chassis_info_version(ipmi_fru_t *fru, unsigned char data)
{
    return EPERM;
}

int 
ipmi_fru_get_chassis_info_type(ipmi_fru_t    *fru,
			       unsigned char *type)
{
    GET_DATA_PREFIX(chassis_info, CHASSIS_INFO);
    
    *type = u->type;

    _ipmi_fru_unlock(fru);

    return 0;
}

int 
ipmi_fru_set_chassis_info_type(ipmi_fru_t    *fru,
			       unsigned char type)
{
    GET_DATA_PREFIX(chassis_info, CHASSIS_INFO);

    rec->changed |= u->type != type;
    u->type = type;

    _ipmi_fru_unlock(fru);

    return 0;
}

GET_DATA_STR(chassis_info, CHASSIS_INFO, part_number)
GET_DATA_STR(chassis_info, CHASSIS_INFO, serial_number)
GET_CUSTOM_STR(chassis_info, CHASSIS_INFO)

static int
fru_encode_chassis_info_area(ipmi_fru_t *fru, unsigned char *data)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    ipmi_fru_record_t *rec = recs[IPMI_FRU_FTR_CHASSIS_INFO_AREA];
    ipmi_fru_chassis_info_area_t *u;
    int               rv;

    if (!rec)
	return 0;

    u = fru_record_get_data(rec);
    data += rec->offset;
    memset(data, 0, rec->length);
    data[0] = 1; /* Version */
    data[1] = rec->length / 8;
    data[2] = u->type;
    if (rec->changed && !rec->rewrite) {
	rv = _ipmi_fru_new_update_record(fru, rec->offset, 3);
	if (rv)
	    return rv;
    }
    rv = fru_encode_fields(fru, rec, &u->fields, data, 3);
    if (rv)
	return rv;
    data[rec->length-1] = -checksum(data, rec->length-1);
    if (rec->changed && !rec->rewrite) {
	/* Write any zeros that need to be written if the data got
	   shorter. */
	if (rec->used_length < rec->orig_used_length) {
	    rv = _ipmi_fru_new_update_record(fru,
					     rec->offset + rec->used_length - 1,
					     (rec->orig_used_length
					      - rec->used_length));
	    if (rv)
		return rv;
	}
	/* Write the checksum */
	rv = _ipmi_fru_new_update_record(fru, rec->offset+rec->length-1, 1);
	if (rv)
	    return rv;
    }
    return 0;
}

/***********************************************************************
 *
 * Handling for FRU board info areas
 *
 **********************************************************************/

#define BOARD_INFO_board_manufacturer	0
#define BOARD_INFO_board_product_name	1
#define BOARD_INFO_board_serial_number	2
#define BOARD_INFO_board_part_number	3
#define BOARD_INFO_fru_file_id		4
#define BOARD_INFO_custom_start		5

typedef struct ipmi_fru_board_info_area_s
{
    /* version bit 7-4 reserved (0000), bit 3-0 == 0001 */
    unsigned char  version;
    unsigned char  lang_code;
    time_t         mfg_time;
    fru_variable_t fields;
} ipmi_fru_board_info_area_t;

static void
board_info_area_free(ipmi_fru_record_t *rec)
{
    ipmi_fru_board_info_area_t *u = fru_record_get_data(rec);

    fru_free_variable_string(&u->fields);
    fru_record_free(rec);
}

static void
board_info_area_setup(ipmi_fru_record_t *rec)
{
    ipmi_fru_internal_use_area_t *u = fru_record_get_data(rec);

    u->version = 1;
}

static fru_variable_t *
board_info_get_fields(ipmi_fru_record_t *rec)
{
    ipmi_fru_board_info_area_t *u;
    u = fru_record_get_data(rec);
    return &u->fields;
}

static int
fru_decode_board_info_area(ipmi_fru_t        *fru,
			   unsigned char     *data,
			   unsigned int      data_len,
			   ipmi_fru_record_t **rrec)
{
    ipmi_fru_board_info_area_t *u;
    ipmi_fru_record_t          *rec;
    int                        err;
    unsigned char              version;
    unsigned char              length;
    unsigned char              *orig_data = data;

    version = *data;
    length = (*(data+1)) * 8;
    if ((length == 0) || (length > data_len)) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%snormal_fru.c(fru_decode_board_info_area):"
		 " FRU string goes past data length",
		 _ipmi_fru_get_iname(fru));
	return EBADF;
    }

    if (checksum(data, length) != 0) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%snormal_fru.c(fru_decode_board_info_area):"
		 " FRU string checksum failed",
		 _ipmi_fru_get_iname(fru));
	return EBADF;
    }

    data_len--; /* remove the checksum */

    rec = fru_record_alloc(IPMI_FRU_FTR_BOARD_INFO_AREA);
    if (!rec)
	return ENOMEM;

    err = fru_setup_min_field(rec, IPMI_FRU_FTR_BOARD_INFO_AREA, 0);
    if (err)
	goto out_err;

    rec->length = length;

    u = fru_record_get_data(rec);

    u->version = version;
    data += 2;
    data_len -= 2;
    u->lang_code = *data;
    if (u->lang_code == 0)
	u->lang_code = IPMI_LANG_CODE_ENGLISH;
    data++;
    data_len--;

    err = read_fru_time(&data, &data_len, &u->mfg_time);
    if (err)
	goto out_err;

    HANDLE_STR_DECODE(BOARD_INFO, board_manufacturer, 0);
    HANDLE_STR_DECODE(BOARD_INFO, board_product_name, 0);
    HANDLE_STR_DECODE(BOARD_INFO, board_serial_number, 1);
    HANDLE_STR_DECODE(BOARD_INFO, board_part_number, 1);
    HANDLE_STR_DECODE(BOARD_INFO, fru_file_id, 1);
    HANDLE_CUSTOM_DECODE(BOARD_INFO);
    rec->used_length = data - orig_data + 2; /* add 1 for the checksum, 1 for term */
    rec->orig_used_length = rec->used_length;

    *rrec = rec;

    return 0;

 out_err:
    board_info_area_free(rec);
    return err;
}

int 
ipmi_fru_get_board_info_version(ipmi_fru_t    *fru,
				unsigned char *version)
{
    GET_DATA_PREFIX(board_info, BOARD_INFO);
    
    *version = u->version;

    _ipmi_fru_unlock(fru);

    return 0;
}

static int
ipmi_fru_set_board_info_version(ipmi_fru_t *fru, unsigned char data)
{
    return EPERM;
}

int 
ipmi_fru_get_board_info_lang_code(ipmi_fru_t    *fru,
				  unsigned char *type)
{
    GET_DATA_PREFIX(board_info, BOARD_INFO);
    
    *type = u->lang_code;

    _ipmi_fru_unlock(fru);

    return 0;
}

int 
ipmi_fru_set_board_info_lang_code(ipmi_fru_t    *fru,
				  unsigned char lang)
{
    GET_DATA_PREFIX(board_info, BOARD_INFO);

    rec->changed |= u->lang_code != lang;
    u->lang_code = lang;

    _ipmi_fru_unlock(fru);

    return 0;
}

int 
ipmi_fru_get_board_info_mfg_time(ipmi_fru_t *fru,
				 time_t     *time)
{
    GET_DATA_PREFIX(board_info, BOARD_INFO);
    
    *time = u->mfg_time;

    _ipmi_fru_unlock(fru);

    return 0;
}

int 
ipmi_fru_set_board_info_mfg_time(ipmi_fru_t *fru,
				 time_t     time)
{
    GET_DATA_PREFIX(board_info, BOARD_INFO);
    
    rec->changed |= u->mfg_time != time;
    u->mfg_time = time;

    _ipmi_fru_unlock(fru);

    return 0;
}

GET_DATA_STR(board_info, BOARD_INFO, board_manufacturer)
GET_DATA_STR(board_info, BOARD_INFO, board_product_name)
GET_DATA_STR(board_info, BOARD_INFO, board_serial_number)
GET_DATA_STR(board_info, BOARD_INFO, board_part_number)
GET_DATA_STR(board_info, BOARD_INFO, fru_file_id)
GET_CUSTOM_STR(board_info, BOARD_INFO)

static int
fru_encode_board_info_area(ipmi_fru_t *fru, unsigned char *data)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    ipmi_fru_record_t *rec = recs[IPMI_FRU_FTR_BOARD_INFO_AREA];
    ipmi_fru_board_info_area_t *u;
    int               rv;

    if (!rec)
	return 0;

    u = fru_record_get_data(rec);
    data += rec->offset;
    data[0] = 1; /* Version */
    data[1] = rec->length / 8;
    data[2] = u->lang_code;
    write_fru_time(data+3, u->mfg_time);
    
    if (rec->changed && !rec->rewrite) {
	rv = _ipmi_fru_new_update_record(fru, rec->offset, 6);
	if (rv)
	    return rv;
    }
    rv = fru_encode_fields(fru, rec, &u->fields, data, 6);
    if (rv)
	return rv;
    data[rec->length-1] = -checksum(data, rec->length-1);
    if (rec->changed && !rec->rewrite) {
	/* Write any zeros that need to be written if the data got
	   shorter.  Subtract off 1 for the checksum since it is in
	   the used length */
	if (rec->used_length < rec->orig_used_length) {
	    rv = _ipmi_fru_new_update_record(fru,
					     rec->offset + rec->used_length - 1,
					     (rec->orig_used_length
					      - rec->used_length));
	    if (rv)
		return rv;
	}
	/* Write the checksum */
	rv = _ipmi_fru_new_update_record(fru, rec->offset+rec->length-1, 1);
	if (rv)
	    return rv;
    }
    return 0;
}

/***********************************************************************
 *
 * Handling for FRU product info areas
 *
 **********************************************************************/

#define PRODUCT_INFO_manufacturer_name		0
#define PRODUCT_INFO_product_name		1
#define PRODUCT_INFO_product_part_model_number	2
#define PRODUCT_INFO_product_version		3
#define PRODUCT_INFO_product_serial_number	4
#define PRODUCT_INFO_asset_tag			5
#define PRODUCT_INFO_fru_file_id		6
#define PRODUCT_INFO_custom_start		7

typedef struct ipmi_fru_product_info_area_s
{
    /* version bit 7-4 reserved (0000), bit 3-0 == 0001 */
    unsigned char  version;
    unsigned char  lang_code;
    fru_variable_t fields;
} ipmi_fru_product_info_area_t;

static void
product_info_area_free(ipmi_fru_record_t *rec)
{
    ipmi_fru_product_info_area_t *u = fru_record_get_data(rec);

    fru_free_variable_string(&u->fields);
    fru_record_free(rec);
}

static void
product_info_area_setup(ipmi_fru_record_t *rec)
{
    ipmi_fru_internal_use_area_t *u = fru_record_get_data(rec);

    u->version = 1;
}

static fru_variable_t *
product_info_get_fields(ipmi_fru_record_t *rec)
{
    ipmi_fru_product_info_area_t *u;
    u = fru_record_get_data(rec);
    return &u->fields;
}

static int
fru_decode_product_info_area(ipmi_fru_t        *fru,
			     unsigned char     *data,
			     unsigned int      data_len,
			     ipmi_fru_record_t **rrec)
{
    ipmi_fru_product_info_area_t *u;
    ipmi_fru_record_t            *rec;
    int                          err;
    unsigned char                version;
    unsigned int                 length;
    unsigned char                *orig_data = data;

    version = *data;
    length = (*(data+1)) * 8;
    if ((length == 0) || (length > data_len)) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%snormal_fru.c(fru_decode_product_info_area):"
		 " FRU string goes past data length",
		 _ipmi_fru_get_iname(fru));
	return EBADF;
    }

    if (checksum(data, length) != 0) {
	ipmi_log(IPMI_LOG_ERR_INFO,
		 "%snormal_fru.c(fru_decode_product_info_area):"
		 " FRU string checksum failed",
		 _ipmi_fru_get_iname(fru));
	return EBADF;
    }

    data_len--; /* remove the checksum */

    rec = fru_record_alloc(IPMI_FRU_FTR_PRODUCT_INFO_AREA);
    if (!rec)
	return ENOMEM;

    err = fru_setup_min_field(rec, IPMI_FRU_FTR_PRODUCT_INFO_AREA, 0);
    if (err)
	goto out_err;

    rec->length = length;

    u = fru_record_get_data(rec);

    u->version = version;
    data += 2;
    data_len -= 2;
    u->lang_code = *data;
    if (u->lang_code == 0)
	u->lang_code = IPMI_LANG_CODE_ENGLISH;
    data++;
    data_len--;
    HANDLE_STR_DECODE(PRODUCT_INFO, manufacturer_name, 0);
    HANDLE_STR_DECODE(PRODUCT_INFO, product_name, 0);
    HANDLE_STR_DECODE(PRODUCT_INFO, product_part_model_number, 0);
    HANDLE_STR_DECODE(PRODUCT_INFO, product_version, 0);
    HANDLE_STR_DECODE(PRODUCT_INFO, product_serial_number, 1);
    HANDLE_STR_DECODE(PRODUCT_INFO, asset_tag, 0);
    HANDLE_STR_DECODE(PRODUCT_INFO, fru_file_id, 1);
    HANDLE_CUSTOM_DECODE(PRODUCT_INFO);
    rec->used_length = data - orig_data + 2; /* add 1 for the checksum, 1 for term */
    rec->orig_used_length = rec->used_length;

    *rrec = rec;

    return 0;

 out_err:
    product_info_area_free(rec);
    return err;
}

int 
ipmi_fru_get_product_info_version(ipmi_fru_t    *fru,
				  unsigned char *version)
{
    GET_DATA_PREFIX(product_info, PRODUCT_INFO);
    
    *version = u->version;

    _ipmi_fru_unlock(fru);

    return 0;
}

static int
ipmi_fru_set_product_info_version(ipmi_fru_t *fru, unsigned char data)
{
    return EPERM;
}

int 
ipmi_fru_get_product_info_lang_code(ipmi_fru_t    *fru,
				    unsigned char *type)
{
    GET_DATA_PREFIX(product_info, PRODUCT_INFO);
    
    *type = u->lang_code;

    _ipmi_fru_unlock(fru);

    return 0;
}

int 
ipmi_fru_set_product_info_lang_code(ipmi_fru_t    *fru,
				    unsigned char lang)
{
    GET_DATA_PREFIX(product_info, PRODUCT_INFO);
    
    rec->changed |= u->lang_code != lang;
    u->lang_code = lang;

    _ipmi_fru_unlock(fru);

    return 0;
}

GET_DATA_STR(product_info, PRODUCT_INFO, manufacturer_name)
GET_DATA_STR(product_info, PRODUCT_INFO, product_name)
GET_DATA_STR(product_info, PRODUCT_INFO, product_part_model_number)
GET_DATA_STR(product_info, PRODUCT_INFO, product_version)
GET_DATA_STR(product_info, PRODUCT_INFO, product_serial_number)
GET_DATA_STR(product_info, PRODUCT_INFO, asset_tag)
GET_DATA_STR(product_info, PRODUCT_INFO, fru_file_id)
GET_CUSTOM_STR(product_info, PRODUCT_INFO)

static int
fru_encode_product_info_area(ipmi_fru_t *fru, unsigned char *data)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    ipmi_fru_record_t *rec = recs[IPMI_FRU_FTR_PRODUCT_INFO_AREA];
    ipmi_fru_product_info_area_t *u;
    int               rv;

    if (!rec)
	return 0;

    u = fru_record_get_data(rec);
    data += rec->offset;
    memset(data, 0, rec->length);
    data[0] = 1; /* Version */
    data[1] = rec->length / 8;
    data[2] = u->lang_code;
    
    if (rec->changed && !rec->rewrite) {
	rv = _ipmi_fru_new_update_record(fru, rec->offset, 3);
	if (rv)
	    return rv;
    }
    rv = fru_encode_fields(fru, rec, &u->fields, data, 3);
    if (rv)
	return rv;
	/* Write any zeros that need to be written if the data got
	   shorter. */
    data[rec->length-1] = -checksum(data, rec->length-1);
    if (rec->changed && !rec->rewrite) {
	if (rec->used_length < rec->orig_used_length) {
	    rv = _ipmi_fru_new_update_record(fru,
					     rec->offset + rec->used_length - 1,
					     (rec->orig_used_length
					      - rec->used_length));
	    if (rv)
		return rv;
	}
	/* Write the checksum */
	rv = _ipmi_fru_new_update_record(fru, rec->offset+rec->length-1, 1);
	if (rv)
	    return rv;
    }
    return 0;
}

/***********************************************************************
 *
 * Handling for FRU multi-records
 *
 **********************************************************************/
typedef struct ipmi_fru_record_elem_s
{
    /* Where relative to the beginning of the record area does this
       record start? */
    unsigned int  offset;

    /* Has this record been changed (needs to be written)? */
    char          changed;

    unsigned char type;
    unsigned char format_version;
    unsigned char length;
    unsigned char *data;
} ipmi_fru_record_elem_t;

typedef struct ipmi_fru_multi_record_s
{
    /* Actual length of the array. */
    unsigned int           rec_len;

    /* Number of used elements in the array */
    unsigned int           num_records;
    ipmi_fru_record_elem_t *records;

    /* Dummy field to keep the macros happy */
    int                    version;
} ipmi_fru_multi_record_area_t;

static void
multi_record_area_free(ipmi_fru_record_t *rec)
{
    ipmi_fru_multi_record_area_t *u = fru_record_get_data(rec);
    int                          i;

    if (u->records) {
	for (i=0; i<u->num_records; i++) {
	    if (u->records[i].data)
		ipmi_mem_free(u->records[i].data);
	}
	ipmi_mem_free(u->records);
    }
    fru_record_free(rec);
}

static int
fru_decode_multi_record_area(ipmi_fru_t        *fru,
			     unsigned char     *data,
			     unsigned int      data_len,
			     ipmi_fru_record_t **rrec)
{
    ipmi_fru_record_t       *rec;
    int                     err;
    int                     i;
    unsigned int            num_records;
    unsigned char           *orig_data = data;
    unsigned int            orig_data_len = data_len;
    ipmi_fru_multi_record_area_t *u;
    ipmi_fru_record_elem_t  *r;
    unsigned char           sum;
    unsigned int            length;
    unsigned int            start_offset = 0;
    unsigned int            left = data_len;

    /* First scan for the number of records. */
    num_records = 0;
    for (;;) {
	unsigned char eol;

	if (left < 5) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%snormal_fru.c(fru_decode_multi_record_area):"
		     " Data not long enough for multi record",
		     _ipmi_fru_get_iname(fru));
	    return EBADF;
	}

	if (checksum(data, 5) != 0) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%snormal_fru.c(fru_decode_multi_record_area):"
		     " Header checksum for record %d failed",
		     _ipmi_fru_get_iname(fru), num_records+1);
	    return EBADF;
	}

	length = data[2];
	if ((length + 5) > left) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%snormal_fru.c(fru_decode_multi_record_area):"
		     " Record went past end of data",
		     _ipmi_fru_get_iname(fru));
	    return EBADF;
	}

	sum = checksum(data+5, length) + data[3];
	if (sum != 0) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%snormal_fru.c(fru_decode_multi_record_area):"
		     " Data checksum for record %d failed",
		     _ipmi_fru_get_iname(fru), num_records+1);
	    return EBADF;
	}

	num_records++;

	eol = data[1] & 0x80;

	data += length + 5;
	left -= length + 5;

	if (eol)
	    /* End of list */
	    break;
    }

    rec = fru_record_alloc(IPMI_FRU_FTR_MULTI_RECORD_AREA);
    if (!rec)
	return ENOMEM;

    rec->length = data_len;
    rec->used_length = data - orig_data;
    rec->orig_used_length = rec->used_length;

    u = fru_record_get_data(rec);
    u->num_records = num_records;
    u->rec_len = num_records;
    u->records = ipmi_mem_alloc(sizeof(ipmi_fru_record_elem_t) * num_records);
    if (!u->records) {
	err = ENOMEM;
	goto out_err;
    }
    memset(u->records, 0, sizeof(ipmi_fru_record_elem_t) * num_records);

    data = orig_data;
    data_len = orig_data_len;
    for (i=0; i<num_records; i++) {
	/* No checks required, they've already been done above. */
	length = data[2];
	r = u->records + i;
	if (length == 0)
	    r->data = ipmi_mem_alloc(1);
	else
	    r->data = ipmi_mem_alloc(length);
	if (!r->data) {
	    err = ENOMEM;
	    goto out_err;
	}

	memcpy(r->data, data+5, length);
	r->length = length;
	r->type = data[0];
	r->format_version = data[1] & 0xf;
	r->offset = start_offset;

	data += length + 5;
	start_offset += length + 5;
    }

    *rrec = rec;

    return 0;

 out_err:
    multi_record_area_free(rec);
    return err;
}

unsigned int
ipmi_fru_get_num_multi_records(ipmi_fru_t *fru)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;
    unsigned int                 num;

    if (!_ipmi_fru_is_normal_fru(fru))
	return 0;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	_ipmi_fru_unlock(fru);
	return 0;
    }

    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
    num = u->num_records;
    _ipmi_fru_unlock(fru);
    return num;
}

int
ipmi_fru_get_multi_record_type(ipmi_fru_t    *fru,
			       unsigned int  num,
			       unsigned char *type)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	_ipmi_fru_unlock(fru);
	return ENOSYS;
    }
    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
    if (num >= u->num_records) {
	_ipmi_fru_unlock(fru);
	return E2BIG;
    }
    *type = u->records[num].type;
    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_get_multi_record_format_version(ipmi_fru_t    *fru,
					 unsigned int  num,
					 unsigned char *ver)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	_ipmi_fru_unlock(fru);
	return ENOSYS;
    }
    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
    if (num >= u->num_records) {
	_ipmi_fru_unlock(fru);
	return E2BIG;
    }
    *ver = u->records[num].format_version;
    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_get_multi_record_data_len(ipmi_fru_t   *fru,
				   unsigned int num,
				   unsigned int *len)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	_ipmi_fru_unlock(fru);
	return ENOSYS;
    }
    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
    if (num >= u->num_records) {
	_ipmi_fru_unlock(fru);
	return E2BIG;
    }
    *len = u->records[num].length;
    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_get_multi_record_data(ipmi_fru_t    *fru,
			       unsigned int  num,
			       unsigned char *data,
			       unsigned int  *length)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	_ipmi_fru_unlock(fru);
	return ENOSYS;
    }
    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
    if (num >= u->num_records) {
	_ipmi_fru_unlock(fru);
	return E2BIG;
    }
    if (*length < u->records[num].length) {
	_ipmi_fru_unlock(fru);
	return EINVAL;
    }
    memcpy(data, u->records[num].data, u->records[num].length);
    *length = u->records[num].length;
    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_set_multi_record(ipmi_fru_t    *fru,
			  unsigned int  num,
			  unsigned char type,
			  unsigned char version,
			  unsigned char *data,
			  unsigned int  length)
{
    normal_fru_rec_data_t        *info = _ipmi_fru_get_rec_data(fru);
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;
    unsigned char                *new_data;
    ipmi_fru_record_t            *rec;
    int                          raw_diff = 0;
    int                          i;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    rec = recs[IPMI_FRU_FTR_MULTI_RECORD_AREA];
    if (!rec) {
	_ipmi_fru_unlock(fru);
	return ENOSYS;
    }

    u = fru_record_get_data(rec);

    if (num >= u->num_records) {
	if (!data) {
	    /* Don't expand if we are deleting an invalid field,
	       return an error. */
	    _ipmi_fru_unlock(fru);
	    return EINVAL;
	}

	num = u->num_records;
	/* If not enough room, expand the array by a set amount (to
	   keep from thrashing memory when adding lots of things). */
	if (u->num_records >= u->rec_len) {
	    unsigned int           new_len = u->rec_len + 16;
	    ipmi_fru_record_elem_t *new_recs;

	    new_recs = ipmi_mem_alloc(new_len * sizeof(*new_recs));
	    if (!new_recs) {
		_ipmi_fru_unlock(fru);
		return ENOMEM;
	    }
	    memset(new_recs, 0, new_len * sizeof(*new_recs));
	    if (u->records) {
		memcpy(new_recs, u->records, u->rec_len * sizeof(*new_recs));
		ipmi_mem_free(u->records);
	    }
	    u->records = new_recs;
	    u->rec_len = new_len;
	}
	if (u->num_records == 0)
	    info->header_changed = 1;
	u->num_records++;
	u->records[num].offset = rec->used_length;
	u->records[num].length = 0;
	u->records[num].changed = 1;
	u->records[num].data = NULL;
	raw_diff = 5; /* Header size */
    }

    if (data) {
	raw_diff += length - u->records[num].length;

	/* Is there enough space? */
	if ((rec->used_length + raw_diff) > rec->length)
	    return ENOSPC;

	/* Modifying the record. */
	if (length == 0)
	    new_data = ipmi_mem_alloc(1);
	else
	    new_data = ipmi_mem_alloc(length);
	if (!new_data) {
	    _ipmi_fru_unlock(fru);
	    return ENOMEM;
	}
	memcpy(new_data, data, length);
	if (u->records[num].data)
	    ipmi_mem_free(u->records[num].data);
	u->records[num].data = new_data;
	u->records[num].type = type;
	u->records[num].format_version = version;
	u->records[num].length = length;
	if (raw_diff) {
	    for (i=num+1; i<u->num_records; i++) {
		u->records[i].offset += raw_diff;
		u->records[i].changed = 1;
	    }
	}
    } else {
	/* Deleting the record. */
	if (u->records[num].data)
	    ipmi_mem_free(u->records[num].data);
	u->num_records--;
	raw_diff = - (5 + u->records[num].length);
	for (i=num; i<u->num_records; i++) {
	    u->records[i] = u->records[i+1];
	    u->records[i].offset += raw_diff;
	    u->records[i].changed = 1;
	}
	if (u->num_records == 0)
	    /* Need to write "0" for the multi-records. */
	    info->header_changed = 1;
    }

    rec->used_length += raw_diff;
    rec->changed |= 1;
    _ipmi_fru_unlock(fru);
    return 0;
}

static int
fru_encode_multi_record(ipmi_fru_t             *fru,
			ipmi_fru_record_t      *rec,
			ipmi_fru_multi_record_area_t *u,
			int                    idx,
			unsigned char          *data,
			unsigned int           *offset)
{
    unsigned int           o = *offset;
    ipmi_fru_record_elem_t *elem = u->records + idx;
    int                    rv;

    if (o != elem->offset)
	return EBADF;

    data += o;
    data[0] = elem->type;
    data[1] = 2; /* Version */
    if (idx+1 == u->num_records)
	data[1] |= 0x80; /* Last record */
    data[2] = elem->length;
    data[3] = -checksum(elem->data, elem->length);
    data[4] = -checksum(data, 4);
    memcpy(data+5, elem->data, elem->length);

    if (rec->changed && !rec->rewrite) {
	rv = _ipmi_fru_new_update_record(fru, rec->offset+elem->offset,
					 elem->length+5);
	if (rv)
	    return rv;
    }

    *offset = o + elem->length + 5;
    return 0;
}

static int
fru_encode_multi_record_area(ipmi_fru_t *fru, unsigned char *data)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    ipmi_fru_record_t *rec = recs[IPMI_FRU_FTR_MULTI_RECORD_AREA];
    ipmi_fru_multi_record_area_t *u;
    int               rv;
    int               i;
    unsigned int      offset;

    if (!rec)
	return 0;

    u = fru_record_get_data(rec);
    data += rec->offset;
    memset(data, 0, rec->length);

    if (u->num_records == 0)
	return 0;
    
    offset = 0;
    for (i=0; i<u->num_records; i++) {
	rv = fru_encode_multi_record(fru, rec, u, i, data, &offset);
	if (rv)
	    return rv;
    }
    return 0;
}


/***********************************************************************
 *
 * Area processing
 *
 **********************************************************************/

static fru_area_info_t fru_area_info[IPMI_FRU_FTR_NUMBER] = 
{
    { 0, 0,  1, NULL,                    internal_use_area_free,
      sizeof(ipmi_fru_internal_use_area_t),
      fru_decode_internal_use_area, fru_encode_internal_use_area,
      internal_use_area_setup },
    { 2, 3,  7, chassis_info_get_fields, chassis_info_area_free,
      sizeof(ipmi_fru_chassis_info_area_t),
      fru_decode_chassis_info_area, fru_encode_chassis_info_area,
      chassis_info_area_setup },
    { 5, 6, 13, board_info_get_fields,   board_info_area_free,
      sizeof(ipmi_fru_board_info_area_t),
      fru_decode_board_info_area, fru_encode_board_info_area,
      board_info_area_setup },
    { 7, 3, 12, product_info_get_fields, product_info_area_free,
      sizeof(ipmi_fru_product_info_area_t),
      fru_decode_product_info_area, fru_encode_product_info_area,
      product_info_area_setup },
    { 0, 0,  0, NULL,                    multi_record_area_free,
      sizeof(ipmi_fru_multi_record_area_t),
      fru_decode_multi_record_area, fru_encode_multi_record_area,
      NULL },
};

static int
check_rec_position(ipmi_fru_t   *fru,
		   int          recn,
		   unsigned int offset,
		   unsigned int length)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    int               pos;
    unsigned int      data_len = _ipmi_fru_get_data_len(fru);
    int               max_start = data_len - 8;

    /* Zero is invalid, and it must be a multiple of 8. */
    if ((offset == 0) || ((offset % 8) != 0))
	return EINVAL;

    /* Make sure the used area still fits. */
    if (recs[recn] && (length < recs[recn]->used_length))
	return E2BIG;

    /* FRU data record starts cannot exceed 2040 bytes.  The offsets
       are in multiples of 8 and the sizes are 8-bits, thus 8 *
       255.  The end of the data can go till the end of the FRU. */
    if (max_start > 2040)
	max_start = 2040;
    if ((offset > max_start) || ((offset + length) > data_len))
	return EINVAL;

    /* Check that this is not in the previous record's space. */
    pos = recn - 1;
    while ((pos >= 0) && !recs[pos])
	pos--;
    if (pos >= 0) {
	if (offset < (recs[pos]->offset + recs[pos]->length))
	    return EINVAL;
    }

    /* Check that this is not in the next record's space. */
    pos = recn + 1;
    while ((pos < IPMI_FRU_FTR_NUMBER) && !recs[pos])
	pos++;
    if (pos < IPMI_FRU_FTR_NUMBER) {
	if ((offset + length) > recs[pos]->offset)
	    return EINVAL;
    }

    return 0;
}

int
ipmi_fru_add_area(ipmi_fru_t   *fru,
		  unsigned int area,
		  unsigned int offset,
		  unsigned int length)
{
    normal_fru_rec_data_t *info = _ipmi_fru_get_rec_data(fru);
    ipmi_fru_record_t     **recs;
    ipmi_fru_record_t     *rec;
    int                   rv;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;

    /* Truncate the length to a multiple of 8. */
    length = length & ~(8-1);

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (recs[area]) {
	_ipmi_fru_unlock(fru);
	return EEXIST;
    }

    rv = check_rec_position(fru, area, offset, length);
    if (rv) {
	_ipmi_fru_unlock(fru);
	return rv;
    }

    rec = fru_record_alloc(area);
    if (!rec) {
	_ipmi_fru_unlock(fru);
	return ENOMEM;
    }
    rec->changed = 1;
    rec->rewrite = 1;
    rec->used_length = fru_area_info[area].empty_length;
    rec->orig_used_length = rec->used_length;
    rec->offset = offset;
    rec->length = length;
    info->header_changed = 1;

    rv = fru_setup_min_field(rec, area, 1);
    if (rv) {
	_ipmi_fru_unlock(fru);
	return rv;
    }

    recs[area] = rec;
    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_delete_area(ipmi_fru_t *fru, int area)
{
    ipmi_fru_record_t **recs;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    fru_record_destroy(recs[area]); 
    recs[area] = NULL;
    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_area_get_offset(ipmi_fru_t   *fru,
			 unsigned int area,
			 unsigned int *offset)
{
    ipmi_fru_record_t **recs;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;
    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[area]) {
	_ipmi_fru_unlock(fru);
	return ENOENT;
    }

    *offset = recs[area]->offset;

    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_area_get_length(ipmi_fru_t   *fru,
			 unsigned int area,
			 unsigned int *length)
{
    ipmi_fru_record_t **recs;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[area]) {
	_ipmi_fru_unlock(fru);
	return ENOENT;
    }

    *length = recs[area]->length;

    _ipmi_fru_unlock(fru);
    return 0;
}

int
ipmi_fru_area_set_offset(ipmi_fru_t   *fru,
			 unsigned int area,
			 unsigned int offset)
{
    normal_fru_rec_data_t *info = _ipmi_fru_get_rec_data(fru);
    ipmi_fru_record_t     **recs;
    int                   rv;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[area]) {
	_ipmi_fru_unlock(fru);
	return ENOENT;
    }

    if (recs[area]->offset == offset) {
	_ipmi_fru_unlock(fru);
	return 0;
    }

    if (area == IPMI_FRU_FTR_MULTI_RECORD_AREA) {
	/* Multi-record lengths are not defined, but just goto the end.
	   So adjust the length for comparison here. */
	int newlength = (recs[area]->length
			 + recs[area]->offset - offset);
	rv = check_rec_position(fru, area, offset, newlength);
    } else {
	rv = check_rec_position(fru, area, offset, recs[area]->length);
    }
    if (!rv) {
	if (area == IPMI_FRU_FTR_MULTI_RECORD_AREA)
	    recs[area]->length += recs[area]->offset - offset;
	recs[area]->offset = offset;
	recs[area]->changed = 1;
	recs[area]->rewrite = 1;
	info->header_changed = 1;
    }

    _ipmi_fru_unlock(fru);
    return rv;
}

int
ipmi_fru_area_set_length(ipmi_fru_t   *fru,
			 unsigned int area,
			 unsigned int length)
{
    ipmi_fru_record_t **recs;
    int               rv;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    /* Truncate the length to a multiple of 8. */
    length = length & ~(8-1);

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;
    if (length == 0)
	return EINVAL;
    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[area]) {
	_ipmi_fru_unlock(fru);
	return ENOENT;
    }

    if (recs[area]->length == length) {
	_ipmi_fru_unlock(fru);
	return 0;
    }

    rv = check_rec_position(fru, area, recs[area]->offset, length);
    if (!rv) {
	if (length > recs[area]->length)
	    /* Only need to rewrite the whole record (to get the zeroes
	       into the unused area) if we increase the length. */
	    recs[area]->rewrite = 1;
	recs[area]->length = length;
	recs[area]->changed = 1;
    }

    _ipmi_fru_unlock(fru);
    return rv;
}

int
ipmi_fru_area_get_used_length(ipmi_fru_t *fru,
			      unsigned int area,
			      unsigned int *used_length)
{
    ipmi_fru_record_t **recs;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    if (area >= IPMI_FRU_FTR_NUMBER)
	return EINVAL;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[area]) {
	_ipmi_fru_unlock(fru);
	return ENOENT;
    }

    *used_length = recs[area]->used_length;

    _ipmi_fru_unlock(fru);
    return 0;
}

/***********************************************************************
 *
 * Handling for FRU generic interface.
 *
 **********************************************************************/

typedef struct fru_data_rep_s
{
    char                      *name;
    enum ipmi_fru_data_type_e type;
    int                       hasnum;

    union {
	struct {
	    int (*fetch_uchar)(ipmi_fru_t *fru, unsigned char *data);
	    int (*set_uchar)(ipmi_fru_t *fru, unsigned char data);
	} inttype;

	struct {
	    int (*fetch_uchar)(ipmi_fru_t *fru, unsigned int num,
			       unsigned char *data);
	    int (*set_uchar)(ipmi_fru_t *fru, unsigned int num,
			     unsigned char data);
	} intnumtype;

	struct {
	    int (*fetch)(ipmi_fru_t *fru, double *data);
	    int (*set)(ipmi_fru_t *fru, double data);
	} floattype;

	struct {
	    int (*fetch)(ipmi_fru_t *fru, unsigned int num, double *data);
	    int (*set)(ipmi_fru_t *fru, unsigned int num, double data);
	} floatnumtype;

	struct {
	    int (*fetch)(ipmi_fru_t *fru, time_t *data);
	    int (*set)(ipmi_fru_t *fru, time_t data);
	} timetype;

	struct {
	    int (*fetch)(ipmi_fru_t *fru, unsigned int num,
			 time_t *data);
	    int (*set)(ipmi_fru_t *fru, unsigned int num,
		       time_t data);
	} timenumtype;

	struct {
	    int (*fetch_len)(ipmi_fru_t *fru, unsigned int *len);
	    int (*fetch_type)(ipmi_fru_t *fru, enum ipmi_str_type_e *type);
	    int (*fetch_data)(ipmi_fru_t *fru, char *data,
			      unsigned int *max_len);
	    int (*set)(ipmi_fru_t *fru, enum ipmi_str_type_e type,
		       char *data, unsigned int len);
	} strtype;

	struct {
	    int (*fetch_len)(ipmi_fru_t *fru, unsigned int num,
			     unsigned int *len);
	    int (*fetch_type)(ipmi_fru_t *fru, unsigned int num,
			      enum ipmi_str_type_e *type);
	    int (*fetch_data)(ipmi_fru_t *fru, unsigned int num,
			      char *data, unsigned int *max_len);
	    int (*set)(ipmi_fru_t *fru, unsigned int num,
		       enum ipmi_str_type_e type, char *data,
		       unsigned int len);
	} strnumtype;

	struct {
	    int (*fetch_len)(ipmi_fru_t *fru, unsigned int *len);
	    int (*fetch_data)(ipmi_fru_t *fru, unsigned char *data,
			      unsigned int *max_len);
	    int (*set)(ipmi_fru_t *fru, unsigned char *data,
		       unsigned int len);
	} bintype;

	struct {
	    int (*fetch_len)(ipmi_fru_t *fru, unsigned int num,
			     unsigned int *len);
	    int (*fetch_data)(ipmi_fru_t *fru, unsigned intnum,
			      unsigned char *data, unsigned int *max_len);
	    int (*set)(ipmi_fru_t *fru, unsigned int num,
		       unsigned char *data, unsigned int len);
	} binnumtype;
    } u;
} fru_data_rep_t;

#define F_UCHAR(x) { .name = #x, .type = IPMI_FRU_DATA_INT, .hasnum = 0, \
		     .u = { .inttype = { .fetch_uchar = ipmi_fru_get_ ## x, \
					 .set_uchar = ipmi_fru_set_ ## x }}}
#define F_NUM_UCHAR(x) { .name = #x, .type = IPMI_FRU_DATA_INT, .hasnum = 1, \
		         .u = { .intnumtype = {				     \
				 .fetch_uchar = ipmi_fru_get_ ## x,	     \
				 .set_uchar = ipmi_fru_set_ ## x }}}
#define F_TIME(x) { .name = #x, .type = IPMI_FRU_DATA_TIME, .hasnum = 0, \
		    .u = { .timetype = { .fetch = ipmi_fru_get_ ## x,    \
					 .set = ipmi_fru_set_ ## x }}}
#define F_NUM_TIME(x) { .name = #x, .type = IPMI_FRU_DATA_TIME, .hasnum = 1, \
		        .u = { .timenumtype = { .fetch = ipmi_fru_get_ ## x, \
					        .set = ipmi_fru_set_ ## x }}}
#define F_STR(x) { .name = #x, .type = IPMI_FRU_DATA_ASCII, .hasnum = 0, \
		   .u = { .strtype = {					     \
			  .fetch_len = ipmi_fru_get_ ## x ## _len, \
		          .fetch_type = ipmi_fru_get_ ## x ## _type, \
		          .fetch_data = ipmi_fru_get_ ## x, \
			  .set = ipmi_fru_set_ ## x }}}
#define F_NUM_STR(x) { .name = #x, .type = IPMI_FRU_DATA_ASCII, .hasnum = 1, \
		       .u = { .strnumtype = {                                \
			      .fetch_len = ipmi_fru_get_ ## x ## _len, \
		              .fetch_type = ipmi_fru_get_ ## x ## _type,\
		              .fetch_data = ipmi_fru_get_ ## x, \
			      .set = ipmi_fru_set_ ## x }}}
#define F_BIN(x) { .name = #x, .type = IPMI_FRU_DATA_BINARY, .hasnum = 0, \
		   .u = { .bintype = {					     \
			  .fetch_len = ipmi_fru_get_ ## x ## _len, \
		   	  .fetch_data = ipmi_fru_get_ ## x, \
			  .set = ipmi_fru_set_ ## x }}}
#define F_NUM_BIN(x) { .name = #x, .type = IPMI_FRU_DATA_BINARY, .hasnum = 1, \
		       .u = { .binnumtype = {				      \
			      .fetch_len = ipmi_fru_get_ ## x ## _len, \
		       	      .fetch_data = ipmi_fru_get_ ## x, \
			      .set = ipmi_fru_set_ ## x }}}
static fru_data_rep_t frul[] =
{
    F_UCHAR(internal_use_version),
    F_BIN(internal_use),
    F_UCHAR(chassis_info_version),
    F_UCHAR(chassis_info_type),
    F_STR(chassis_info_part_number),
    F_STR(chassis_info_serial_number),
    F_NUM_STR(chassis_info_custom),
    F_UCHAR(board_info_version),
    F_UCHAR(board_info_lang_code),
    F_TIME(board_info_mfg_time),
    F_STR(board_info_board_manufacturer),
    F_STR(board_info_board_product_name),
    F_STR(board_info_board_serial_number),
    F_STR(board_info_board_part_number),
    F_STR(board_info_fru_file_id),
    F_NUM_STR(board_info_custom),
    F_UCHAR(product_info_version),
    F_UCHAR(product_info_lang_code),
    F_STR(product_info_manufacturer_name),
    F_STR(product_info_product_name),
    F_STR(product_info_product_part_model_number),
    F_STR(product_info_product_version),
    F_STR(product_info_product_serial_number),
    F_STR(product_info_asset_tag),
    F_STR(product_info_fru_file_id),
    F_NUM_STR(product_info_custom),
};
#define NUM_FRUL_ENTRIES (sizeof(frul) / sizeof(fru_data_rep_t))

int
ipmi_fru_str_to_index(char *name)
{
    int i;
    for (i=0; i<NUM_FRUL_ENTRIES; i++) {
	if (strcmp(name, frul[i].name) == 0)
	    return i;
    }
    return -1;
}

char *
ipmi_fru_index_to_str(int index)
{
    if ((index < 0) || (index >= NUM_FRUL_ENTRIES))
	return NULL;

    return frul[index].name;
}

int
ipmi_fru_get(ipmi_fru_t                *fru,
	     int                       index,
	     const char                **name,
	     int                       *num,
	     enum ipmi_fru_data_type_e *dtype,
	     int                       *intval,
	     time_t                    *time,
	     char                      **data,
	     unsigned int              *data_len)
{
    fru_data_rep_t *p;
    unsigned char  ucval, dummy_ucval;
    unsigned int   dummy_uint;
    time_t         dummy_time;
    int            rv = 0, rv2 = 0;
    unsigned int   len;
    char           *dval = NULL;
    enum ipmi_fru_data_type_e rdtype;
    enum ipmi_str_type_e stype;
    

    if ((index < 0) || (index >= NUM_FRUL_ENTRIES))
	return EINVAL;

    p = frul + index;

    if (name)
	*name = p->name;

    rdtype = p->type;

    switch (p->type) {
    case IPMI_FRU_DATA_INT:
	if (intval) {
	    if (! p->hasnum) {
		rv = p->u.inttype.fetch_uchar(fru, &ucval);
	    } else {
		rv = p->u.intnumtype.fetch_uchar(fru, *num, &ucval);
		rv2 = p->u.intnumtype.fetch_uchar(fru, (*num)+1, &dummy_ucval);
	    }
	    if (!rv)
		*intval = ucval;
	}
	break;

    case IPMI_FRU_DATA_TIME:
	if (time) {
	    if (! p->hasnum) {
		rv = p->u.timetype.fetch(fru, time);
	    } else {
		rv = p->u.timenumtype.fetch(fru, *num, time);
		rv2 = p->u.timenumtype.fetch(fru, (*num)+1, &dummy_time);
	    }
	}
	break;

    case IPMI_FRU_DATA_ASCII:
	if (dtype) {
	    if (! p->hasnum) {
		rv = p->u.strtype.fetch_type(fru, &stype);
	    } else {
		rv = p->u.strnumtype.fetch_type(fru, *num, &stype);
	    }
	    if (rv) {
		break;
	    } else {
		switch (stype) {
		case IPMI_UNICODE_STR: rdtype = IPMI_FRU_DATA_UNICODE; break;
		case IPMI_BINARY_STR: rdtype = IPMI_FRU_DATA_BINARY; break;
		case IPMI_ASCII_STR: break;
		}
	    }
	}

	if (data_len || data) {
	    if (! p->hasnum) {
		rv = p->u.strtype.fetch_len(fru, &len);
	    } else {
		rv = p->u.strnumtype.fetch_len(fru, *num, &len);
	    }
	    if (rv)
		break;

	    if (data) {
		dval = ipmi_mem_alloc(len);
		if (!dval) {
		    rv = ENOMEM;
		    break;
		}
		if (! p->hasnum) {
		    rv = p->u.strtype.fetch_data(fru, dval, &len);
		} else {
		    rv = p->u.strnumtype.fetch_data(fru, *num, dval, &len);
		}
		if (rv)
		    break;
		*data = dval;
	    }

	    if (data_len)
		*data_len = len;
	}

	if (p->hasnum)
	    rv2 = p->u.strnumtype.fetch_len(fru, (*num)+1, &dummy_uint);
	break;

    case IPMI_FRU_DATA_BINARY:
	if (data_len || data) {
	    if (! p->hasnum) {
		rv = p->u.bintype.fetch_len(fru, &len);
	    } else {
		rv = p->u.binnumtype.fetch_len(fru, *num, &len);
	    }
	    if (rv)
		break;

	    if (data) {
		dval = ipmi_mem_alloc(len);
		if (!dval) {
		    rv = ENOMEM;
		    break;
		}
		if (! p->hasnum) {
		    rv = p->u.bintype.fetch_data(fru, (char *) dval, &len);
		} else {
		    rv = p->u.binnumtype.fetch_data(fru, *num, (char *) dval,
						    &len);
		}
		if (rv)
		    break;
		*data = dval;
	    }

	    if (data_len)
		*data_len = len;
	}

	if (p->hasnum)
	    rv2 = p->u.binnumtype.fetch_len(fru, (*num)+1, &dummy_uint);
	break;

    default:
	break;
    }

    if (rv) {
	if (dval)
	    ipmi_mem_free(dval);
	return rv;
    }

    if (p->hasnum) {
	if (rv2)
	    *num = -1;
	else
	    *num = (*num) + 1;
    }

    if (dtype)
	*dtype = rdtype;

    return 0;
}

int
ipmi_fru_set_int_val(ipmi_fru_t *fru,
		     int        index,
		     int        num,
		     int        val)
{
    fru_data_rep_t *p;
    int            rv;

    if ((index < 0) || (index >= NUM_FRUL_ENTRIES))
	return EINVAL;

    p = frul + index;

    if (p->type != IPMI_FRU_DATA_INT)
	return EINVAL;

    if (! p->hasnum) {
	rv = p->u.inttype.set_uchar(fru, val);
    } else {
	rv = p->u.intnumtype.set_uchar(fru, num, val);
    }

    return rv;
}

int
ipmi_fru_set_float_val(ipmi_fru_t *fru,
		       int        index,
		       int        num,
		       double     val)
{
    fru_data_rep_t *p;
    int            rv;

    if ((index < 0) || (index >= NUM_FRUL_ENTRIES))
	return EINVAL;

    p = frul + index;

    if (p->type != IPMI_FRU_DATA_FLOAT)
	return EINVAL;

    if (! p->hasnum) {
	rv = p->u.floattype.set(fru, val);
    } else {
	rv = p->u.floatnumtype.set(fru, num, val);
    }

    return rv;
}

int
ipmi_fru_set_time_val(ipmi_fru_t *fru,
		      int        index,
		      int        num,
		      time_t     val)
{
    fru_data_rep_t *p;
    int            rv;
    

    if ((index < 0) || (index >= NUM_FRUL_ENTRIES))
	return EINVAL;

    p = frul + index;

    if (p->type != IPMI_FRU_DATA_TIME)
	return EINVAL;

    if (! p->hasnum) {
	rv = p->u.timetype.set(fru, val);
    } else {
	rv = p->u.timenumtype.set(fru, num, val);
    }

    return rv;
}

int
ipmi_fru_set_data_val(ipmi_fru_t                *fru,
		      int                       index,
		      int                       num,
		      enum ipmi_fru_data_type_e dtype,
		      char                      *data,
		      unsigned int              len)
{
    fru_data_rep_t       *p;
    int                  rv;
    enum ipmi_str_type_e stype;
    

    if ((index < 0) || (index >= NUM_FRUL_ENTRIES))
	return EINVAL;

    p = frul + index;

    switch (dtype) {
    case IPMI_FRU_DATA_UNICODE: stype = IPMI_UNICODE_STR; break;
    case IPMI_FRU_DATA_BINARY: stype = IPMI_BINARY_STR; break;
    case IPMI_FRU_DATA_ASCII: stype = IPMI_ASCII_STR; break;
    default:
	return EINVAL;
    }

    switch (p->type)
    {
    case IPMI_FRU_DATA_UNICODE:
    case IPMI_FRU_DATA_ASCII:
	if (! p->hasnum) {
	    rv = p->u.strtype.set(fru, stype, data, len);
	} else {
	    rv = p->u.strnumtype.set(fru, num, stype, data, len);
	}
	break;

    case IPMI_FRU_DATA_BINARY:
	if (! p->hasnum) {
	    rv = p->u.bintype.set(fru, data, len);
	} else {
	    rv = p->u.binnumtype.set(fru, num, data, len);
	}
	break;

    default:
	return EINVAL;
    }

    return rv;
}

/***********************************************************************
 *
 * FRU node handling
 *
 **********************************************************************/
static void
fru_node_destroy(ipmi_fru_node_t *node)
{
    ipmi_fru_t *fru = _ipmi_fru_node_get_data(node);

    ipmi_fru_deref(fru);
}

typedef struct fru_mr_array_idx_s
{
    int             index;
    const char      *name;
    ipmi_fru_node_t *mr_node;
    ipmi_fru_t      *fru;
} fru_mr_array_idx_t;

static void
fru_mr_array_idx_destroy(ipmi_fru_node_t *node)
{
    fru_mr_array_idx_t *info = _ipmi_fru_node_get_data(node);
    ipmi_fru_t         *fru = info->fru;

    ipmi_fru_deref(fru);
    ipmi_fru_put_node(info->mr_node);
    ipmi_mem_free(info);
}

static int
fru_mr_array_idx_get_field(ipmi_fru_node_t           *pnode,
			   unsigned int              index,
			   const char                **name,
			   enum ipmi_fru_data_type_e *dtype,
			   int                       *intval,
			   time_t                    *time,
			   double                    *floatval,
			   char                      **data,
			   unsigned int              *data_len,
			   ipmi_fru_node_t           **sub_node)
{
    fru_mr_array_idx_t *info = _ipmi_fru_node_get_data(pnode);
    int                rv;
    unsigned int       rlen;
    unsigned char      *rdata;

    if (index == 0) {
	/* Raw FRU data */
	rv = ipmi_fru_get_multi_record_data_len(info->fru, info->index, &rlen);
	if (rv)
	    return rv;
	if (data) {
	    rdata = ipmi_mem_alloc(rlen);
	    if (!rdata)
		return ENOMEM;
	    rv = ipmi_fru_get_multi_record_data(info->fru, info->index, rdata,
						&rlen);
	    if (rv) {
		ipmi_mem_free(rdata);
		return rv;
	    }
	    *data = rdata;
	}

	if (data_len)
	    *data_len = rlen;

	if (dtype)
	    *dtype = IPMI_FRU_DATA_BINARY;

	if (name)
	    *name = "raw-data";

	return 0;
    } else if (index == 1) {
	/* FRU node itself. */
	if (info->mr_node == NULL)
	    return EINVAL;

	if (intval)
	    *intval = -1;
	if (name)
	    *name = info->name;
	if (dtype)
	    *dtype = IPMI_FRU_DATA_SUB_NODE;
	if (sub_node) {
	    ipmi_fru_get_node(info->mr_node);
	    *sub_node = info->mr_node;
	}
	return 0;
    } else
	return EINVAL;
}

static int
fru_mr_array_get_field(ipmi_fru_node_t           *pnode,
		       unsigned int              index,
		       const char                **name,
		       enum ipmi_fru_data_type_e *dtype,
		       int                       *intval,
		       time_t                    *time,
		       double                    *floatval,
		       char                      **data,
		       unsigned int              *data_len,
		       ipmi_fru_node_t           **sub_node)
{
    fru_mr_array_idx_t *info;
    ipmi_fru_t         *fru = _ipmi_fru_node_get_data(pnode);
    ipmi_fru_node_t    *node;
    ipmi_fru_node_t    *snode;
    const char         *sname;
    int                rv = 0;

    if (index >= ipmi_fru_get_num_multi_records(fru))
	return EINVAL;

    if (name)
	*name = NULL;
    if (dtype)
	*dtype = IPMI_FRU_DATA_SUB_NODE;
    if (intval)
	*intval = -1;
    if (sub_node) {
	node = _ipmi_fru_node_alloc(fru);
	if (!node)
	    return ENOMEM;
	info = ipmi_mem_alloc(sizeof(*info));
	if (!info) {
	    ipmi_fru_put_node(node);
	    return ENOMEM;
	}
	memset(info, 0, sizeof(*info));
	info->index = index;
	info->fru = fru;
	ipmi_fru_ref(fru);
	_ipmi_fru_node_set_data(node, info);

	rv = ipmi_fru_multi_record_get_root_node(fru, index, &sname, &snode);
	if (rv) {
	    /* No decode data, just do a "raw" node. */
	    info->mr_node = NULL;
	    info->name = "multirecord";
	} else {
	    info->mr_node = snode;
	    info->name = sname;
	}
	_ipmi_fru_node_set_get_field(node, fru_mr_array_idx_get_field);
	_ipmi_fru_node_set_destructor(node, fru_mr_array_idx_destroy);

	*sub_node = node;
    }
    return rv;
}

typedef struct fru_array_s
{
    int        index;
    ipmi_fru_t *fru;
} fru_array_t;

static void
fru_array_idx_destroy(ipmi_fru_node_t *node)
{
    fru_array_t *info = _ipmi_fru_node_get_data(node);
    ipmi_fru_t  *fru = info->fru;

    ipmi_fru_deref(fru);
    ipmi_mem_free(info);
}

static int
fru_array_idx_get_field(ipmi_fru_node_t           *pnode,
			unsigned int              index,
			const char                **name,
			enum ipmi_fru_data_type_e *dtype,
			int                       *intval,
			time_t                    *time,
			double                    *floatval,
			char                      **data,
			unsigned int              *data_len,
			ipmi_fru_node_t           **sub_node)
{
    fru_array_t *info = _ipmi_fru_node_get_data(pnode);
    int         num = index;
    int         rv;

    if (name)
	*name = NULL;

    rv = ipmi_fru_get(info->fru, info->index, NULL, &num, dtype,
		      intval, time, data, data_len);
    if ((rv == E2BIG) || (rv == ENOSYS))
	rv = EINVAL;
    return rv;
}

static int
fru_node_get_field(ipmi_fru_node_t           *pnode,
		   unsigned int              index,
		   const char                **name,
		   enum ipmi_fru_data_type_e *dtype,
		   int                       *intval,
		   time_t                    *time,
		   double                    *floatval,
		   char                      **data,
		   unsigned int              *data_len,
		   ipmi_fru_node_t           **sub_node)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;
    ipmi_fru_t                   *fru = _ipmi_fru_node_get_data(pnode);
    ipmi_fru_node_t              *node;
    int                          rv;
    int                          num;
    int                          len;

    if ((index >= 0) && (index < NUM_FRUL_ENTRIES)) {
	num = 0;
	rv = ipmi_fru_get(fru, index, name, &num, NULL, NULL, NULL, NULL,
			  NULL);
	if (rv)
	    return rv;

	if (num != 0) {
	    fru_array_t *info;
	    /* name is set by the previous call */
	    if (dtype)
		*dtype = IPMI_FRU_DATA_SUB_NODE;
	    if (intval) {
		/* Get the length of the array by searching. */
		len = 1;
		while (num != -1) {
		    len++;
		    rv = ipmi_fru_get(fru, index, NULL, &num, NULL, NULL,
				      NULL, NULL, NULL);
		    if (rv)
			return rv;
		}
		*intval = len;
	    }
	    if (sub_node) {
		node = _ipmi_fru_node_alloc(fru);
		if (!node)
		    return ENOMEM;
		info = ipmi_mem_alloc(sizeof(*info));
		if (!info) {
		    ipmi_fru_put_node(node);
		    return ENOMEM;
		}
		info->index = index;
		info->fru = fru;
		_ipmi_fru_node_set_data(node, info);
		_ipmi_fru_node_set_get_field(node, fru_array_idx_get_field);
		_ipmi_fru_node_set_destructor(node, fru_array_idx_destroy);
		ipmi_fru_ref(fru);

		*sub_node = node;
	    }
	    return 0;
	} else
	    /* Not an array, everything is ok. */
	    return ipmi_fru_get(fru, index, name, NULL, dtype, intval, time,
				data, data_len);

    } else if (index == NUM_FRUL_ENTRIES) {
	/* Handle multi-records. */
	_ipmi_fru_lock(fru);
	recs = normal_fru_get_recs(fru);
	if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	    _ipmi_fru_unlock(fru);
	    return ENOSYS;
	}
	if (intval) {
	    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
	    *intval = u->num_records;
	}
	_ipmi_fru_unlock(fru);

	if (name)
	    *name = "multirecords";
	if (dtype)
	    *dtype = IPMI_FRU_DATA_SUB_NODE;
	if (sub_node) {
	    node = _ipmi_fru_node_alloc(fru);
	    if (!node)
		return ENOMEM;
	    _ipmi_fru_node_set_data(node, fru);
	    _ipmi_fru_node_set_get_field(node, fru_mr_array_get_field);
	    _ipmi_fru_node_set_destructor(node, fru_node_destroy);
	    ipmi_fru_ref(fru);

	    *sub_node = node;
	}
	return 0;
    } else
	return EINVAL;
}

/***********************************************************************
 *
 * Normal-fru-specific processing
 *
 **********************************************************************/
static void
fru_record_destroy(ipmi_fru_record_t *rec)
{
    if (rec)
	rec->handlers->free(rec);
}

static void
fru_cleanup_recs(ipmi_fru_t *fru)
{
    normal_fru_rec_data_t *info = _ipmi_fru_get_rec_data(fru);
    int                   i;

    if (!info)
	return;

    for (i=0; i<IPMI_FRU_FTR_NUMBER; i++)
	fru_record_destroy(info->recs[i]);

    ipmi_mem_free(info);
    _ipmi_fru_set_rec_data(fru, NULL);
}

static void
fru_write_complete(ipmi_fru_t *fru)
{
    ipmi_fru_record_t **recs = normal_fru_get_recs(fru);
    int               i;

    for (i=0; i<IPMI_FRU_FTR_NUMBER; i++) {
	ipmi_fru_record_t *rec = recs[i];
	if (rec) {
	    rec->rewrite = 0;
	    rec->changed = 0;
	    rec->orig_used_length = rec->used_length;
	    if (rec->handlers->get_fields) {
		fru_variable_t *f = rec->handlers->get_fields(rec);
		int j;
		for (j=0; j<f->next; j++)
		    f->strings[i].changed = 0;
	    }
	}
    }
}

static int
fru_write(ipmi_fru_t *fru)
{
    normal_fru_rec_data_t *info = _ipmi_fru_get_rec_data(fru);
    ipmi_fru_record_t     **recs = normal_fru_get_recs(fru);
    int                   i;
    int                   rv;
    unsigned char         *data = _ipmi_fru_get_data_ptr(fru);

    data[0] = 1; /* Version */
    for (i=0; i<IPMI_FRU_FTR_MULTI_RECORD_AREA; i++) {
	if (recs[i])
	    data[i+1] = recs[i]->offset / 8;
	else
	    data[i+1] = 0;
    }
    if (recs[i] && recs[i]->used_length)
	data[i+1] = recs[i]->offset / 8;
    else
	data[i+1] = 0;
    data[6] = 0;
    data[7] = -checksum(data, 7);

    if (info->header_changed) {
	rv = _ipmi_fru_new_update_record(fru, 0, 8);
	if (rv)
	    return rv;
    }

    for (i=0; i<IPMI_FRU_FTR_NUMBER; i++) {
	ipmi_fru_record_t *rec = recs[i];

	if (rec) {
	    rv = rec->handlers->encode(fru, data);
	    if (rv)
		return rv;
	    if (rec->rewrite) {
		if (i == IPMI_FRU_FTR_MULTI_RECORD_AREA)
		    rv = _ipmi_fru_new_update_record(fru, rec->offset,
						     rec->used_length);
		else
		    rv = _ipmi_fru_new_update_record(fru, rec->offset,
						     rec->length);
		if (rv)
		    return rv;
		
	    }
	}
    }    

    return 0;
}

static int
fru_get_root_node(ipmi_fru_t *fru, const char **name, ipmi_fru_node_t **rnode)
{
    ipmi_fru_node_t *node;

    if (name)
	*name = "standard FRU";
    if (rnode) {
	node = _ipmi_fru_node_alloc(fru);
	if (!node)
	    return ENOMEM;
	_ipmi_fru_node_set_data(node, fru);
	_ipmi_fru_node_set_get_field(node, fru_node_get_field);
	_ipmi_fru_node_set_destructor(node, fru_node_destroy);
	ipmi_fru_ref(fru);
	*rnode = node;
    }
    return 0;
}

/************************************************************************
 *
 * For OEM-specific FRU multi-record decode and field get
 *
 ************************************************************************/

static locked_list_t *fru_multi_record_oem_handlers;

typedef struct fru_multi_record_oem_handlers_s {
    unsigned int                               manufacturer_id;
    unsigned char                              record_type_id;
    ipmi_fru_oem_multi_record_get_root_node_cb get_root;
    void                                       *cb_data;
} fru_multi_record_oem_handlers_t;

int
_ipmi_fru_register_multi_record_oem_handler
(unsigned int                               manufacturer_id,
 unsigned char                              record_type_id,
 ipmi_fru_oem_multi_record_get_root_node_cb get_root,
 void                                       *cb_data)
{
    fru_multi_record_oem_handlers_t *new_item;

    new_item = ipmi_mem_alloc(sizeof(*new_item));
    if (!new_item)
	return ENOMEM;

    new_item->manufacturer_id = manufacturer_id;
    new_item->record_type_id = record_type_id;
    new_item->get_root = get_root;
    new_item->cb_data = cb_data;

    if (!locked_list_add(fru_multi_record_oem_handlers, new_item, NULL)) {
        ipmi_mem_free(new_item);
	return ENOMEM;
    }
    return 0;
}

static int
fru_multi_record_oem_handler_cmp_dereg(void *cb_data, void *item1, void *item2)
{
    fru_multi_record_oem_handlers_t *hndlr = item1;
    fru_multi_record_oem_handlers_t *cmp = cb_data;

    if ((hndlr->manufacturer_id == cmp->manufacturer_id)
	&& (hndlr->record_type_id == cmp->record_type_id))
    {
	/* We re-use the cb_data as a marker to tell we found it. */
        cmp->cb_data = cmp;
        locked_list_remove(fru_multi_record_oem_handlers, item1, item2);
        ipmi_mem_free(hndlr);
	return LOCKED_LIST_ITER_STOP;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

int
_ipmi_fru_deregister_multi_record_oem_handler(unsigned int manufacturer_id,
					      unsigned char record_type_id)
{
    fru_multi_record_oem_handlers_t tmp;

    tmp.manufacturer_id = manufacturer_id;
    tmp.record_type_id = record_type_id;
    tmp.cb_data = NULL;
    locked_list_iterate(fru_multi_record_oem_handlers,
                        fru_multi_record_oem_handler_cmp_dereg,
                        &tmp);
    if (!tmp.cb_data)
	return ENOENT;
    return 0;
}

typedef struct oem_search_node_s
{
    unsigned int    manufacturer_id;
    unsigned char   record_type_id;
    ipmi_fru_t      *fru;
    ipmi_fru_node_t *node;
    unsigned char   *mr_data;
    unsigned char   mr_data_len;
    const char      *name;
    int             rv;
} oem_search_node_t;

static int
get_root_node(void *cb_data, void *item1, void *item2)
{
    fru_multi_record_oem_handlers_t *hndlr = item1;
    oem_search_node_t               *cmp = cb_data;

    if ((hndlr->record_type_id == cmp->record_type_id)
	&& ((hndlr->record_type_id < 0xc0)
	    || (hndlr->manufacturer_id == cmp->manufacturer_id)))
    {
	cmp->rv = hndlr->get_root(cmp->fru, cmp->manufacturer_id,
				  cmp->record_type_id,
				  cmp->mr_data, cmp->mr_data_len,
				  hndlr->cb_data, &cmp->name, &cmp->node);
	
	return LOCKED_LIST_ITER_STOP;
    } else {
        cmp->rv = EINVAL;
    }
    return LOCKED_LIST_ITER_CONTINUE;
}

int
ipmi_fru_multi_record_get_root_node(ipmi_fru_t      *fru,
				    unsigned int    record_num,
				    const char      **name,
				    ipmi_fru_node_t **node)
{
    ipmi_fru_record_t            **recs;
    ipmi_fru_multi_record_area_t *u;
    unsigned char                *d;
    oem_search_node_t            cmp;

    if (!_ipmi_fru_is_normal_fru(fru))
	return ENOSYS;

    _ipmi_fru_lock(fru);
    recs = normal_fru_get_recs(fru);
    if (!recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]) {
	_ipmi_fru_unlock(fru);
	return ENOSYS;
    }
    u = fru_record_get_data(recs[IPMI_FRU_FTR_MULTI_RECORD_AREA]);
    if (record_num >= u->num_records) {
	_ipmi_fru_unlock(fru);
	return E2BIG;
    }
    if (u->records[record_num].length < 3) {
	_ipmi_fru_unlock(fru);
	return EINVAL;
    }

    d = u->records[record_num].data;
    cmp.manufacturer_id = d[0] | (d[1] << 8) | (d[2] << 16);
    cmp.record_type_id = u->records[record_num].type;
    cmp.fru = fru;
    cmp.node = NULL;
    cmp.mr_data = d;
    cmp.mr_data_len = u->records[record_num].length;
    cmp.name = NULL;
    cmp.rv = 0;
    locked_list_iterate(fru_multi_record_oem_handlers, get_root_node, &cmp);
    _ipmi_fru_unlock(fru);
    if (cmp.rv)
	return cmp.rv;
    if (node)
	*node = cmp.node;
    else
	ipmi_fru_put_node(cmp.node);
    if (name)
	*name = cmp.name;
    return 0;
}

/************************************************************************
 *
 * Standard multi-record handlers.
 *
 ************************************************************************/

static int
convert_int_to_fru_int(const char                *name,
		       int                       val,
		       const char                **rname,
		       enum ipmi_fru_data_type_e *dtype,
		       int                       *intval)
{
    if (rname)
	*rname = name;
    if (dtype)
	*dtype = IPMI_FRU_DATA_INT;
    if (intval)
	*intval = val;
    return 0;
}

static int
convert_float_to_fru_float(const char                *name,
			   double                    val,
			   const char                **rname,
			   enum ipmi_fru_data_type_e *dtype,
			   double                    *floatval)
{
    if (rname)
	*rname = name;
    if (dtype)
	*dtype = IPMI_FRU_DATA_FLOAT;
    if (floatval)
	*floatval = val;
    return 0;
}

static int
convert_int_to_fru_boolean(const char                *name,
			   int                       val,
			   const char                **rname,
			   enum ipmi_fru_data_type_e *dtype,
			   int                       *intval)
{
    if (rname)
	*rname = name;
    if (dtype)
	*dtype = IPMI_FRU_DATA_BOOLEAN;
    if (intval)
	*intval = val != 0;
    return 0;
}

typedef struct std_power_supply_info_s
{
    unsigned char data[24];
} std_power_supply_info_t;

static void std_power_supply_info_cleanup_rec(std_power_supply_info_t *rec)
{
    ipmi_mem_free(rec);
}

static void
std_power_supply_info_root_destroy(ipmi_fru_node_t *node)
{
    std_power_supply_info_t *rec = _ipmi_fru_node_get_data(node);
    std_power_supply_info_cleanup_rec(rec);
}

static int
std_power_supply_info_get_field(ipmi_fru_node_t           *pnode,
				unsigned int              index,
				const char                **name,
				enum ipmi_fru_data_type_e *dtype,
				int                       *intval,
				time_t                    *time,
				double                    *floatval,
				char                      **data,
				unsigned int              *data_len,
				ipmi_fru_node_t           **sub_node)
{
    std_power_supply_info_t *rec = _ipmi_fru_node_get_data(pnode);
    unsigned char           *d = rec->data;
    int                     rv = 0;
    int                     val;
    double                  fval;

    switch(index) {
    case 0: /* overall capacity */
	rv = convert_int_to_fru_int("overall capacity",
				    (d[0] | (d[1] << 8)) & 0x0fff,
				    name, dtype, intval);
	break;

    case 1: /* Peak VA */
	val = d[2] | (d[3] << 8);
	if (val == 0xffff)
	    rv = ENOSYS;
	else
	    rv = convert_int_to_fru_int("peak VA", val, name, dtype, intval);
	break;

    case 2: /* inrush current */
	if (d[4] == 0xff)
	    rv = ENOSYS;
	else
	    rv = convert_int_to_fru_int("inrush current", d[4],
					name, dtype, intval);
	break;

    case 3: /* inrush interval (in ms) */
	if (d[4] == 0xff) /* Yes, d[4] is correct, not valid if inrush
			     current not specified. */
	    rv = ENOSYS;
	else {
	    fval = ((double) d[4]) / 1000.0;
	    rv = convert_float_to_fru_float("inrush interval", fval,
					    name, dtype, floatval);
	}
	break;

    case 4: /* low input voltage 1 */
	fval = ((double) (d[6] | (d[7] << 8))) / 100.0;
	rv = convert_float_to_fru_float("low input voltage 1", fval,
					name, dtype, floatval);
	break;

    case 5: /* high input voltage 1 */
	fval = ((double) (d[8] | (d[9] << 8))) / 100.0;
	rv = convert_float_to_fru_float("high input voltage 1", fval,
					name, dtype, floatval);
	break;

    case 6: /* low input voltage 2 */
	fval = ((double) (d[10] | (d[11] << 8))) / 100.0;
	rv = convert_float_to_fru_float("low input voltage 2", fval,
					name, dtype, floatval);
	break;

    case 7: /* high input voltage 2 */
	fval = ((double) (d[12] | (d[13] << 8))) / 100.0;
	rv = convert_float_to_fru_float("high input voltage 2", fval,
					name, dtype, floatval);
	break;

    case 8: /* low frequency */
	rv = convert_int_to_fru_int("low frequency", d[14],
				    name, dtype, intval);
	break;

    case 9: /* high frequency */
	rv = convert_int_to_fru_int("low frequency", d[15],
				    name, dtype, intval);
	break;

    case 10: /* A/C dropout tolerance (in ms) */
	fval = ((double) d[4]) / 1000.0;
	rv = convert_float_to_fru_float("A/C dropout tolerance", fval,
					name, dtype, floatval);
	break;

    case 11: /* tach pulses per rotation */
	rv = convert_int_to_fru_boolean("tach pulses per rotation",
					d[17] & 0x10,
					name, dtype, intval);
	break;

    case 12: /* hot swap support */
	rv = convert_int_to_fru_boolean("hot swap support", d[17] & 0x08,
					name, dtype, intval);
	break;

    case 13: /* autoswitch */
	rv = convert_int_to_fru_boolean("autoswitch", d[17] & 0x04,
					name, dtype, intval);
	break;

    case 14: /* power factor correction */
	rv = convert_int_to_fru_boolean("power factor correction",
					d[17] & 0x02,
					name, dtype, intval);
	break;

    case 15: /* predictive fail support */
	rv = convert_int_to_fru_boolean("predictive fail support",
					d[17] & 0x01,
					name, dtype, intval);
	break;

    case 16: /* peak capacity hold up time (in sec) */
	rv = convert_int_to_fru_int("peak capacity hold up time", d[19] >> 4,
				    name, dtype, intval);
	break;

    case 17: /* peak capacity */
	rv = convert_int_to_fru_int("peak capacity",
				    (d[18] | (d[19] << 8)) & 0xfff,
				    name, dtype, intval);
	break;

    case 18: /* combined wattage voltage 1 */
	if ((d[20] == 0) && (d[21] == 0) && (d[22] == 0))
	    return ENOSYS;
	switch (d[20] >> 4) {
	case 0: fval = 12.0; break;
	case 1: fval = -12.0; break;
	case 2: fval = 5.0; break;
	case 3: fval = 3.3; break;
	default: fval = 0.0;
	}
	rv = convert_float_to_fru_float("combined wattage voltage 1", fval,
					name, dtype, floatval);
	break;

    case 19: /* combined wattage voltage 2 */
	if ((d[20] == 0) && (d[21] == 0) && (d[22] == 0))
	    return ENOSYS;
	switch (d[20] & 0x0f) {
	case 0: fval = 12.0; break;
	case 1: fval = -12.0; break;
	case 2: fval = 5.0; break;
	case 3: fval = 3.3; break;
	default: fval = 0.0;
	}
	rv = convert_float_to_fru_float("combined wattage voltage 2", fval,
					name, dtype, floatval);
	break;

    case 20: /* combined wattage */
	if ((d[20] == 0) && (d[21] == 0) && (d[22] == 0))
	    return ENOSYS;
	rv = convert_int_to_fru_int("combined wattage", d[21] | (d[22] << 8),
				    name, dtype, intval);
	break;

    case 21: /* predictive fail tack low threshold */
	rv = convert_int_to_fru_int("predictive fail tack low threshold",
				    d[23] & 0x0f,
				    name, dtype, intval);
	break;

    default:
	rv = EINVAL;
    }

    return rv;
}

static int
std_get_power_supply_info_root(ipmi_fru_t          *fru,
			       unsigned char       *mr_data,
			       unsigned int        mr_data_len,
			       const char          **name,
			       ipmi_fru_node_t     **rnode)
{
    std_power_supply_info_t *rec;
    ipmi_fru_node_t         *node;
    int                     rv;

    if (mr_data_len < 24)
	return EINVAL;
    
    rec = ipmi_mem_alloc(sizeof(*rec));
    if (!rec)
	return ENOMEM;
    memcpy(rec->data, mr_data, 24);

    node = _ipmi_fru_node_alloc(fru);
    if (!node)
	goto out_no_mem;

    _ipmi_fru_node_set_data(node, rec);
    _ipmi_fru_node_set_get_field(node, std_power_supply_info_get_field);
    _ipmi_fru_node_set_destructor(node, std_power_supply_info_root_destroy);
    *rnode = node;

    if (name)
	*name = "Power Supply Information";

    return 0;

 out_no_mem:
    rv = ENOMEM;
    goto out_cleanup;

 out_cleanup:
    std_power_supply_info_cleanup_rec(rec);
    return rv;
}

typedef struct std_dc_output_s
{
    unsigned char data[13];
} std_dc_output_t;

static void std_dc_output_cleanup_rec(std_dc_output_t *rec)
{
    ipmi_mem_free(rec);
}

static void
std_dc_output_root_destroy(ipmi_fru_node_t *node)
{
    std_dc_output_t *rec = _ipmi_fru_node_get_data(node);
    std_dc_output_cleanup_rec(rec);
}

static int
std_dc_output_get_field(ipmi_fru_node_t           *pnode,
			unsigned int              index,
			const char                **name,
			enum ipmi_fru_data_type_e *dtype,
			int                       *intval,
			time_t                    *time,
			double                    *floatval,
			char                      **data,
			unsigned int              *data_len,
			ipmi_fru_node_t           **sub_node)
{
    std_dc_output_t *rec = _ipmi_fru_node_get_data(pnode);
    unsigned char   *d = rec->data;
    int             rv = 0;
    double          fval;
    int16_t         val16;

    switch(index) {
    case 0: /* output number */
	rv = convert_int_to_fru_int("output number", d[0] & 0x0f,
				    name, dtype, intval);
	break;

    case 1: /* standby */
	rv = convert_int_to_fru_boolean("standby", d[0] & 0x80,
					name, dtype, intval);
	break;

    case 2: /* nominal voltage */
	val16 = d[1] | (d[2] << 8);
	fval = ((double) val16) / 100.0;
	rv = convert_float_to_fru_float("nominal voltage", fval,
					name, dtype, floatval);
	break;

    case 3: /* max negative voltage deviation */
	val16 = d[3] | (d[4] << 8);
	fval = ((double) val16) / 100.0;
	rv = convert_float_to_fru_float("max negative voltage deviation", fval,
					name, dtype, floatval);
	break;

    case 4: /* max positive voltage deviation */
	val16 = d[5] | (d[6] << 8);
	fval = ((double) val16) / 100.0;
	rv = convert_float_to_fru_float("max positive voltage deviation", fval,
					name, dtype, floatval);
	break;

    case 5: /* ripple */
	val16 = d[7] | (d[8] << 8);
	fval = ((double) val16) / 1000.0;
	rv = convert_float_to_fru_float("ripple", fval,
					name, dtype, floatval);
	break;

    case 6: /* min current */
	val16 = d[9] | (d[10] << 8);
	fval = ((double) val16) / 1000.0;
	rv = convert_float_to_fru_float("min current", fval,
					name, dtype, floatval);
	break;

    case 7: /* max current */
	val16 = d[11] | (d[12] << 8);
	fval = ((double) val16) / 1000.0;
	rv = convert_float_to_fru_float("max current", fval,
					name, dtype, floatval);
	break;

    default:
	rv = EINVAL;
    }

    return rv;
}

static int
std_get_dc_output_root(ipmi_fru_t          *fru,
		       unsigned char       *mr_data,
		       unsigned int        mr_data_len,
		       const char          **name,
		       ipmi_fru_node_t     **rnode)
{
    std_dc_output_t *rec;
    ipmi_fru_node_t *node;
    int             rv;

    if (mr_data_len < 13)
	return EINVAL;
    
    rec = ipmi_mem_alloc(sizeof(*rec));
    if (!rec)
	return ENOMEM;
    memcpy(rec->data, mr_data, 13);

    node = _ipmi_fru_node_alloc(fru);
    if (!node)
	goto out_no_mem;

    _ipmi_fru_node_set_data(node, rec);
    _ipmi_fru_node_set_get_field(node, std_dc_output_get_field);
    _ipmi_fru_node_set_destructor(node, std_dc_output_root_destroy);
    *rnode = node;

    if (name)
	*name = "DC Output";

    return 0;

 out_no_mem:
    rv = ENOMEM;
    goto out_cleanup;

 out_cleanup:
    std_dc_output_cleanup_rec(rec);
    return rv;
}

typedef struct std_dc_load_s
{
    unsigned char data[13];
} std_dc_load_t;

static void std_dc_load_cleanup_rec(std_dc_load_t *rec)
{
    ipmi_mem_free(rec);
}

static void
std_dc_load_root_destroy(ipmi_fru_node_t *node)
{
    std_dc_load_t *rec = _ipmi_fru_node_get_data(node);
    std_dc_load_cleanup_rec(rec);
}

static int
std_dc_load_get_field(ipmi_fru_node_t           *pnode,
		      unsigned int              index,
		      const char                **name,
		      enum ipmi_fru_data_type_e *dtype,
		      int                       *intval,
		      time_t                    *time,
		      double                    *floatval,
		      char                      **data,
		      unsigned int              *data_len,
		      ipmi_fru_node_t           **sub_node)
{
    std_dc_load_t *rec = _ipmi_fru_node_get_data(pnode);
    unsigned char *d = rec->data;
    int           rv = 0;
    double        fval;
    int16_t       val16;

    switch(index) {
    case 0: /* output number */
	rv = convert_int_to_fru_int("output number", d[0] & 0x0f,
				    name, dtype, intval);
	break;

    case 1: /* nominal voltage */
	val16 = d[1] | (d[2] << 8);
	fval = ((double) val16) / 100.0;
	rv = convert_float_to_fru_float("nominal voltage", fval,
					name, dtype, floatval);
	break;

    case 2: /* min voltage */
	val16 = d[3] | (d[4] << 8);
	fval = ((double) val16) / 100.0;
	rv = convert_float_to_fru_float("min voltage", fval,
					name, dtype, floatval);
	break;

    case 3: /* max voltage */
	val16 = d[5] | (d[6] << 8);
	fval = ((double) val16) / 100.0;
	rv = convert_float_to_fru_float("max voltage", fval,
					name, dtype, floatval);
	break;

    case 4: /* ripple */
	val16 = d[7] | (d[8] << 8);
	fval = ((double) val16) / 1000.0;
	rv = convert_float_to_fru_float("ripple", fval,
					name, dtype, floatval);
	break;

    case 5: /* min current */
	val16 = d[9] | (d[10] << 8);
	fval = ((double) val16) / 1000.0;
	rv = convert_float_to_fru_float("min current", fval,
					name, dtype, floatval);
	break;

    case 6: /* max current */
	val16 = d[11] | (d[12] << 8);
	fval = ((double) val16) / 1000.0;
	rv = convert_float_to_fru_float("max current", fval,
					name, dtype, floatval);
	break;

    default:
	rv = EINVAL;
    }

    return rv;
}

static int
std_get_dc_load_root(ipmi_fru_t          *fru,
		     unsigned char       *mr_data,
		     unsigned int        mr_data_len,
		     const char          **name,
		     ipmi_fru_node_t     **rnode)
{
    std_dc_load_t   *rec;
    ipmi_fru_node_t *node;
    int             rv;

    if (mr_data_len < 13)
	return EINVAL;
    
    rec = ipmi_mem_alloc(sizeof(*rec));
    if (!rec)
	return ENOMEM;
    memcpy(rec->data, mr_data, 13);

    node = _ipmi_fru_node_alloc(fru);
    if (!node)
	goto out_no_mem;

    _ipmi_fru_node_set_data(node, rec);
    _ipmi_fru_node_set_get_field(node, std_dc_load_get_field);
    _ipmi_fru_node_set_destructor(node, std_dc_load_root_destroy);
    *rnode = node;

    if (name)
	*name = "DC Load";

    return 0;

 out_no_mem:
    rv = ENOMEM;
    goto out_cleanup;

 out_cleanup:
    std_dc_load_cleanup_rec(rec);
    return rv;
}

static int
std_get_mr_root(ipmi_fru_t          *fru,
		unsigned int        manufacturer_id,
		unsigned char       record_type_id,
		unsigned char       *mr_data,
		unsigned int        mr_data_len,
		void                *cb_data,
		const char          **name,
		ipmi_fru_node_t     **node)
{
    switch (record_type_id) {
    case 0x00:
	return std_get_power_supply_info_root(fru, mr_data, mr_data_len,
					      name, node);
    case 0x01:
	return std_get_dc_output_root(fru, mr_data, mr_data_len,
				      name, node);
    case 0x02:
	return std_get_dc_load_root(fru, mr_data, mr_data_len,
				    name, node);
    default:
	return EINVAL;
    }
}

/***********************************************************************
 *
 * FRU decoding
 *
 **********************************************************************/

typedef struct fru_offset_s
{
    int type;
    int offset;
} fru_offset_t;

static int
process_fru_info(ipmi_fru_t *fru)
{
    normal_fru_rec_data_t *info;
    ipmi_fru_record_t **recs;
    unsigned char     *data = _ipmi_fru_get_data_ptr(fru);
    unsigned int      data_len = _ipmi_fru_get_data_len(fru);
    fru_offset_t      foff[IPMI_FRU_FTR_NUMBER];
    int               i, j;
    int               err = 0;
    unsigned char     version;

    if (checksum(data, 8) != 0)
	return EBADF;

    version = *data;
    if (version != 1)
	/* Only support version 1 */
	return EBADF;

    for (i=0; i<IPMI_FRU_FTR_NUMBER; i++) {
	foff[i].type = i;
	if (! (_ipmi_fru_get_fetch_mask(fru) & (1 << i))) {
	    foff[i].offset = 0;
	    continue;
	}
	foff[i].offset = data[i+1] * 8;
	if (foff[i].offset >= data_len) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%snormal_fru.c(process_fru_info):"
		     " FRU offset exceeds data length",
		     _ipmi_fru_get_iname(fru));
	    return EBADF;
	}
    }

    /* Fields are *supposed* to occur in the specified order.  Verify
       this. */
    for (i=0, j=1; j<IPMI_FRU_FTR_NUMBER; i=j, j++) {
	if (foff[i].offset == 0)
	    continue;
	while (foff[j].offset == 0) {
	    j++;
	    if (j >= IPMI_FRU_FTR_NUMBER)
	        goto check_done;
	}
	if (foff[i].offset >= foff[j].offset) {
	    ipmi_log(IPMI_LOG_ERR_INFO,
		     "%snormal_fru.c(process_fru_info):"
		     " FRU fields did not occur in the correct order",
		     _ipmi_fru_get_iname(fru));
	    return EBADF;
	}
    }
 check_done:

    info = ipmi_mem_alloc(sizeof(*info));
    if (!info)
	return ENOMEM;
    memset(info, 0, sizeof(*info));

    _ipmi_fru_set_rec_data(fru, info);

    info->version = version;

    recs = info->recs;

    _ipmi_fru_set_op_cleanup_recs(fru, fru_cleanup_recs);
    _ipmi_fru_set_op_write_complete(fru, fru_write_complete);
    _ipmi_fru_set_op_write(fru, fru_write);
    _ipmi_fru_set_op_get_root_node(fru, fru_get_root_node);

    _ipmi_fru_set_is_normal_fru(fru, 1);

    for (i=0; i<IPMI_FRU_FTR_NUMBER; i++) {
	int plen, next_off, offset;
	ipmi_fru_record_t *rec;

	offset = foff[i].offset;
	if (offset == 0)
	    continue;

	for (j=i+1; j<IPMI_FRU_FTR_NUMBER; j++) {
	    if (foff[j].offset)
		break;
	}
	
	if (j >= IPMI_FRU_FTR_NUMBER)
	    next_off = data_len;
	else
	    next_off = foff[j].offset;
	plen = next_off - offset;

	rec = NULL;
	err = fru_area_info[i].decode(fru, data+offset, plen, &recs[i]);
	if (err)
	    goto out_err;

	if (recs[i])
	    recs[i]->offset = offset;
    }

    return 0;

 out_err:
    fru_cleanup_recs(fru);
    return err;
}

/************************************************************************
 *
 * Init/shutdown
 *
 ************************************************************************/

int
_ipmi_normal_fru_init(void)
{
    int rv;

    fru_multi_record_oem_handlers = locked_list_alloc
	(ipmi_get_global_os_handler());
    if (!fru_multi_record_oem_handlers)
        return ENOMEM;

    rv = _ipmi_fru_register_multi_record_oem_handler(0,
						     0x00,
						     std_get_mr_root,
						     NULL);
    if (rv)
	return rv;
    rv = _ipmi_fru_register_multi_record_oem_handler(0,
						     0x01,
						     std_get_mr_root,
						     NULL);
    if (rv)
	return rv;
    rv = _ipmi_fru_register_multi_record_oem_handler(0,
						     0x02,
						     std_get_mr_root,
						     NULL);
    if (rv)
	return rv;

    rv = _ipmi_fru_register_decoder(process_fru_info);
    if (rv)
	return rv;

    return 0;
}

void
_ipmi_normal_fru_shutdown(void)
{
    _ipmi_fru_deregister_decoder(process_fru_info);
    if (fru_multi_record_oem_handlers) {
	_ipmi_fru_deregister_multi_record_oem_handler(0, 0x00);
	_ipmi_fru_deregister_multi_record_oem_handler(0, 0x01);
	_ipmi_fru_deregister_multi_record_oem_handler(0, 0x02);
	locked_list_destroy(fru_multi_record_oem_handlers);
	fru_multi_record_oem_handlers = NULL;
    }
}

/************************************************************************
 *
 * Cruft
 *
 ************************************************************************/

int 
ipmi_fru_get_internal_use_data(ipmi_fru_t    *fru,
			       unsigned char *data,
			       unsigned int  *max_len)
{
    return ipmi_fru_get_internal_use(fru, data, max_len);
}

int 
ipmi_fru_get_internal_use_length(ipmi_fru_t   *fru,
				 unsigned int *length)
{
    return ipmi_fru_get_internal_use_len(fru, length);
}
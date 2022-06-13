/* SPDX-License-Identifier: Unlicense OR MIT-0 */

#ifndef NEZ_M3U_PARSER_H
#define NEZ_M3U_PARSER_H


/* define NEZ_M3U_STATIC if you
 * want *all* functions to be static */

#ifdef NEZ_M3U_STATIC
#undef NEZ_M3U_STATIC
#define NEZ_M3U_STATIC static
#else
#define NEZ_M3U_STATIC
#endif

typedef struct nez_m3u_s nez_m3u_t;

enum NEZ_M3U_LINE_ENUM {
    NEZ_M3U_UNKNOWN,
    NEZ_M3U_COMMENT,
    NEZ_M3U_TRACK
};

typedef enum NEZ_M3U_LINE_ENUM NEZ_M3U_LINE;

struct nez_m3u_s {
    /* src is a pointer to the current chunk of data
     * passed by nez_m3u_parse */
    const char *src;

    /* p is our current position within the chunk */
    const char *p;

    /* after parsing a line, linetype will be set to
     * an appropriate value.
     * NEZ_M3U_UNKNOWN = parse error
     * NEZ_M3U_COMMENT = a line beginning with a #
     * NEZ_M3U_TRACK = successfully parsed a track number
     */
    NEZ_M3U_LINE linetype;

    /* a pointer to the current line */
    const char *line;
    /* the length of the current line */
    unsigned int linelength;

    /* a pointer to the filename portion of the line */
    const char *filename;
    unsigned int filename_len;

    /* a pointer to the title portion of the line
     * this will be the "raw" value, see the nez_m3u_title
     * function for escaping the title
     */
    const char *title;
    /* the length of the title portion */
    unsigned int title_len;

    /* for all these fields, -1 = unknown */
    /* the track number */
    int tracknum;

    /* the length of the song in milliseconds */
    int length;

    /* the fadeout length in milliseconds */
    int fade;

    /* the intro length in milliseconds */
    int intro;

    /* the length of the looping portion in milliseconds */
    int loop;

    /* number of times to loop the track */
    int loops;
};


#ifdef __cplusplus
extern "C" {
#endif

/* initialize the struct */
NEZ_M3U_STATIC
void
nez_m3u_init(nez_m3u_t *n);

/* call this with your m3u_line and length until it returns 0 */
NEZ_M3U_STATIC
unsigned int
nez_m3u_parse(nez_m3u_t *n, const char *m3u_line, unsigned int len);

/* call this with NULL, 0 for dest/len to get the length of the title,
 * this is the length in bytes without the NULL terminator.
 * Len should be the length of your destination buffer, including the null
 * terminator. If your buffer is too small, the string will be truncated,
 * there will *always* be a NULL byte added.
 * If dest is not NULL, len *must* be provided */
NEZ_M3U_STATIC
unsigned int nez_m3u_title(const nez_m3u_t *n, char *dest, unsigned int len);

#ifdef __cplusplus
}
#endif

#endif

#ifdef NEZ_M3U_IMPLEMENTATION

#include <stddef.h>

static const char *
nez_m3u_find_char(const char *data, char d, unsigned int length) {
    const char *p = data;
    while(p - data < length) {
        if(*p == '\\') {
            p += 2;
            continue;
        }
        if(*p == d) return p;
        p++;
    }
    return p;
}

static unsigned int
nez_m3u_parse_base10(int *r, const char *buf, unsigned int len) {
    int t = 0;
    int f = 1;
    const char *b = buf;
    *r = 0;

    while(*b && len) {
        if(b == buf && *b == '-') {
            f = -1;
        } else if(*b >= 48 && *b <= 57) {
            t = (*b - 48) * f;
        } else {
            break;
        }

        *r *= 10;
        *r += t;
        b++;
        len--;
    }
    return b - buf;
}

static unsigned int
nez_m3u_parse_base16(int *r, const char *buf, unsigned int len) {
    int t = 0;
    int f = 1;
    const char *b = buf;
    *r = 0;

    while(*b && len) {
        if(b == buf && *b == '-') {
            f = -1;
        } else if(*b >= 48 && *b <= 57) {
            t = (*b - 48) * f;
        } else if(*b >= 65 && *b <= 70) {
            t = (*b - 55) * f;
        } else if(*b >= 97 && *b <= 102) {
            t = (*b - 87) * f;
        } else {
            break;
        }

        *r *= 16;
        *r += t;
        b++;
        len--;
    }
    return b - buf;
}

static unsigned int
nez_m3u_parse_timestamp(int *r, const char *buf, unsigned int len) {
    int c = 0;
    int t = 0;
    int m = 0;
    unsigned int l = 0;
    const char *b = buf;

    while(*b && len) {
        if(*b == ':') {
            c *= 60;
            b++;
            len--;
            continue;
        }

        if(*b == '.') {
            b++;
            len--;
            l = nez_m3u_parse_base10(&m,b,len);
            break;
        }

        l = nez_m3u_parse_base10(&t,b,len);
        if(l == 0) break;
        b += l;
        len -= l;
        c += t;
    }

    l = b - buf;
    if(l) {
        c *= 1000;
        c += m;
        *r = c;
    }

    return l;
}

static unsigned int
nez_m3u_parse_track(int *r, const char *buf, unsigned int len) {
    *r = 0;
    if(len == 0) return 0;
    if(*buf == '$') {
        buf++;
        len--;
        return nez_m3u_parse_base16(r,buf,len);
    }
    return nez_m3u_parse_base10(r,buf,len);
}

static void
nez_m3u_parse_line(nez_m3u_t *n, const char *data, unsigned int length) {
    const char *p = data;
    const char *d = p;

    n->line = data;
    n->linelength = length;

    n->title = NULL;
    n->title_len = 0;

    n->tracknum = -1;
    n->length = -1;
    n->fade = -1;
    n->intro = -1;
    n->loop = -1;
    n->loops = -1;

    if(length == 0) {
        return;
    }

    if(*p == '#') {
        n->linetype = NEZ_M3U_COMMENT;
        return;
    }

    /* first field: filename::format */
    p = nez_m3u_find_char(d, ',' , length - (d - data));
    if(p - data == length) {
        /* got to EOL without finding a comma */
        return;
    }
    n->filename_len = p - d;
    n->filename = d;

    d = p + 1;

    /* second field: tracknumber */
    p = nez_m3u_find_char(d, ',' , length - (d - data));
    if(!nez_m3u_parse_track(&n->tracknum,d,p - d)) return;
    if(p - data == length) {
        /* eol */
        return;
    }
    d = p + 1;

    /* we've now parsed the bare minimum */
    n->linetype = NEZ_M3U_TRACK;

    /* song title */
    p = nez_m3u_find_char(d, ',',  length - (d - data));
    n->title_len = p - d;
    n->title = d;
    if(p - data == length) return;
    d = p + 1;

    /* track length */
    p = nez_m3u_find_char(d, ',' , length - (d - data));
    nez_m3u_parse_timestamp(&n->length,d,p - d);
    if(p - data == length) return;
    d = p + 1;

    /* loop time */
    p = nez_m3u_find_char(d, ',' ,  length - (d - data));
    if(p - d > 0) {
        if(*d == '-') {
            n->loop = n->length;
        } else {
            nez_m3u_parse_timestamp(&n->loop,d,p-d);
            if( *(p-1) == '-' ) {
                n->intro = n->loop;
                n->loop = n->length - n->intro;
            } else {
                n->intro = n->length - n->loop;
            }
        }
    }
    if(p - data == length) return;
    d = p + 1;

    /* track fade */
    p = nez_m3u_find_char(d, ',' , length - (d - data));
    nez_m3u_parse_timestamp(&n->fade,d,p-d);
    if(p - data == length) return;
    d = p + 1;

    /* track loops */
    p = nez_m3u_find_char(d, ',' ,  length - (d - data));
    if(p - d > 0) {
        if(!nez_m3u_parse_base10(&n->loops,d,p-d)) {
            n->loops = 1;
        }
    }
    if(p - data == length) return;
    d = p + 1;

    return;

}

NEZ_M3U_STATIC
void
nez_m3u_init(nez_m3u_t *n) {
    n->src = NULL;
}

NEZ_M3U_STATIC
unsigned int
nez_m3u_parse(nez_m3u_t *n, const char *src, unsigned int len) {
    unsigned int offset = 0;
    unsigned int r = 0;
    const char *d = NULL;
    const char *p = NULL;

    n->linetype = NEZ_M3U_UNKNOWN;

    if(len == 0) return 0;

    if(src != n->src) {
        n->src = src;
        n->p = src;
    }

    p = n->p;
    d = p;

    if(p - src < len) {
        r = 1;

        p = nez_m3u_find_char(d,'\n',len - (d - src));
        if(p == NULL) {
            p = nez_m3u_find_char(d,'\r',len - (d - src));
            if(p == NULL) p = n->src + len;
        }
        if(*p == '\n') {
            if(p - 1 >= d && *(p-1) == '\r') {
                offset = 1;
            }
        }
        nez_m3u_parse_line(n,d,p-d-offset);

        n->p = ++p;
    }
    return r;
}

NEZ_M3U_STATIC
unsigned int
nez_m3u_title(const nez_m3u_t *n, char *dest, unsigned int len) {
    unsigned int i = 0;
    const char *t = n->title;

    if(n->title == NULL || n->title_len == 0) return i;
    if(len == 0 && dest != NULL) return i;

    if(len) len--;

    while(*t && t - n->title < n->title_len) {
        if(len && i == len) break;
        if(*t == '\\') {
            t++;
        }
        if(dest != NULL) {
            dest[i] = *t;
        }
        i++;
        t++;
    }
    if(dest != NULL) {
        dest[i] = '\0';
    }
    return i;
}


#endif

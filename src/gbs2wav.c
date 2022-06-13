#include "thirdparty/SameBoy/Core/gb.h"
#include "thirdparty/SameBoy/Core/apu.h"

#define NEZ_M3U_IMPLEMENTATION
#define NEZ_M3U_STATIC
#include "nez-m3u-parser.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BUFFER_SIZE 8192

typedef struct str_buffer {
    uint8_t *x;
    uint32_t a;
    uint32_t len;
} str_buffer;

typedef struct audio_buffer {
    FILE *output;
    uint64_t startFrames;
    uint64_t totalFrames;
    uint64_t fadeFrames;
    int16_t samples[2 * BUFFER_SIZE];
    uint8_t packed[2 * BUFFER_SIZE * sizeof(int16_t)];
    uint32_t curSample;
} audio_buffer;

static str_buffer id3_buffer;

static uint8_t *slurp(const char *filename, uint32_t *size);
static void dump_gbs_info(const GB_gbs_info_t *info);

static void fade_frames(int16_t *d, unsigned int framesRem, unsigned int fadeFrames, unsigned int frameCount);
static void pack_frames(uint8_t *d, int16_t *s, unsigned int frameCount);

static int write_wav_header(FILE *f, uint64_t totalFrames, str_buffer *id3);
static int write_wav_footer(FILE *f, str_buffer *id3);

static void id3_init(str_buffer *s);
static int id3_add_text(str_buffer *s, const char *frame, const char *data, size_t datalen);
static int id3_add_comment(str_buffer *s,const char *data, size_t datalen);
static int id3_add_private(str_buffer *s, const char *description, const char *data, size_t datalen);

static void pack_int16le(uint8_t *d, int16_t n);
static void pack_uint16le(uint8_t *d, uint16_t n);
static void pack_uint32le(uint8_t *d, uint32_t n);

static void on_sample(GB_gameboy_t *gb, GB_sample_t *sample) {
    audio_buffer *abuffer = GB_get_user_data(gb);
    if(abuffer->totalFrames) {
        abuffer->samples[abuffer->curSample++] = sample->left;
        abuffer->samples[abuffer->curSample++] = sample->right;
        abuffer->totalFrames--;

        if(abuffer->curSample == 2 * BUFFER_SIZE) {
            fade_frames(abuffer->samples,abuffer->totalFrames,abuffer->fadeFrames,BUFFER_SIZE);
            pack_frames(abuffer->packed,abuffer->samples,BUFFER_SIZE);
            fwrite(abuffer->packed,1,2 * BUFFER_SIZE * sizeof(int16_t), abuffer->output);
            abuffer->curSample = 0;
        }
    }
}

static void dummy_vblank(GB_gameboy_t *gb) {

}

static uint32_t rgb_encode_callback(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    return 0;
}

static uint32_t pixels[160 * 144];

int main(int argc, const char *argv[]) {
    uint8_t *m3uData;
    uint32_t m3uSize;

    uint8_t *gbsData;
    uint32_t gbsSize;

    audio_buffer abuffer;

    unsigned int i;
    unsigned int trackNo;
    unsigned int t;

    char trackName[BUFFER_SIZE];
    char outName[BUFFER_SIZE];
    char baseName[BUFFER_SIZE];

    char *tmp = NULL;
    size_t tmpAlloc;
    char *title = NULL;
    char *artist = NULL;
    char *date = NULL;
    char *ripper = NULL;
    char *tagger = NULL;
    char *c = NULL;

    GB_gameboy_t gb;
    GB_gbs_info_t gbsInfo;
    nez_m3u_t m3u;

    m3uData = NULL;
    gbsData = NULL;
    tmp = NULL;

    m3uSize = 0;
    gbsSize = 0;
    tmpAlloc = 0;

    if(argc < 2) {
        printf("Usage: /path/to/file.gbs (/path/to/file.m3u)\n");
        return 1;
    }

    id3_buffer.x = (uint8_t *)malloc(sizeof(uint8_t) * BUFFER_SIZE);
    if(id3_buffer.x == NULL) return 1;
    id3_buffer.len = 0;
    id3_buffer.a = BUFFER_SIZE;

    gbsData = slurp(argv[1], &gbsSize);
    if(gbsData == NULL) return 1;

    GB_init(&gb, GB_MODEL_CGB_E);
    GB_load_gbs_from_buffer(&gb, gbsData, gbsSize, &gbsInfo);

    memcpy(baseName,argv[1],strlen(argv[1]));
    baseName[strlen(argv[1])] = '\0';

    c = &baseName[strlen(baseName)-1];
    while(c > &baseName[0] && *c != '/') {
        *c = '\0';
        c--;
    }

    if(argc > 2) {
        m3uData = slurp(argv[2], &m3uSize);
        if(m3uData == NULL) return 1;
        nez_m3u_init(&m3u);
        i = 0;
        if(nez_m3u_parse(&m3u,(const char *)m3uData,m3uSize) == 0) return 1;
        while(m3u.linetype == NEZ_M3U_COMMENT) {
            if(tmpAlloc < m3u.linelength) {
                tmp = realloc(tmp,m3u.linelength + 1);
            }
            memcpy(tmp,&m3u.line[0],m3u.linelength);
            tmp[m3u.linelength] = '\0';

            if( (c = strstr(tmp,"@TITLE")) != NULL) {
                c += strlen("@TITLE");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) continue;
                title = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(title,c,strlen(c) + 1);
            } else if( (c = strstr(tmp,"@ARTIST")) != NULL) {
                c += strlen("@ARTIST");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) continue;
                artist = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(artist,c,strlen(c) + 1);
            } else if( (c = strstr(tmp,"@DATE")) != NULL) {
                c += strlen("@DATE");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) continue;
                date = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(date,c,strlen(c) + 1);
            } else if( (c = strstr(tmp,"@RIPPER")) != NULL) {
                c += strlen("@RIPPER");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) continue;
                ripper = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(ripper,c,strlen(c) + 1);
            } else if( (c = strstr(tmp,"@TAGGER")) != NULL) {
                c += strlen("@TAGGER");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) continue;
                tagger = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(tagger,c,strlen(c) + 1);
            }
            else if(i == 0) {
                c = tmp;
                while(*c && *c == '#') c++;
                while(*c && *c == ' ') c++;
                if(!*c) continue;
                title = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(title,c,strlen(c) + 1);
            }

            if(nez_m3u_parse(&m3u,(const char *)m3uData,m3uSize) == 0) return 1;
            i++;
        }
        /* find any tags we can */
        nez_m3u_init(&m3u);
    }

    if(title == NULL) {
        if(strlen(gbsInfo.title)) title = gbsInfo.title;
    }
    if(artist == NULL) {
        if(strlen(gbsInfo.author)) artist = gbsInfo.author;
    }

    dump_gbs_info(&gbsInfo);
    GB_set_sample_rate(&gb, SAMPLE_RATE);
    GB_set_user_data(&gb, &abuffer);
    GB_apu_set_sample_callback(&gb, on_sample);
    GB_set_pixels_output(&gb, pixels);
    GB_set_vblank_callback(&gb, dummy_vblank);
    GB_set_rgb_encode_callback(&gb, rgb_encode_callback);

    i = gbsInfo.first_track;
    while(i < gbsInfo.track_count) {
        trackName[0] = '\0';

        if(m3uData == NULL) {
            trackNo = i;
            abuffer.totalFrames = 3 * 60 * SAMPLE_RATE;
            abuffer.fadeFrames  = 10 * SAMPLE_RATE;
        } else {
            if(nez_m3u_parse(&m3u,(const char *)m3uData,m3uSize) == 0) goto done;
            while(m3u.linetype != NEZ_M3U_TRACK) {
                if(nez_m3u_parse(&m3u,(const char *)m3uData,m3uSize) == 0) goto done;
            }
            trackNo = m3u.tracknum;
            t = nez_m3u_title(&m3u, NULL, 0);
            t = t + 1 > sizeof(trackName) ? sizeof(trackName) : t + 1;
            nez_m3u_title(&m3u,trackName,t);
            if(m3u.length != -1) {
                abuffer.totalFrames = m3u.length * SAMPLE_RATE / 1000;
            } else {
                if(m3u.fade == -1) {
                    abuffer.totalFrames = 170 * SAMPLE_RATE;
                } else {
                    abuffer.totalFrames = 180 * SAMPLE_RATE;
                }
            }

            if(m3u.fade != -1) {
                abuffer.fadeFrames = m3u.fade * SAMPLE_RATE / 1000;
            } else {
                abuffer.fadeFrames  = 10 * SAMPLE_RATE;
            }

            abuffer.totalFrames += abuffer.fadeFrames;
        }


        if(strlen(trackName) == 0) {
            snprintf(trackName,sizeof(trackName),"%s %03d/%03d", gbsInfo.title,trackNo + 1,gbsInfo.track_count);
        }

        id3_init(&id3_buffer);
        if(title != NULL) {
            id3_add_text(&id3_buffer,"TALB",title,strlen(title));
        }
        if(artist != NULL) {
            id3_add_text(&id3_buffer,"TPE1",artist,strlen(artist));
        }
        if(strlen(trackName) > 0) {
            id3_add_text(&id3_buffer,"TIT2",trackName,strlen(trackName));
        }

        /* find any non-file safe characters in trackname and replace them */
        while( (c = strchr(trackName,'/')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'\\')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,':')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'*')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'"')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'?')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'<')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'>')) != NULL) {
            *c = '_';
        }
        while( (c = strchr(trackName,'|')) != NULL) {
            *c = '_';
        }

        snprintf(outName,sizeof(outName),"%s%03d %s.wav",
          baseName,i+1,trackName);

        printf("outName: %s\n",outName);
        abuffer.output = fopen(outName,"wb");
        abuffer.curSample = 0;
        abuffer.startFrames = abuffer.totalFrames;
        write_wav_header(abuffer.output,abuffer.totalFrames,&id3_buffer);

        GB_reset(&gb);
        /* GB_lcd_off(&gb); */
        GB_gbs_switch_track(&gb,i);

        double nextPct = 0.0f;

        while(abuffer.totalFrames) {
            double pct = 1.0 - ( (double)abuffer.totalFrames / (double)abuffer.startFrames);
            pct *= 100.0;
            GB_run(&gb);
            if(pct >= nextPct) {
                printf("%f%%\n",pct);
                nextPct += 1.0;
            }
        }

        if(abuffer.curSample > 0) {
            fade_frames(abuffer.samples,abuffer.totalFrames,abuffer.fadeFrames,abuffer.curSample / 2);
            pack_frames(abuffer.packed,abuffer.samples,abuffer.curSample / 2);
            fwrite(abuffer.packed,1,abuffer.curSample * sizeof(int16_t), abuffer.output);
        }

        write_wav_footer(abuffer.output,&id3_buffer);
        fclose(abuffer.output);

        i++;
    }
    done:

    return 0;
}

static uint8_t *slurp(const char *filename, uint32_t *size) {
    uint8_t *buf;
    FILE *f = fopen(filename,"rb");
    if(f == NULL) {
        fprintf(stderr,"Error opening %s: %s\n",
          filename,
          strerror(errno));
        return NULL;
    }
    fseek(f,0,SEEK_END);
    *size = ftell(f);
    fseek(f,0,SEEK_SET);

    buf = (uint8_t *)malloc(*size);
    if(buf == NULL) {
        fprintf(stderr,"out of memory\n");
        return NULL;
    }
    if(fread(buf,1,*size,f) != *size) {
        fprintf(stderr,"error reading file\n");
        free(buf);
        return NULL;
    }
    return buf;
}

static void dump_gbs_info(const GB_gbs_info_t *info) {
    printf("GBS Info:\n");
    printf("Track count: %u\n",info->track_count);
    printf("First track: %u\n",info->first_track);
    printf("Title: %s\n",info->title);
    printf("Author: %s\n",info->author);
    printf("Copyright: %s\n",info->copyright);
    printf("\n");
}

static void pack_uint32_syncsafe(uint8_t *output, uint32_t val) {
    output[0] = (uint8_t)((val & 0x0FE00000) >> 21);
    output[1] = (uint8_t)((val & 0x001FC000) >> 14);
    output[2] = (uint8_t)((val & 0x00003F80) >> 7);
    output[3] = (uint8_t)((val & 0x0000007F));
}

static void id3_update_len(str_buffer *s) {
    pack_uint32_syncsafe(&s->x[6],s->len - 10);
}

static void id3_init(str_buffer *s) {
    s->len = 10;

    s->x[0] = 'I';
    s->x[1] = 'D';
    s->x[2] = '3';
    s->x[3] = 0x04;
    s->x[4] = 0x00;
    s->x[5] = 0x00;

    id3_update_len(s);
}

static int id3_add_text(str_buffer *s, const char *frame, const char *data, size_t datalen) {
    uint8_t *t;

    while(s->len + 10 + (1 + datalen + 1) > s->a) {
        t = realloc(s->x, s->a + 512);
        if(t == NULL) {
            return -1;
        }
        s->x = t;
        s->a += 512;
    }

    memcpy(&s->x[s->len],frame,4);
    s->len += 4;

    pack_uint32_syncsafe(&s->x[s->len],(1 + datalen + 1));
    s->len += 4;

    s->x[s->len++] = 0x00;
    s->x[s->len++] = 0x00;
    s->x[s->len++] = 0x03;

    memcpy(&s->x[s->len], data, datalen);
    s->len += datalen;
    s->x[s->len++] = 0x00;

    id3_update_len(s);

    return 0;
}

static int id3_add_comment(str_buffer *s, const char *data, size_t datalen) {
    uint8_t *t;

    while(s->len + 10 + (1 + 3 + datalen + 1 + 1) > s->a) {
        t = realloc(s->x, s->a + 512);
        if(t == NULL) {
            return -1;
        }
        s->x = t;
        s->a += 512;
    }

    s->x[s->len++] = 'C';
    s->x[s->len++] = 'O';
    s->x[s->len++] = 'M';
    s->x[s->len++] = 'M';

    pack_uint32_syncsafe(&s->x[s->len],(1 + 3 + datalen + 1 + 1));
    s->len += 4;

    s->x[s->len++] = 0x00; /* flags */
    s->x[s->len++] = 0x00; /* flags */
    s->x[s->len++] = 0x03; /* encoding */

    s->x[s->len++] = 'e';
    s->x[s->len++] = 'n';
    s->x[s->len++] = 'g';
    s->x[s->len++] = 0x00; /* short content descrip */

    memcpy(&s->x[s->len], data, datalen);
    s->len += datalen;
    s->x[s->len++] = 0x00;

    id3_update_len(s);

    return 0;
}

static int id3_add_private(str_buffer *s, const char *description, const char *data, size_t datalen) {
    uint8_t *t;

    while(s->len + 10 + (1 + strlen(description) + 1 + datalen + 1) > s->a) {
        t = realloc(s->x, s->a + 512);
        if(t == NULL) {
            return -1;
        }
        s->x = t;
        s->a += 512;
    }

    s->x[s->len++] = 'T';
    s->x[s->len++] = 'X';
    s->x[s->len++] = 'X';
    s->x[s->len++] = 'X';

    pack_uint32_syncsafe(&s->x[s->len],(1 + strlen(description) + 1 + datalen + 1));
    s->len += 4;

    s->x[s->len++] = 0x00; /* flags */
    s->x[s->len++] = 0x00; /* flags */
    s->x[s->len++] = 0x03; /* encoding */

    memcpy(&s->x[s->len], description, strlen(description));
    s->len += strlen(description);
    s->x[s->len++] = 0x00; /* short content descrip */

    memcpy(&s->x[s->len], data, datalen);
    s->len += datalen;
    s->x[s->len++] = 0x00;

    id3_update_len(s);

    return 0;
}
static void pack_int16le(uint8_t *d, int16_t n) {
    d[0] = (uint8_t)(n      );
    d[1] = (uint8_t)(n >> 8 );
}

static void pack_uint16le(uint8_t *d, uint16_t n) {
    d[0] = (uint8_t)(n      );
    d[1] = (uint8_t)(n >> 8 );
}

static void pack_uint32le(uint8_t *d, uint32_t n) {
    d[0] = (uint8_t)(n      );
    d[1] = (uint8_t)(n >> 8 );
    d[2] = (uint8_t)(n >> 16);
    d[3] = (uint8_t)(n >> 24);
}

static int write_wav_footer(FILE *f, str_buffer *id3) {
    uint8_t tmp[4];

    if(id3->len == 10) return 1;

    if(fwrite("ID3 ",1,4,f) != 4) return 0;
    pack_uint32le(tmp,id3->len);
    if(fwrite(tmp,1,4,f) != 4) return 0;
    if(fwrite(id3->x,1,id3->len,f) != id3->len) return 0;
    return 1;
}

static int write_wav_header(FILE *f, uint64_t totalFrames, str_buffer *id3) {
    unsigned int dataSize = totalFrames * sizeof(int16_t) * CHANNELS;
    unsigned int id3Size = 0;
    uint8_t tmp[4];

    if(id3->len > 10) id3Size = id3->len + 8;

    if(fwrite("RIFF",1,4,f) != 4) return 0;
    pack_uint32le(tmp,dataSize + 44 - 8 + id3Size);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    if(fwrite("WAVE",1,4,f) != 4) return 0;
    if(fwrite("fmt ",1,4,f) != 4) return 0;

    pack_uint32le(tmp,16); /*fmtSize */
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint16le(tmp,1); /* audioFormat */
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint16le(tmp,CHANNELS); /* numChannels */
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint32le(tmp,SAMPLE_RATE);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint32le(tmp,SAMPLE_RATE * CHANNELS * sizeof(int16_t));
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint16le(tmp,CHANNELS * sizeof(int16_t));
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint16le(tmp,sizeof(int16_t) * 8);
    if(fwrite(tmp,1,2,f) != 2) return 0;

    if(fwrite("data",1,4,f) != 4) return 0;

    pack_uint32le(tmp,dataSize);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    return 1;
}

static void
fade_frames(int16_t *data, unsigned int framesRem, unsigned int framesFade, unsigned int frameCount) {
    unsigned int i = 0;
    unsigned int f = framesFade;
    double fade;

    if(framesRem - frameCount > framesFade) return;
    if(framesRem > framesFade) {
        i = framesRem - framesFade;
        f += i;
    } else {
        f = framesRem;
    }

    while(i<frameCount) {
        fade = (double)(f-i) / (double)framesFade;
        data[(i*2)+0] *= fade;
        data[(i*2)+1] *= fade;
        i++;
    }

    return;
}

static void pack_frames(uint8_t *d, int16_t *s, unsigned int frameCount) {
    unsigned int i = 0;
    while(i<frameCount) {
        pack_int16le(&d[0],s[(i*2)+0]);
#if CHANNELS == 2
        pack_int16le(&d[sizeof(int16_t)],s[(i*2)+1]);
#endif
        i++;
        d += (sizeof(int16_t) * CHANNELS);
    }
}

static int write_frames(FILE *f, uint8_t *d, unsigned int frameCount) {
    return fwrite(d,sizeof(int16_t) * CHANNELS,frameCount,f) == frameCount;
}

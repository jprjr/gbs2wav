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

#define DEFAULT_SAMPLE_RATE 48000
#define MAX_CHANNELS 2
#define BUFFER_SIZE (8192 * 2)
#define DEFAULT_GB_MODEL GB_MODEL_DMG_B

#define CLAMP(val, min, max) ( (val) < (min) ? (min) : (val) > (max) ? (max) : (val) )
#define str_equals(s1,s2) (strcmp(s1,s2) == 0)
#define str_iequals(s1,s2) (strcasecmp(s1,s2) == 0)
#define str_istarts(s1,s2) (strncasecmp(s1,s2,strlen(s2)) == 0)

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
    int16_t samples[MAX_CHANNELS * BUFFER_SIZE];
    uint8_t packed[MAX_CHANNELS * BUFFER_SIZE * 2];
    uint64_t curSample;
} audio_buffer;

static str_buffer id3_buffer;

static uint8_t *slurp(const char *filename, uint32_t *size);
static void dump_gbs_info(const GB_gbs_info_t *info);

static void fade_frames_mono(int16_t *d, uint64_t framesRem, uint64_t fadeFrames, uint64_t frameCount);
static void pack_frames_mono(uint8_t *d, int16_t *s, uint64_t frameCount);

static void fade_frames_stereo(int16_t *d, uint64_t framesRem, uint64_t fadeFrames, uint64_t frameCount);
static void pack_frames_stereo(uint8_t *d, int16_t *s, uint64_t frameCount);

static int write_wav_header(FILE *f, uint64_t channels, uint64_t totalFrames, uint32_t sampleRate, str_buffer *id3);
static int write_wav_footer(FILE *f, str_buffer *id3);

static void id3_init(str_buffer *s);
static int id3_add_text(str_buffer *s, const char *frame, const char *data, size_t datalen);
static int id3_add_comment(str_buffer *s,const char *data, size_t datalen);
static int id3_add_private(str_buffer *s, const char *description, const char *data, size_t datalen);

static void pack_int16le(uint8_t *d, int16_t n);
static void pack_uint16le(uint8_t *d, uint16_t n);
static void pack_uint32le(uint8_t *d, uint32_t n);
static uint64_t scan_uint(const char *s);
static int usage(const char *self, int e);

static void on_sample_mono(GB_gameboy_t *gb, GB_sample_t *sample) {
    int32_t s;
    audio_buffer *abuffer = GB_get_user_data(gb);
    if(abuffer->totalFrames) {
        s = (int32_t)sample->left;
        s += (int32_t)sample->right;
        s /= 2;
        abuffer->samples[abuffer->curSample++] = CLAMP(s,-0x8000,0x7FFF);
        abuffer->totalFrames--;

        if(abuffer->curSample == BUFFER_SIZE) {
            fade_frames_mono(abuffer->samples,abuffer->totalFrames,abuffer->fadeFrames,BUFFER_SIZE);
            pack_frames_mono(abuffer->packed,abuffer->samples,BUFFER_SIZE);
            fwrite(abuffer->packed,1, BUFFER_SIZE * 2, abuffer->output);
            abuffer->curSample = 0;
        }
    }
}

static void on_sample_stereo(GB_gameboy_t *gb, GB_sample_t *sample) {
    audio_buffer *abuffer = GB_get_user_data(gb);
    if(abuffer->totalFrames) {
        abuffer->samples[abuffer->curSample++] = sample->left;
        abuffer->samples[abuffer->curSample++] = sample->right;
        abuffer->totalFrames--;

        if(abuffer->curSample == 2 * BUFFER_SIZE) {
            fade_frames_stereo(abuffer->samples,abuffer->totalFrames,abuffer->fadeFrames,BUFFER_SIZE);
            pack_frames_stereo(abuffer->packed,abuffer->samples,BUFFER_SIZE);
            fwrite(abuffer->packed,1,2 * BUFFER_SIZE * 2, abuffer->output);
            abuffer->curSample = 0;
        }
    }
}

static const char *NAME_DMG_B          = "Game Boy";
static const char *NAME_SGB            = "Super Game Boy (NTSC)";
static const char *NAME_SGB_NO_SFC     = "Super Game Boy (NTSC) (No SFC)";
static const char *NAME_SGB_PAL        = "Super Game Boy (PAL)";
static const char *NAME_SGB_PAL_NO_SFC = "Super Game Boy (PAL) (No SFC)";
static const char *NAME_SGB2           = "Super Game Boy 2";
static const char *NAME_SGB2_NO_SFC    = "Super Game Boy 2 (No SFC)";
static const char *NAME_MGB            = "Game Boy Pocket/Light";
static const char *NAME_CGB_0          = "Game Boy Color (CPU CGB 0)";
static const char *NAME_CGB_A          = "Game Boy Color (CPU CGB A)";
static const char *NAME_CGB_B          = "Game Boy Color (CPU CGB B)";
static const char *NAME_CGB_C          = "Game Boy Color (CPU CGB C)";
static const char *NAME_CGB_D          = "Game Boy Color (CPU CGB D)";
static const char *NAME_CGB_E          = "Game Boy Color";
static const char *NAME_AGB_A          = "Game Boy Advance";
static const char *NAME_GBP_A          = "Game Boy Player";

int main(int argc, const char *argv[]) {
    int r = 1;
    const char* self;
    uint8_t *m3uData;
    uint32_t m3uSize;

    uint8_t *gbsData;
    uint32_t gbsSize;

    audio_buffer abuffer;

    unsigned int i;
    unsigned int trackNo;
    unsigned int t;
    unsigned int channels;
    uint64_t sampleRate;

    char trackName[BUFFER_SIZE];
    char outName[BUFFER_SIZE];
    char baseName[BUFFER_SIZE];

    char *tmp = NULL;
    size_t tmpAlloc;
    char *title = NULL;
    char *artist = NULL;
    char *composer = NULL;
    char *date = NULL;
    char *ripper = NULL;
    char *tagger = NULL;
    char *c = NULL;
    const char *s = NULL;
    const char *modelName = NULL;

    GB_gameboy_t gb;
    GB_gbs_info_t gbsInfo;
    GB_model_t gb_model;
    nez_m3u_t m3u;

    m3uData = NULL;
    gbsData = NULL;
    tmp = NULL;

    m3uSize = 0;
    gbsSize = 0;
    tmpAlloc = 0;
    sampleRate = DEFAULT_SAMPLE_RATE;
    channels = MAX_CHANNELS;
    gb_model = DEFAULT_GB_MODEL;

    self = *argv++;
    argc--;

    while(argc > 0) {
        if(str_equals(*argv,"--")) {
            argv++;
            argc--;
            break;
        }
        else if(str_istarts(*argv,"--help") || str_istarts(*argv,"/?")) {
            return usage(self,0);
        }
        else if(str_istarts(*argv,"--mono")) {
            channels = 1;
            argv++;
            argc--;
        }
        else if(str_istarts(*argv,"--model")) {
            c = strchr(*argv,'=');
            if(c != NULL) {
                s = &c[1];
            } else {
                argv++;
                argc--;
                s = *argv;
            }
            if(str_iequals(s,"dmg-b")) {
                gb_model = GB_MODEL_DMG_B;
            } else if(str_iequals(s,"sgb")) {
                gb_model = GB_MODEL_SGB;
            } else if(str_iequals(s,"sgb-no-sfc")) {
                gb_model = GB_MODEL_SGB_NO_SFC;
            } else if(str_iequals(s,"sgb-ntsc")) {
                gb_model = GB_MODEL_SGB_NTSC;
            } else if(str_iequals(s,"sgb-ntsc-no-sfc")) {
                gb_model = GB_MODEL_SGB_NTSC_NO_SFC;
            } else if(str_iequals(s,"sgb-pal")) {
                gb_model = GB_MODEL_SGB_PAL;
            } else if(str_iequals(s,"sgb-pal-no-sfc")) {
                gb_model = GB_MODEL_SGB_PAL_NO_SFC;
            } else if(str_iequals(s,"mgb")) {
                gb_model = GB_MODEL_MGB;
            } else if(str_iequals(s,"sgb2")) {
                gb_model = GB_MODEL_SGB2;
            } else if(str_iequals(s,"sgb2-no-sfc")) {
                gb_model = GB_MODEL_SGB2_NO_SFC;
            } else if(str_iequals(s,"cgb-0")) {
                gb_model = GB_MODEL_CGB_0;
            } else if(str_iequals(s,"cgb-a")) {
                gb_model = GB_MODEL_CGB_A;
            } else if(str_iequals(s,"cgb-b")) {
                gb_model = GB_MODEL_CGB_B;
            } else if(str_iequals(s,"cgb-c")) {
                gb_model = GB_MODEL_CGB_C;
            } else if(str_iequals(s,"cgb-d")) {
                gb_model = GB_MODEL_CGB_D;
            } else if(str_iequals(s,"cgb-e")) {
                gb_model = GB_MODEL_CGB_E;
            } else if(str_iequals(s,"agb-a")) {
                gb_model = GB_MODEL_AGB_A;
            } else if(str_iequals(s,"gbp-a")) {
                gb_model = GB_MODEL_GBP_A;
            } else if(str_iequals(s,"dmg")) {
                gb_model = GB_MODEL_DMG_B;
            } else if(str_iequals(s,"cgb")) {
                gb_model = GB_MODEL_CGB_E;
            } else if(str_iequals(s,"agb")) {
                gb_model = GB_MODEL_AGB;
            } else if(str_iequals(s,"gbp")) {
                gb_model = GB_MODEL_GBP;
            } else {
                fprintf(stderr,"unknown model string %s\n",s);
                return usage(self,1);
            }
            argv++;
            argc--;
        }
        else if(str_istarts(*argv,"--sample-rate") || str_istarts(*argv,"--samplerate")) {
            c = strchr(*argv,'=');
            if(c != NULL) {
                s = &c[1];
            } else {
                argv++;
                argc--;
                s = *argv;
            }
            sampleRate = scan_uint(s);
            if(sampleRate == 0) {
                return usage(self,1);
            }
            argv++;
            argc--;
        } else break;
    }


    if(argc < 1) {
        return usage(self,1);
    }

    id3_buffer.x = (uint8_t *)malloc(sizeof(uint8_t) * BUFFER_SIZE);
    if(id3_buffer.x == NULL) goto done;
    id3_buffer.len = 0;
    id3_buffer.a = BUFFER_SIZE;

    gbsData = slurp(argv[0], &gbsSize);
    if(gbsData == NULL) goto done;

    GB_init(&gb, gb_model);
    GB_set_sample_rate(&gb, sampleRate);
    GB_set_user_data(&gb, &abuffer);
    if(channels == 1) {
        GB_apu_set_sample_callback(&gb, on_sample_mono);
    } else {
        GB_apu_set_sample_callback(&gb, on_sample_stereo);
    }
    GB_set_rendering_disabled(&gb, 1);

    GB_load_gbs_from_buffer(&gb, gbsData, gbsSize, &gbsInfo);

    memcpy(baseName,argv[0],strlen(argv[0]));
    baseName[strlen(argv[0])] = '\0';

    c = &baseName[strlen(baseName)-1];
    while(c >= &baseName[0] && *c != '/') {
        *c = '\0';
        c--;
    }

    if(argc > 1) {
        m3uData = slurp(argv[1], &m3uSize);
        if(m3uData == NULL) goto done;
        nez_m3u_init(&m3u);
        i = 0;
        if(nez_m3u_parse(&m3u,(const char *)m3uData,m3uSize) == 0) goto done;
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
                if(!*c) goto nextline;
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
                if(!*c) goto nextline;
                artist = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(artist,c,strlen(c) + 1);
            } else if( (c = strstr(tmp,"@COMPOSER")) != NULL) {
                c += strlen("@COMPOSER");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) goto nextline;
                composer = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(composer,c,strlen(c) + 1);
            } else if( (c = strstr(tmp,"@DATE")) != NULL) {
                c += strlen("@DATE");
                if(*c == ':') {
                    c++;
                } else if(*c == '@') {
                    c++;
                }
                while(*c && *c == ' ') c++;
                if(!*c) goto nextline;
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
                if(!*c) goto nextline;
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
                if(!*c) goto nextline;
                tagger = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(tagger,c,strlen(c) + 1);
            }
            else if(i == 0) {
                c = tmp;
                while(*c && *c == '#') c++;
                while(*c && *c == ' ') c++;
                if(!*c) goto nextline;
                title = malloc(sizeof(char) * strlen(c) + 1);
                memcpy(title,c,strlen(c) + 1);
            }

            nextline:
            if(nez_m3u_parse(&m3u,(const char *)m3uData,m3uSize) == 0) goto done;
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
    if(m3uData != NULL) {
        printf("Parsed M3U tags:\n");
        if(title != NULL && title != gbsInfo.title) {
            printf("Title: %s\n",title);
        }
        if(artist != NULL && artist != gbsInfo.author) {
            printf("Artist: %s\n",artist);
        }
        if(composer != NULL) {
            printf("Composer: %s\n",composer);
        }
        if(date != NULL) {
            printf("Date: %s\n",date);
        }
        if(ripper != NULL) {
            printf("Ripper: %s\n",ripper);
        }
        if(tagger != NULL) {
            printf("tagger: %s\n",tagger);
        }
        printf("\n");
    }

    switch(gb_model) {
        case GB_MODEL_DMG_B: modelName = NAME_DMG_B; break;
        case GB_MODEL_SGB: modelName = NAME_SGB; break;
        case GB_MODEL_SGB_NO_SFC: modelName = NAME_SGB_NO_SFC; break;
        case GB_MODEL_SGB_PAL: modelName = NAME_SGB_PAL; break;
        case GB_MODEL_SGB_PAL_NO_SFC: modelName = NAME_SGB_PAL_NO_SFC; break;
        case GB_MODEL_MGB: modelName = NAME_MGB; break;
        case GB_MODEL_SGB2: modelName = NAME_SGB2; break;
        case GB_MODEL_SGB2_NO_SFC: modelName = NAME_SGB2_NO_SFC; break;
        case GB_MODEL_CGB_0: modelName = NAME_CGB_0; break;
        case GB_MODEL_CGB_A: modelName = NAME_CGB_A; break;
        case GB_MODEL_CGB_B: modelName = NAME_CGB_B; break;
        case GB_MODEL_CGB_C: modelName = NAME_CGB_C; break;
        case GB_MODEL_CGB_D: modelName = NAME_CGB_D; break;
        case GB_MODEL_CGB_E: modelName = NAME_CGB_E; break;
        case GB_MODEL_AGB_A: modelName = NAME_AGB_A; break;
        case GB_MODEL_GBP_A: modelName = NAME_GBP_A; break;
        default: abort();
    }

    printf("Emulating %s\n",modelName);

    printf("Rendering as 16-bit, %u-channel, %luHz WAVE\n",
      channels, sampleRate);

    i = gbsInfo.first_track;
    while(i < gbsInfo.track_count) {
        trackName[0] = '\0';

        if(m3uData == NULL) {
            trackNo = i;
            abuffer.totalFrames = 3 * 60 * sampleRate;
            abuffer.fadeFrames  = 10 * sampleRate;
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
                abuffer.totalFrames = m3u.length * sampleRate / 1000;
            } else {
                if(m3u.fade == -1) {
                    abuffer.totalFrames = 170 * sampleRate;
                } else {
                    abuffer.totalFrames = 180 * sampleRate;
                }
            }

            if(m3u.fade != -1) {
                abuffer.fadeFrames = m3u.fade * sampleRate / 1000;
            } else {
                abuffer.fadeFrames  = 10 * sampleRate;
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
        if(composer != NULL) {
            id3_add_text(&id3_buffer,"TCOM",composer,strlen(composer));
        }
        if(date != NULL) {
            id3_add_text(&id3_buffer,"TDRL",date,strlen(date));
        }
        if(strlen(trackName) > 0) {
            id3_add_text(&id3_buffer,"TIT2",trackName,strlen(trackName));
        }
        if(ripper != NULL) {
            id3_add_private(&id3_buffer,"gbs_ripper",ripper,strlen(ripper));
        }
        if(tagger != NULL) {
            id3_add_private(&id3_buffer,"gbs_tagger",tagger,strlen(tagger));
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

        snprintf(outName,sizeof(outName),"%s%03u %s.wav",
          baseName,i+1,trackName);

        printf("Saving track %u to: %s\n",i+1,outName);
        abuffer.output = fopen(outName,"wb");
        abuffer.curSample = 0;
        abuffer.startFrames = abuffer.totalFrames;
        write_wav_header(abuffer.output,channels,abuffer.totalFrames,(uint32_t)sampleRate,&id3_buffer);

        GB_reset(&gb);
        /* GB_lcd_off(&gb); */
        GB_gbs_switch_track(&gb,trackNo);

        double nextPct = 0.0f;
        printf("%02.0f%%\n",0.0);

        while(abuffer.totalFrames) {
            double pct = 1.0 - ( (double)abuffer.totalFrames / (double)abuffer.startFrames);
            pct *= 100.0;
            GB_run(&gb);
            if(pct >= nextPct) {
                printf("\x1b[1F%02.0f%%\n",pct);
                nextPct += 1.0;
            }
        }

        if(abuffer.curSample > 0) {
            if(channels == 1) {
                fade_frames_mono(abuffer.samples,abuffer.totalFrames,abuffer.fadeFrames,abuffer.curSample);
                pack_frames_mono(abuffer.packed,abuffer.samples,abuffer.curSample);
            } else {
                fade_frames_stereo(abuffer.samples,abuffer.totalFrames,abuffer.fadeFrames,abuffer.curSample / 2);
                pack_frames_stereo(abuffer.packed,abuffer.samples,abuffer.curSample / 2);
            }
            fwrite(abuffer.packed,1,abuffer.curSample * 2, abuffer.output);
        }

        write_wav_footer(abuffer.output,&id3_buffer);
        fclose(abuffer.output);
        printf("\x1b[1F%02.0f%%\n",100.0);

        i++;
    }
    r = 0;

    done:
    GB_free(&gb);

    if(title != NULL) free(title);
    if(artist != NULL) free(artist);
    if(date != NULL) free(date);
    if(ripper != NULL) free(ripper);
    if(tagger != NULL) free(tagger);
    if(tmp != NULL) free(tmp);
    if(id3_buffer.x != NULL) free(id3_buffer.x);
    if(gbsData != NULL) free(gbsData);
    if(m3uData != NULL) free(m3uData);

    return r;
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
        fclose(f);
        return NULL;
    }
    if(fread(buf,1,*size,f) != *size) {
        fprintf(stderr,"error reading file\n");
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
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

static int write_wav_header(FILE *f, uint64_t channels, uint64_t totalFrames, uint32_t sampleRate, str_buffer *id3) {
    uint64_t dataSize = totalFrames * 2 * channels;
    uint64_t id3Size = 0;
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

    pack_uint16le(tmp,channels); /* numChannels */
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint32le(tmp,sampleRate);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint32le(tmp,sampleRate * channels * 2);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint16le(tmp,channels * 2);
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint16le(tmp,2 * 8);
    if(fwrite(tmp,1,2,f) != 2) return 0;

    if(fwrite("data",1,4,f) != 4) return 0;

    pack_uint32le(tmp,dataSize);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    return 1;
}

static void
fade_frames_mono(int16_t *data, uint64_t framesRem, uint64_t framesFade, uint64_t frameCount) {
    uint64_t i = 0;
    uint64_t f = framesFade;
    double fade;
    int32_t s;

    if(framesRem > framesFade) {
        i = framesRem - framesFade;
        f += i;
    } else {
        f = framesRem;
    }

    while(i<frameCount && f > i) {
        fade = (double)(f-i) / (double)framesFade;
        s = (int32_t)data[i];
        s *= fade;
        s = CLAMP(s,-0x8000,0x7FFF);
        data[i] = (int16_t)s;
        i++;
    }

    while(i<frameCount) {
        data[i] = 0;
        i++;
    }

    return;
}


static void
fade_frames_stereo(int16_t *data, uint64_t framesRem, uint64_t framesFade, uint64_t frameCount) {
    uint64_t i = 0;
    uint64_t f = framesFade;
    double fade;
    int32_t sl, sr;

    if(framesRem > framesFade) {
        i = framesRem - framesFade;
        f += i;
    } else {
        f = framesRem;
    }

    while(i<frameCount && f > i) {
        fade = (double)(f-i) / (double)framesFade;
        sl = (int32_t)data[(i*2)+0];
        sr = (int32_t)data[(i*2)+1];
        sl *= fade;
        sr *= fade;
        sl = CLAMP(sl,-0x8000,0x7FFF);
        sr = CLAMP(sr,-0x8000,0x7FFF);
        data[(i*2)+0] = (int16_t)sl;
        data[(i*2)+1] = (int16_t)sr;
        i++;
    }

    while(i<frameCount) {
        data[(i*2)+0] = 0;
        data[(i*2)+1] = 0;
        i++;
    }

    return;
}

static void pack_frames_mono(uint8_t *d, int16_t *s, uint64_t frameCount) {
    uint64_t i = 0;
    while(i<frameCount) {
        pack_int16le(&d[0],s[i]);
        i++;
        d += 2;
    }
}

static void pack_frames_stereo(uint8_t *d, int16_t *s, uint64_t frameCount) {
    uint64_t i = 0;
    while(i<frameCount) {
        pack_int16le(&d[0],s[(i*2)+0]);
        pack_int16le(&d[2],s[(i*2)+1]);
        i++;
        d += 4;
    }
}

static uint64_t scan_uint(const char *str) {
    const char *s = str;
    uint64_t num = 0;
    while(*s) {
        if(*s < 48 || *s > 57) break;
        num *= 10;
        num += (*s - 48);
        s++;
    }

    return num;
}

static int usage(const char *self, int e) {
    printf("Usage: %s --mono --model=model --sample-rate=rate /path/to/file.gbs (/path/to/file.m3u)\n",self);
    return e;
}

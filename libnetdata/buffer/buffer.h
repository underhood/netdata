// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_WEB_BUFFER_H
#define NETDATA_WEB_BUFFER_H 1

#include "../libnetdata.h"

#define WEB_DATA_LENGTH_INCREASE_STEP 1024
#define BIG_BUFFER_GROWTH_RATIO 1.25

#define BUFFER_MEMPOOL_MAXSIZE 1024*16

#ifdef BUFFER_USE_MEMPOOL_PER_THREAD
    #ifndef BUFFER_USE_MEMPOOL
        #define BUFFER_USE_MEMPOOL
    #endif
    #define BUFFER_MEMPOOL_SIZE 5
#else
    #define BUFFER_MEMPOOL_SIZE 25
#endif

#ifdef BUFFER_USE_MEMPOOL
    #define BUFFER_MEMPOOL_MIN_SWAP_GROWTH 100
#endif

#ifdef BUFFER_USE_MEMPOOL
    #ifndef BUFFER_USE_MEMPOOL_PER_THREAD
        #error "Not implemented yet. To be implemented ASAP"
    #endif
#endif

typedef struct web_buffer {
    size_t size;        	// allocation size of buffer, in bytes
    size_t len;     		// current data length in buffer, in bytes
    char *buffer;   		// the buffer itself
    uint8_t contenttype;	// the content type of the data in the buffer
    uint8_t options;		// options related to the content
    time_t date;    		// the timestamp this content has been generated
    time_t expires;			// the timestamp this content expires
} BUFFER;

// options
#define WB_CONTENT_CACHEABLE            1
#define WB_CONTENT_NO_CACHEABLE         2
// use for buffers where keys and/or passwords are expected to be stored
// this will actually clear the memory before reuse of buffer
// adding additional security in case of programming errors
// at cost of some performance
#define WB_ALWAYS_CLEAR                 4

// content-types
#define CT_APPLICATION_JSON             1
#define CT_TEXT_PLAIN                   2
#define CT_TEXT_HTML                    3
#define CT_APPLICATION_X_JAVASCRIPT     4
#define CT_TEXT_CSS                     5
#define CT_TEXT_XML                     6
#define CT_APPLICATION_XML              7
#define CT_TEXT_XSL                     8
#define CT_APPLICATION_OCTET_STREAM     9
#define CT_APPLICATION_X_FONT_TRUETYPE  10
#define CT_APPLICATION_X_FONT_OPENTYPE  11
#define CT_APPLICATION_FONT_WOFF        12
#define CT_APPLICATION_FONT_WOFF2       13
#define CT_APPLICATION_VND_MS_FONTOBJ   14
#define CT_IMAGE_SVG_XML                15
#define CT_IMAGE_PNG                    16
#define CT_IMAGE_JPG                    17
#define CT_IMAGE_GIF                    18
#define CT_IMAGE_XICON                  19
#define CT_IMAGE_ICNS                   20
#define CT_IMAGE_BMP                    21
#define CT_PROMETHEUS                   22

#define buffer_cacheable(wb)    do { (wb)->options |= WB_CONTENT_CACHEABLE;    if((wb)->options & WB_CONTENT_NO_CACHEABLE) (wb)->options &= ~WB_CONTENT_NO_CACHEABLE; } while(0)
#define buffer_no_cacheable(wb) do { (wb)->options |= WB_CONTENT_NO_CACHEABLE; if((wb)->options & WB_CONTENT_CACHEABLE)    (wb)->options &= ~WB_CONTENT_CACHEABLE;  (wb)->expires = 0; } while(0)

#define buffer_strlen(wb) ((wb)->len)
extern const char *buffer_tostring(BUFFER *wb);

#define buffer_flush(wb) wb->buffer[(wb)->len = 0] = '\0'
extern void buffer_reset(BUFFER *wb);

extern void buffer_strcat(BUFFER *wb, const char *txt);
extern void buffer_rrd_value(BUFFER *wb, calculated_number value);

extern void buffer_date(BUFFER *wb, int year, int month, int day, int hours, int minutes, int seconds);
extern void buffer_jsdate(BUFFER *wb, int year, int month, int day, int hours, int minutes, int seconds);

extern BUFFER *_buffer_create(size_t size, uint8_t options);
static inline BUFFER *buffer_create(size_t size)
{
    return _buffer_create(size, 0);
}

static inline BUFFER *buffer_create_secure(size_t size) {
    return _buffer_create(size, WB_ALWAYS_CLEAR);
}

extern void buffer_free(BUFFER *b);
extern void buffer_increase(BUFFER *b, size_t free_size_required);

extern void buffer_snprintf(BUFFER *wb, size_t len, const char *fmt, ...) PRINTFLIKE(3, 4);
extern void buffer_vsprintf(BUFFER *wb, const char *fmt, va_list args);
extern void buffer_sprintf(BUFFER *wb, const char *fmt, ...) PRINTFLIKE(2,3);
extern void buffer_strcat_htmlescape(BUFFER *wb, const char *txt);

extern void buffer_char_replace(BUFFER *wb, char from, char to);

extern char *print_number_lu_r(char *str, unsigned long uvalue);
extern char *print_number_llu_r(char *str, unsigned long long uvalue);
extern char *print_number_llu_r_smart(char *str, unsigned long long uvalue);

extern void buffer_print_llu(BUFFER *wb, unsigned long long uvalue);

#ifdef BUFFER_MEMPOOL_STATS
#define BUFFER_MEMPOOL_CALLBACK_EVENT_FREE_AVOID            0x00000001
#define BUFFER_MEMPOOL_CALLBACK_EVENT_RECLAIM               0x00000002
#define BUFFER_MEMPOOL_CALLBACK_EVENT_ALLOC_MISS            0x00000004
#define BUFFER_MEMPOOL_CALLBACK_EVENT_SWAP                  0x00000008

#define BUFFER_MEMPOOL_CALLBACK_EVENT_CANTCACHE_TOOBIG      0x00000100
#define BUFFER_MEMPOOL_CALLBACK_EVENT_CANTCACHE_FULL        0x00000200

extern void buffer_mempool_stats_set_callback(void (*callback)(uint32_t));
#endif

extern char *buffer_mempool_status();

static inline void buffer_need_bytes(BUFFER *buffer, size_t needed_free_size) {
    if(unlikely(buffer->size - buffer->len < needed_free_size))
        buffer_increase(buffer, needed_free_size);
}

#endif /* NETDATA_WEB_BUFFER_H */

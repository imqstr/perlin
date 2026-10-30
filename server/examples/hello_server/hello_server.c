/* smallest useful server plugin, freestanding c (no libc):
 *   - welcomes players on join
 *   - cancels chat that contains "creeper"
 *   - registers /hello, which answers with the online player count
 * build: server/examples/hello_server/build.sh
 */
#include "../../sdk/perlin_server.h"

typedef uint32_t u32;

/* tiny bump allocator for event payloads: reset on every event, plugins never keep them */
static unsigned char heap[1 << 16];
static u32 heap_top;

PERLIN_EXPORT("perlin_alloc") void* perlin_alloc(u32 size) {
    if (heap_top + size > sizeof heap) heap_top = 0;
    void* p = heap + heap_top;
    heap_top += (size + 7u) & ~7u;
    return p;
}
PERLIN_EXPORT("perlin_free") void perlin_free(void* p) { (void)p; }

void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = d; const unsigned char* b = s;
    while (n--) *a++ = *b++;
    return d;
}

static u32 slen(const char* s) { u32 n = 0; while (s[n]) n++; return n; }

/* payload writer / reader: little endian, strings are u32 length + bytes */
static unsigned char out[512];
static u32 outn;
static void w_reset(void) { outn = 0; }
static void w_u8(unsigned char v) { out[outn++] = v; }
static void w_u32(u32 v) { for (int i = 0; i < 4; i++) out[outn++] = (unsigned char)(v >> (8 * i)); }
static void w_u64(unsigned long long v) { for (int i = 0; i < 8; i++) out[outn++] = (unsigned char)(v >> (8 * i)); }
static void w_str(const char* s, u32 n) { w_u32(n); for (u32 i = 0; i < n; i++) out[outn++] = (unsigned char)s[i]; }

static u32 r_u32(const unsigned char** p) { u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)(*p)[i] << (8 * i); *p += 4; return v; }
static unsigned long long r_u64(const unsigned char** p) { unsigned long long v = 0; for (int i = 0; i < 8; i++) v |= (unsigned long long)(*p)[i] << (8 * i); *p += 8; return v; }

static void say(unsigned long long player, const char* text, u32 n) {
    w_reset(); w_u64(player); w_str(text, n);
    perlin_call(PS_OP_MESSAGE, out, outn, 0, 0);
}

static int contains_creeper(const char* s, u32 n) {
    static const char word[] = "creeper";
    for (u32 i = 0; i + 7 <= n; i++) {
        u32 j = 0;
        while (j < 7 && (s[i + j] | 0x20) == word[j]) j++;
        if (j == 7) return 1;
    }
    return 0;
}

PERLIN_EXPORT("perlin_subscriptions") u32 perlin_subscriptions(void) {
    return (1u << PS_EVENT_PLAYER_JOIN) | (1u << PS_EVENT_CHAT) | (1u << PS_EVENT_COMMAND);
}

PERLIN_EXPORT("perlin_event") u32 perlin_event(u32 id, const unsigned char* payload, u32 len) {
    (void)len;
    switch (id) {
    case PS_EVENT_ENABLE: {
        static const char name[] = "hello", desc[] = "Say hello from a plugin";
        w_reset(); w_str(name, slen(name)); w_str(desc, slen(desc));
        perlin_call(PS_OP_REGISTER_COMMAND, out, outn, 0, 0);
        static const char msg[] = "hello_server enabled";
        w_reset(); w_u8(1); w_str(msg, slen(msg));
        perlin_call(PS_OP_LOG, out, outn, 0, 0);
        return 0;
    }
    case PS_EVENT_PLAYER_JOIN: {
        const unsigned char* p = payload;
        unsigned long long player = r_u64(&p);
        u32 n = r_u32(&p);
        char text[96];
        static const char hi[] = "Welcome, ";
        u32 k = 0;
        for (u32 i = 0; hi[i]; i++) text[k++] = hi[i];
        for (u32 i = 0; i < n && k < 90; i++) text[k++] = (char)p[i];
        text[k++] = '!';
        say(player, text, k);
        return 0;
    }
    case PS_EVENT_CHAT: {
        const unsigned char* p = payload;
        unsigned long long player = r_u64(&p);
        u32 nameLen = r_u32(&p); p += nameLen;
        u32 msgLen = r_u32(&p);
        if (contains_creeper((const char*)p, msgLen)) {
            static const char refuse[] = "We do not say that word here.";
            say(player, refuse, slen(refuse));
            return 1; /* cancel */
        }
        return 0;
    }
    case PS_EVENT_COMMAND: {
        const unsigned char* p = payload;
        unsigned long long player = r_u64(&p);
        unsigned char reply[256];
        static const unsigned char in[1] = {0};
        int n = perlin_call(PS_OP_PLAYERS, in, 0, reply, sizeof reply);
        u32 count = 0;
        if (n >= 4) { const unsigned char* q = reply; count = r_u32(&q); }
        char text[48] = "Hello! Players online: ";
        u32 k = slen(text);
        text[k++] = (char)('0' + (count % 10));
        text[k] = 0;
        say(player, text, k);
        return 1;
    }
    }
    return 0;
}

/* Modula-2 Runtime Support */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <float.h>
#include <setjmp.h>

/* Command-line argument storage */
static int m2_argc = 0;
static char **m2_argv = NULL;

/* Stack trace support — lightweight frame tracking for crash diagnostics */
typedef struct m2_StackFrame {
    struct m2_StackFrame *prev;
    const char *proc_name;
    const char *file;
    int line;
} m2_StackFrame;

static __thread m2_StackFrame *m2_frame_stack = NULL;

static void m2_stack_push(m2_StackFrame *frame, const char *proc, const char *file) {
    frame->prev = m2_frame_stack;
    frame->proc_name = proc;
    frame->file = file;
    frame->line = 0;
    m2_frame_stack = frame;
}

static void m2_stack_pop(m2_StackFrame *frame) {
    m2_frame_stack = frame->prev;
}

static void m2_print_stack_trace(void) {
    m2_StackFrame *f = m2_frame_stack;
    int depth = 0;
    if (!f) return;
    fprintf(stderr, "Stack trace (most recent call first):\n");
    while (f && depth < 50) {
        if (f->line > 0) {
            fprintf(stderr, "  #%d %s (%s:%d)\n", depth, f->proc_name, f->file, f->line);
        } else {
            fprintf(stderr, "  #%d %s (%s)\n", depth, f->proc_name, f->file);
        }
        f = f->prev;
        depth++;
    }
}

/* ISO Modula-2 exception handling support */
static jmp_buf m2_exception_buf;
static int m2_exception_code = 0;
static int m2_exception_active = 0;

/* Modula-2+ enhanced exception handling (setjmp/longjmp frame stack) */
typedef struct m2_ExcFrame {
    jmp_buf buf;
    struct m2_ExcFrame *prev;
    int exception_id;
    const char *exception_name;
    void *exception_arg;
} m2_ExcFrame;

static __thread m2_ExcFrame *m2_exc_stack = NULL;

/* Stack-based exception frame macros — no heap allocation.
   Usage:  m2_ExcFrame _ef;
           M2_TRY(_ef) { body; M2_ENDTRY(_ef); }
           M2_CATCH { M2_ENDTRY(_ef); handlers; }           */
#define M2_TRY(frame) \
    (frame).prev = m2_exc_stack; \
    (frame).exception_id = 0; \
    (frame).exception_name = NULL; \
    (frame).exception_arg = NULL; \
    m2_exc_stack = &(frame); \
    if (setjmp((frame).buf) == 0)

#define M2_CATCH else

#define M2_ENDTRY(frame) \
    m2_exc_stack = (frame).prev

/* Callable wrappers for LLVM backend (can't use macros from IR) */
static void m2_exc_push(m2_ExcFrame *frame) {
    frame->prev = m2_exc_stack;
    frame->exception_id = 0;
    frame->exception_name = NULL;
    frame->exception_arg = NULL;
    m2_exc_stack = frame;
}
static void m2_exc_pop(m2_ExcFrame *frame) {
    m2_exc_stack = frame->prev;
}
static int m2_exc_get_id(m2_ExcFrame *frame) {
    return frame->exception_id;
}
static void m2_exc_reraise(m2_ExcFrame *frame);  /* forward decl */

static inline void m2_raise(int id, const char *name, void *arg) {
    if (m2_exc_stack) {
        m2_exc_stack->exception_id = id;
        m2_exc_stack->exception_name = name;
        m2_exc_stack->exception_arg = arg;
        longjmp(m2_exc_stack->buf, id ? id : 1);
    }
    /* Fallback to ISO exception mechanism */
    if (m2_exception_active) {
        m2_exception_code = id ? id : 1;
        longjmp(m2_exception_buf, m2_exception_code);
    }
    /* No handler — terminate with stack trace */
    fprintf(stderr, "Unhandled exception: %s (id=%d)\n", name ? name : "unknown", id);
    m2_print_stack_trace();
    exit(1);
}

static void m2_exc_reraise(m2_ExcFrame *frame) {
    m2_raise(frame->exception_id, frame->exception_name, frame->exception_arg);
}

static void m2_halt(void) {
    exit(0);
}

/* Runtime type information (for TYPECASE / OBJECT) */
typedef struct M2_TypeDesc {
    uint32_t   type_id;
    const char *type_name;
    struct M2_TypeDesc *parent;
    uint32_t   depth;
} M2_TypeDesc;

/* Allocation header prepended before payload for typed REF/OBJECT allocations */
typedef struct M2_RefHeader {
#ifdef M2_RTTI_DEBUG
    uint32_t magic;   /* 0x4D325246 ("M2RF") */
    uint32_t flags;   /* 0 = live, 0xDEADDEAD = freed */
#endif
    M2_TypeDesc *td;
} M2_RefHeader;

#define M2_REFHEADER_MAGIC 0x4D325246u

/* Modula-2+ Thread support (pthreads) */
#ifdef M2_USE_THREADS
#include <pthread.h>
typedef struct m2_Thread {
    pthread_t handle;
    int alerted;
    pthread_mutex_t alert_mu;
} m2_Thread;

static __thread m2_Thread *m2_current_thread = NULL;

/* Thread.Fork — create a new thread from a parameterless procedure */
typedef void (*m2_ThreadProc)(void);
struct m2_thread_start_arg { m2_ThreadProc proc; m2_Thread *self; };

static void *m2_thread_start(void *arg) {
    struct m2_thread_start_arg *a = (struct m2_thread_start_arg *)arg;
    m2_current_thread = a->self;
    a->proc();
    free(a);
    return NULL;
}

static m2_Thread *m2_Thread_Fork(m2_ThreadProc proc) {
    m2_Thread *t = (m2_Thread *)malloc(sizeof(m2_Thread));
    t->alerted = 0;
    pthread_mutex_init(&t->alert_mu, NULL);
    struct m2_thread_start_arg *arg = (struct m2_thread_start_arg *)malloc(sizeof(struct m2_thread_start_arg));
    arg->proc = proc;
    arg->self = t;
    pthread_create(&t->handle, NULL, m2_thread_start, arg);
    return t;
}

static void m2_Thread_Join(m2_Thread *t) {
    pthread_join(t->handle, NULL);
}

static m2_Thread *m2_Thread_Self(void) {
    return m2_current_thread;
}

static void m2_Thread_Alert(m2_Thread *t) {
    pthread_mutex_lock(&t->alert_mu);
    t->alerted = 1;
    pthread_mutex_unlock(&t->alert_mu);
}

static int m2_Thread_TestAlert(void) {
    if (!m2_current_thread) return 0;
    pthread_mutex_lock(&m2_current_thread->alert_mu);
    int a = m2_current_thread->alerted;
    m2_current_thread->alerted = 0;
    pthread_mutex_unlock(&m2_current_thread->alert_mu);
    return a;
}

/* Mutex module */
typedef pthread_mutex_t *m2_Mutex_T;

static m2_Mutex_T m2_Mutex_New(void) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, NULL);
    return m;
}

static void m2_Mutex_Lock(m2_Mutex_T m) { pthread_mutex_lock(m); }
static void m2_Mutex_Unlock(m2_Mutex_T m) { pthread_mutex_unlock(m); }
static void m2_Mutex_Free(m2_Mutex_T m) { pthread_mutex_destroy(m); free(m); }

/* Condition module */
typedef pthread_cond_t *m2_Condition_T;

static m2_Condition_T m2_Condition_New(void) {
    pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    pthread_cond_init(c, NULL);
    return c;
}

static void m2_Condition_Wait(m2_Condition_T c, m2_Mutex_T m) { pthread_cond_wait(c, m); }
static void m2_Condition_Signal(m2_Condition_T c) { pthread_cond_signal(c); }
static void m2_Condition_Broadcast(m2_Condition_T c) { pthread_cond_broadcast(c); }
static void m2_Condition_Free(m2_Condition_T c) { pthread_cond_destroy(c); free(c); }
#endif /* M2_USE_THREADS */

/* Modula-2+ Garbage Collection support (Boehm GC) */
#if defined(M2_USE_GC) && __has_include(<gc/gc.h>)
#include <gc/gc.h>
#else
/* Fallback: use malloc when GC is not available */
#ifdef M2_USE_GC
#undef M2_USE_GC
#endif
#define GC_MALLOC(sz) malloc(sz)
#define GC_REALLOC(p, sz) realloc(p, sz)
#define GC_FREE(p) free(p)
static inline void GC_INIT(void) {}
#endif

/* Allocate a GC-traced REF/OBJECT with M2_RefHeader prepended before payload */
static inline void *M2_ref_alloc(size_t payload_size, M2_TypeDesc *td) {
    M2_RefHeader *hdr = (M2_RefHeader *)GC_MALLOC(sizeof(M2_RefHeader) + payload_size);
    if (!hdr) { fprintf(stderr, "M2_ref_alloc: out of memory\n"); exit(1); }
#ifdef M2_RTTI_DEBUG
    hdr->magic = M2_REFHEADER_MAGIC;
    hdr->flags = 0;
#endif
    hdr->td = td;
    return (void *)(hdr + 1); /* return pointer to payload (past header) */
}

/* Recover the type descriptor from a typed REF/REFANY payload pointer.
   Returns NULL if ref is NULL or (in debug mode) if the header is invalid. */
static inline M2_TypeDesc *M2_TYPEOF(void *ref) {
    if (!ref) return NULL;
    M2_RefHeader *hdr = ((M2_RefHeader *)ref) - 1;
#ifdef M2_RTTI_DEBUG
    if (hdr->magic != M2_REFHEADER_MAGIC) return NULL;
    if (hdr->flags == 0xDEADDEADu) {
        fprintf(stderr, "M2_TYPEOF: use-after-free detected\n");
        return NULL;
    }
#endif
    return hdr->td;
}

/* Check if a payload's type is (or inherits from) a target type descriptor.
   Returns 1 if match, 0 otherwise. Safe with NULL payloads. */
static inline int M2_ISA(void *payload, M2_TypeDesc *target) {
    M2_TypeDesc *td = M2_TYPEOF(payload);
    if (!td || !target) return 0;
    if (td->depth < target->depth) return 0; /* early-out: can't be a subtype */
    while (td) {
        if (td == target) return 1;
        td = td->parent;
    }
    return 0;
}

/* Narrow: returns payload if it matches target type, otherwise raises an exception */
static inline void *M2_NARROW(void *payload, M2_TypeDesc *target) {
    if (M2_ISA(payload, target)) return payload;
    m2_raise(99, "NarrowFault", NULL);
    return NULL; /* unreachable */
}

/* Free a typed REF object — poisons header in debug mode */
static inline void M2_ref_free(void *payload) {
    if (!payload) return;
    M2_RefHeader *hdr = ((M2_RefHeader *)payload) - 1;
#ifdef M2_RTTI_DEBUG
    hdr->flags = 0xDEADDEADu;
#endif
    GC_FREE(hdr);
}

/* PIM4 DIV: floored division (truncates toward negative infinity) */
static inline int32_t m2_div(int32_t a, int32_t b) {
    int32_t q = a / b;
    int32_t r = a % b;
    if ((r != 0) && ((r ^ b) < 0)) q--;
    return q;
}

/* PIM4 MOD: result is always non-negative (when b > 0) */
static inline int32_t m2_mod(int32_t a, int32_t b) {
    int32_t r = a % b;
    if (r < 0) r += (b > 0 ? b : -b);
    return r;
}

/* PIM4 DIV64: 64-bit floored division for LONGINT */
static inline int64_t m2_div64(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if ((r != 0) && ((r ^ b) < 0)) q--;
    return q;
}

/* PIM4 MOD64: 64-bit modulo for LONGINT */
static inline int64_t m2_mod64(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r < 0) r += (b > 0 ? b : -b);
    return r;
}

/* ISO Modula-2 COMPLEX types */
typedef struct { float re, im; } m2_COMPLEX;
typedef struct { double re, im; } m2_LONGCOMPLEX;

static inline m2_COMPLEX m2_complex_add(m2_COMPLEX a, m2_COMPLEX b) {
    return (m2_COMPLEX){ a.re + b.re, a.im + b.im };
}
static inline m2_COMPLEX m2_complex_sub(m2_COMPLEX a, m2_COMPLEX b) {
    return (m2_COMPLEX){ a.re - b.re, a.im - b.im };
}
static inline m2_COMPLEX m2_complex_mul(m2_COMPLEX a, m2_COMPLEX b) {
    return (m2_COMPLEX){ a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re };
}
static inline m2_COMPLEX m2_complex_div(m2_COMPLEX a, m2_COMPLEX b) {
    float d = b.re*b.re + b.im*b.im;
    return (m2_COMPLEX){ (a.re*b.re + a.im*b.im)/d, (a.im*b.re - a.re*b.im)/d };
}
static inline int m2_complex_eq(m2_COMPLEX a, m2_COMPLEX b) {
    return a.re == b.re && a.im == b.im;
}
static inline m2_COMPLEX m2_complex_neg(m2_COMPLEX a) {
    return (m2_COMPLEX){ -a.re, -a.im };
}
static inline float m2_complex_abs(m2_COMPLEX a) {
    return sqrtf(a.re*a.re + a.im*a.im);
}
static inline m2_LONGCOMPLEX m2_lcomplex_add(m2_LONGCOMPLEX a, m2_LONGCOMPLEX b) {
    return (m2_LONGCOMPLEX){ a.re + b.re, a.im + b.im };
}
static inline m2_LONGCOMPLEX m2_lcomplex_sub(m2_LONGCOMPLEX a, m2_LONGCOMPLEX b) {
    return (m2_LONGCOMPLEX){ a.re - b.re, a.im - b.im };
}
static inline m2_LONGCOMPLEX m2_lcomplex_mul(m2_LONGCOMPLEX a, m2_LONGCOMPLEX b) {
    return (m2_LONGCOMPLEX){ a.re*b.re - a.im*b.im, a.re*b.im + a.im*b.re };
}
static inline m2_LONGCOMPLEX m2_lcomplex_div(m2_LONGCOMPLEX a, m2_LONGCOMPLEX b) {
    double d = b.re*b.re + b.im*b.im;
    return (m2_LONGCOMPLEX){ (a.re*b.re + a.im*b.im)/d, (a.im*b.re - a.re*b.im)/d };
}
static inline int m2_lcomplex_eq(m2_LONGCOMPLEX a, m2_LONGCOMPLEX b) {
    return a.re == b.re && a.im == b.im;
}
static inline m2_LONGCOMPLEX m2_lcomplex_neg(m2_LONGCOMPLEX a) {
    return (m2_LONGCOMPLEX){ -a.re, -a.im };
}
static inline double m2_lcomplex_abs(m2_LONGCOMPLEX a) {
    return sqrt(a.re*a.re + a.im*a.im);
}

/* Built-in MAX/MIN - type-generic via macros */
#define m2_max_INTEGER INT32_MAX
#define m2_max_CARDINAL UINT32_MAX
#define m2_max_CHAR 255
#define m2_max_BOOLEAN 1
#define m2_max_REAL FLT_MAX
#define m2_max_LONGREAL DBL_MAX
#define m2_max_BITSET 31
#define m2_max_LONGINT INT64_MAX
#define m2_max_LONGCARD UINT64_MAX
#define m2_min_INTEGER INT32_MIN
#define m2_min_CARDINAL 0
#define m2_min_CHAR 0
#define m2_min_BOOLEAN 0
#define m2_min_REAL FLT_MIN
#define m2_min_LONGREAL DBL_MIN
#define m2_min_BITSET 0
#define m2_min_LONGINT INT64_MIN
#define m2_min_LONGCARD 0
#define m2_max(T) m2_max_##T
#define m2_min(T) m2_min_##T

/* ISO SYSTEM.SHIFT — positive n shifts left, negative shifts right, vacated bits = 0 */
static inline uint32_t m2_shift(uint32_t val, int32_t n) {
    if (n == 0) return val;
    if (n > 0) return (n >= 32) ? 0u : (val << n);
    n = -n;
    return (n >= 32) ? 0u : (val >> n);
}
/* ISO SYSTEM.ROTATE — positive n rotates left, negative rotates right */
static inline uint32_t m2_rotate(uint32_t val, int32_t n) {
    n = n % 32;
    if (n < 0) n += 32;
    if (n == 0) return val;
    return (val << n) | (val >> (32 - n));
}

/* InOut module */
static int m2_InOut_Done = 1;
static void m2_WriteString(const char *s) { printf("%s", s); }
static void m2_WriteLn(void) { printf("\n"); }
static void m2_WriteInt(int32_t n, int32_t w) { printf("%*d", (int)w, (int)n); }
static void m2_WriteCard(uint32_t n, int32_t w) { printf("%*u", (int)w, (unsigned)n); }
static void m2_WriteHex(uint32_t n, int32_t w) { printf("%*X", (int)w, (unsigned)n); }
static void m2_WriteOct(uint32_t n, int32_t w) { printf("%*o", (int)w, (unsigned)n); }
static void m2_Write(char ch) { putchar(ch); }
static void m2_Read(char *ch) { int c = getchar(); *ch = (c == EOF) ? '\0' : (char)c; m2_InOut_Done = (c != EOF); }
static void m2_ReadString(char *s) { m2_InOut_Done = (scanf("%s", s) == 1); }
static void m2_ReadInt(int32_t *n) { m2_InOut_Done = (scanf("%d", n) == 1); }
static void m2_ReadCard(uint32_t *n) { m2_InOut_Done = (scanf("%u", n) == 1); }

static FILE *m2_InFile = NULL;
static FILE *m2_OutFile = NULL;
static void m2_OpenInput(const char *ext) {
    char name[256];
    printf("Input file: "); scanf("%255s", name);
    if (ext && ext[0]) { strcat(name, "."); strcat(name, ext); }
    m2_InFile = fopen(name, "r");
    m2_InOut_Done = (m2_InFile != NULL);
}
static void m2_OpenOutput(const char *ext) {
    char name[256];
    printf("Output file: "); scanf("%255s", name);
    if (ext && ext[0]) { strcat(name, "."); strcat(name, ext); }
    m2_OutFile = fopen(name, "w");
    m2_InOut_Done = (m2_OutFile != NULL);
}
static void m2_CloseInput(void) { if (m2_InFile) { fclose(m2_InFile); m2_InFile = NULL; } }
static void m2_CloseOutput(void) { if (m2_OutFile) { fclose(m2_OutFile); m2_OutFile = NULL; } }

/* RealInOut module */
static int m2_RealInOut_Done = 1;
static void m2_ReadReal(float *r) { m2_RealInOut_Done = (scanf("%f", r) == 1); }
static void m2_WriteReal(float r, int32_t w) { printf("%*g", (int)w, (double)r); }
static void m2_WriteFixPt(float r, int32_t w, int32_t d) { printf("%*.*f", (int)w, (int)d, (double)r); }
static void m2_WriteRealOct(float r) { printf("%.8A", (double)r); }

/* MathLib — Random/Randomize */
static float m2_Random(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }
static void m2_Randomize(uint32_t seed) { srand(seed); }

/* Storage module */
static void m2_ALLOCATE(void **p, uint32_t size) { *p = malloc(size); }
static void m2_DEALLOCATE(void **p, uint32_t size) { free(*p); *p = NULL; (void)size; }

/* Strings module — bounded, always NUL-terminates, truncates on overflow.
   When both source length and capacity are compile-time constants (e.g. a
   string literal assigned to a fixed-size array), the branch resolves at
   compile time and the copy becomes a single memcpy/strcpy intrinsic that
   downstream optimisations (constant-folding of strcmp, etc.) can see through. */
static inline __attribute__((always_inline)) void m2_Strings_Assign(const char *src, char *dst, uint32_t dst_high) {
    size_t cap = (size_t)dst_high + 1;
    size_t slen = __builtin_strlen(src);
    if (__builtin_constant_p(slen) && __builtin_constant_p(cap) && slen < cap) {
        __builtin_memcpy(dst, src, slen + 1);
    } else {
        if (slen >= cap) slen = cap - 1;
        __builtin_memcpy(dst, src, slen);
        dst[slen] = '\0';
    }
}
static void m2_Strings_Insert(const char *sub, char *dst, uint32_t dst_high, uint32_t pos) {
    size_t cap = (size_t)dst_high + 1;
    size_t slen = strlen(sub), dlen = strlen(dst);
    if (pos > dlen) pos = (uint32_t)dlen;
    size_t new_len = dlen + slen;
    if (new_len >= cap) new_len = cap - 1;
    /* how much of the tail after pos can we keep? */
    size_t tail_dst = pos + slen;
    size_t tail_keep = (tail_dst < new_len) ? new_len - tail_dst : 0;
    if (tail_keep > 0)
        memmove(dst + tail_dst, dst + pos, tail_keep);
    /* how much of sub fits? */
    size_t sub_copy = slen;
    if (pos + sub_copy > new_len) sub_copy = new_len - pos;
    if (sub_copy > 0)
        memcpy(dst + pos, sub, sub_copy);
    dst[new_len] = '\0';
}
static void m2_Strings_Delete(char *s, uint32_t s_high, uint32_t pos, uint32_t len) {
    size_t slen = strlen(s);
    (void)s_high; /* delete only shrinks — can never overflow */
    if (pos >= slen) return;
    if (pos + len > slen) len = (uint32_t)(slen - pos);
    memmove(s + pos, s + pos + len, slen - pos - len + 1);
}
static uint32_t m2_Strings_Pos(const char *sub, const char *s) {
    const char *p = strstr(s, sub);
    return p ? (uint32_t)(p - s) : UINT32_MAX;
}
static uint32_t m2_Strings_Length(const char *s) { return (uint32_t)strlen(s); }
static void m2_Strings_Copy(const char *src, uint32_t pos, uint32_t len, char *dst, uint32_t dst_high) {
    size_t cap = (size_t)dst_high + 1;
    size_t slen = strlen(src);
    if (pos >= slen) { dst[0] = '\0'; return; }
    if (pos + len > slen) len = (uint32_t)(slen - pos);
    if (len >= cap) len = (uint32_t)(cap - 1);
    memcpy(dst, src + pos, len);
    dst[len] = '\0';
}
static void m2_Strings_Concat(const char *s1, const char *s2, char *dst, uint32_t dst_high) {
    size_t cap = (size_t)dst_high + 1;
    size_t len1 = strlen(s1), len2 = strlen(s2);
    if (len1 >= cap) len1 = cap - 1;
    memcpy(dst, s1, len1);
    size_t rem = cap - 1 - len1;
    if (len2 > rem) len2 = rem;
    memcpy(dst + len1, s2, len2);
    dst[len1 + len2] = '\0';
}
static int32_t m2_Strings_CompareStr(const char *s1, const char *s2) { return (int32_t)strcmp(s1, s2); }
static void m2_Strings_CAPS(char *s, uint32_t s_high) { for (uint32_t i = 0; i <= s_high && s[i]; i++) s[i] = (char)toupper((unsigned char)s[i]); }

/* Terminal module */
static int m2_Terminal_Done = 1;
static void m2_Terminal_Read(char *ch) { int c = getchar(); *ch = (c == EOF) ? '\0' : (char)c; m2_Terminal_Done = (c != EOF); }
static void m2_Terminal_Write(char ch) { putchar(ch); }
static void m2_Terminal_WriteString(const char *s) { printf("%s", s); }
static void m2_Terminal_WriteLn(void) { printf("\n"); }

/* FileSystem module */
typedef FILE *m2_File;
static int m2_FileSystem_Done = 1;
static void m2_Lookup(m2_File *f, const char *name, int newFile) {
    *f = fopen(name, newFile ? "w+" : "r+");
    if (!*f && !newFile) *f = fopen(name, "r");
    m2_FileSystem_Done = (*f != NULL);
}
static void m2_Close(m2_File *f) { if (*f) { fclose(*f); *f = NULL; } }
static void m2_ReadChar(m2_File *f, char *ch) {
    int c = fgetc(*f);
    *ch = (c == EOF) ? '\0' : (char)c;
    m2_FileSystem_Done = (c != EOF);
}
static void m2_WriteChar(m2_File *f, char ch) {
    fputc(ch, *f);
}

/* SYSTEM module */
#define m2_ADR(x) ((void *)&(x))
#define m2_TSIZE(T) ((uint32_t)sizeof(T))

/* ISO STextIO module */
static void m2_STextIO_WriteChar(char ch) { putchar(ch); }
static void m2_STextIO_ReadChar(char *ch) { int c = getchar(); *ch = (c == EOF) ? '\0' : (char)c; }
static void m2_STextIO_WriteString(const char *s) { printf("%s", s); }
static void m2_STextIO_ReadString(char *s, uint32_t s_high) {
    if (fgets(s, (int)(s_high + 1), stdin) == NULL) s[0] = '\0';
    /* strip trailing newline */
    size_t len = strlen(s);
    if (len > 0 && s[len-1] == '\n') s[len-1] = '\0';
}
static void m2_STextIO_WriteLn(void) { putchar('\n'); }
static void m2_STextIO_SkipLine(void) { int c; while ((c = getchar()) != '\n' && c != EOF); }
static void m2_STextIO_ReadToken(char *s, uint32_t s_high) { m2_STextIO_ReadString(s, s_high); }

/* ISO SWholeIO module */
static void m2_SWholeIO_WriteInt(int32_t n, uint32_t w) { printf("%*d", (int)w, (int)n); }
static void m2_SWholeIO_ReadInt(int32_t *n) { scanf("%d", (int *)n); }
static void m2_SWholeIO_WriteCard(uint32_t n, uint32_t w) { printf("%*u", (int)w, (unsigned)n); }
static void m2_SWholeIO_ReadCard(uint32_t *n) { scanf("%u", (unsigned *)n); }

/* ISO SRealIO module */
static void m2_SRealIO_WriteFloat(float r, uint32_t sigFigs, uint32_t w) {
    printf("%*.*e", (int)w, (int)sigFigs, (double)r);
}
static void m2_SRealIO_WriteFixed(float r, int32_t place, uint32_t w) {
    printf("%*.*f", (int)w, (int)place, (double)r);
}
static void m2_SRealIO_WriteReal(float r, uint32_t w) { printf("%*g", (int)w, (double)r); }
static void m2_SRealIO_ReadReal(float *r) { double d; scanf("%lf", &d); *r = (float)d; }

/* ISO SLongIO module */
static void m2_SLongIO_WriteFloat(double r, uint32_t sigFigs, uint32_t w) {
    printf("%*.*e", (int)w, (int)sigFigs, r);
}
static void m2_SLongIO_WriteFixed(double r, int32_t place, uint32_t w) {
    printf("%*.*f", (int)w, (int)place, r);
}
static void m2_SLongIO_WriteLongReal(double r, uint32_t w) { printf("%*g", (int)w, r); }
static void m2_SLongIO_ReadLongReal(double *r) { scanf("%lf", r); }

/* Args module */
static uint32_t m2_Args_ArgCount(void) { return (uint32_t)m2_argc; }
static void m2_Args_GetArg(uint32_t n, char *buf, uint32_t buf_high) {
    (void)buf_high;
    if ((int)n < m2_argc) {
        strncpy(buf, m2_argv[n], buf_high + 1);
        buf[buf_high] = '\0';
    } else {
        buf[0] = '\0';
    }
}

/* BinaryIO module - file handle table using FILE* pointers */
#define M2_MAX_FILES 32
static FILE *m2_bio_files[M2_MAX_FILES];
static int m2_bio_init = 0;
static int m2_BinaryIO_Done = 1;

static void m2_bio_ensure_init(void) {
    if (!m2_bio_init) {
        for (int i = 0; i < M2_MAX_FILES; i++) m2_bio_files[i] = NULL;
        m2_bio_init = 1;
    }
}

static int m2_bio_alloc(void) {
    m2_bio_ensure_init();
    for (int i = 0; i < M2_MAX_FILES; i++) {
        if (m2_bio_files[i] == NULL) return i;
    }
    return -1;
}

static void m2_BinaryIO_OpenRead(const char *name, uint32_t *fh) {
    int slot = m2_bio_alloc();
    if (slot < 0) { m2_BinaryIO_Done = 0; *fh = 0; return; }
    m2_bio_files[slot] = fopen(name, "rb");
    if (m2_bio_files[slot]) { *fh = (uint32_t)(slot + 1); m2_BinaryIO_Done = 1; }
    else { *fh = 0; m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_OpenWrite(const char *name, uint32_t *fh) {
    int slot = m2_bio_alloc();
    if (slot < 0) { m2_BinaryIO_Done = 0; *fh = 0; return; }
    m2_bio_files[slot] = fopen(name, "wb");
    if (m2_bio_files[slot]) { *fh = (uint32_t)(slot + 1); m2_BinaryIO_Done = 1; }
    else { *fh = 0; m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_Close(uint32_t fh) {
    m2_bio_ensure_init();
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        fclose(m2_bio_files[fh-1]);
        m2_bio_files[fh-1] = NULL;
    }
}

static void m2_BinaryIO_ReadByte(uint32_t fh, uint32_t *b) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        int c = fgetc(m2_bio_files[fh-1]);
        if (c == EOF) { *b = 0; m2_BinaryIO_Done = 0; }
        else { *b = (uint32_t)(unsigned char)c; m2_BinaryIO_Done = 1; }
    } else { *b = 0; m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_WriteByte(uint32_t fh, uint32_t b) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        fputc((unsigned char)(b & 0xFF), m2_bio_files[fh-1]);
        m2_BinaryIO_Done = 1;
    } else { m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_ReadBytes(uint32_t fh, char *buf, uint32_t n, uint32_t *actual) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        *actual = (uint32_t)fread(buf, 1, n, m2_bio_files[fh-1]);
        m2_BinaryIO_Done = (*actual > 0) ? 1 : 0;
    } else { *actual = 0; m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_WriteBytes(uint32_t fh, const char *buf, uint32_t n) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        fwrite(buf, 1, n, m2_bio_files[fh-1]);
        m2_BinaryIO_Done = 1;
    } else { m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_FileSize(uint32_t fh, uint32_t *size) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        long cur = ftell(m2_bio_files[fh-1]);
        fseek(m2_bio_files[fh-1], 0, SEEK_END);
        *size = (uint32_t)ftell(m2_bio_files[fh-1]);
        fseek(m2_bio_files[fh-1], cur, SEEK_SET);
        m2_BinaryIO_Done = 1;
    } else { *size = 0; m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_Seek(uint32_t fh, uint32_t pos) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        fseek(m2_bio_files[fh-1], (long)pos, SEEK_SET);
        m2_BinaryIO_Done = 1;
    } else { m2_BinaryIO_Done = 0; }
}

static void m2_BinaryIO_Tell(uint32_t fh, uint32_t *pos) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        *pos = (uint32_t)ftell(m2_bio_files[fh-1]);
        m2_BinaryIO_Done = 1;
    } else { *pos = 0; m2_BinaryIO_Done = 0; }
}

static int m2_BinaryIO_IsEOF(uint32_t fh) {
    if (fh >= 1 && fh <= M2_MAX_FILES && m2_bio_files[fh-1]) {
        return feof(m2_bio_files[fh-1]) ? 1 : 0;
    }
    return 1;
}

/* Target layout guards — compile-time validation */
_Static_assert(sizeof(void *) == 8, "pointer size mismatch: expected 8 bytes");
_Static_assert(sizeof(int32_t) == 4, "int32_t size mismatch");
_Static_assert(sizeof(int64_t) == 8, "int64_t size mismatch");
_Static_assert(sizeof(float) == 4, "float size mismatch");
_Static_assert(sizeof(double) == 8, "double size mismatch");
_Static_assert(_Alignof(void *) == 8, "pointer alignment mismatch");
/* Foreign C bindings: GfxBridge */
extern void gfx_update_logical_size(void * ren, void * win);
extern int32_t gfx_ttf_init(void);
extern void gfx_warp_mouse(void * win, int32_t x, int32_t y);
extern void gfx_delay(int32_t ms);
extern void gfx_log(void * path, void * msg);
extern void gfx_get_color(void * ren, int32_t *r, int32_t *g, int32_t *b, int32_t *a);
extern void gfx_set_clipboard(void * text);
extern void gfx_hide_win(void * win);
extern void gfx_set_clip(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
extern void gfx_set_win_max_size(void * win, int32_t w, int32_t h);
extern int32_t gfx_tex_width(void * tex);
extern void gfx_ttf_quit(void);
extern void gfx_pb_set(void * pb, int32_t x, int32_t y, int32_t idx);
extern void gfx_clear(void * ren);
extern int32_t gfx_font_height(void * font);
extern int32_t gfx_pb_height(void * pb);
extern int32_t gfx_is_text_active(void);
extern int32_t gfx_screen_height(void);
extern void gfx_pb_fill_row(void * pb, int32_t x, int32_t y, int32_t w, int32_t idx);
extern void gfx_minimize_win(void * win);
extern int32_t gfx_text_width(void * font, void * text);
extern void gfx_pb_rgba_set32(void * pb, int32_t offset, int32_t val);
extern void gfx_set_win_pos(void * win, int32_t x, int32_t y);
extern int32_t gfx_event_mouse_btn(void);
extern int32_t gfx_pb_save_h(void * region);
extern void gfx_set_blend(void * ren, int32_t mode);
extern int32_t gfx_tex_height(void * tex);
extern int32_t gfx_init(void);
extern int32_t gfx_event_mouse_y(void);
extern int32_t gfx_event_mod(void);
extern void * gfx_create_texture(void * ren, int32_t w, int32_t h);
extern void * gfx_load_bmp_keyed(void * ren, void * path, int32_t kr, int32_t kg, int32_t kb);
extern int32_t gfx_event_key(void);
extern int32_t gfx_key_state(int32_t scancode);
extern void gfx_pb_set_pal(void * pb, int32_t idx, int32_t r, int32_t g, int32_t b);
extern int32_t gfx_get_clip_y(void * ren);
extern void * gfx_pb_pixel_ptr(void * pb);
extern void gfx_raise_win(void * win);
extern void gfx_draw_text_wrapped(void * ren, void * font, void * text, int32_t x, int32_t y, int32_t wrapWidth, int32_t r, int32_t g, int32_t b, int32_t a);
extern int32_t gfx_font_get_style(void * font);
extern void * gfx_alloc(int32_t bytes);
extern void * gfx_pb_create(int32_t w, int32_t h);
extern int32_t gfx_mouse_global(int32_t *x, int32_t *y);
extern void gfx_pb_composite(void * dst, void * src, int32_t transparentIdx);
extern void gfx_set_tex_alpha(void * tex, int32_t alpha);
extern void gfx_set_color(void * ren, int32_t r, int32_t g, int32_t b, int32_t a);
extern void * gfx_text_texture(void * ren, void * font, void * text, int32_t r, int32_t g, int32_t b, int32_t a);
extern void gfx_start_text(void);
extern void gfx_buf_set(void * buf, int32_t offset, int32_t val);
extern int32_t gfx_event_wheel_x(void);
extern void gfx_pb_flush_tex(void * tex, void * pb);
extern void gfx_dealloc(void * ptr);
extern int32_t gfx_event_text_len(void);
extern void gfx_pb_render_ham(void * ren, void * tex, void * pb, int32_t mode);
extern int32_t gfx_pb_rgba_get32(void * pb, int32_t offset);
extern void gfx_draw_rect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
extern void gfx_destroy_texture(void * tex);
extern void gfx_set_title(void * win, void * title);
extern int32_t gfx_pb_pal_packed(void * pb, int32_t idx);
extern int32_t gfx_has_clipboard(void);
extern void gfx_pb_render(void * ren, void * tex, void * pb);
extern int32_t gfx_wait_event_timeout(int32_t ms);
extern int32_t gfx_event_scancode(void);
extern int32_t gfx_mouse_state(int32_t *x, int32_t *y);
extern void gfx_pb_render_alpha(void * ren, void * tex, void * pb, int32_t alpha);
extern void gfx_show_win(void * win);
extern void gfx_present(void * ren);
extern int32_t gfx_event_win_event(void);
extern int32_t gfx_pb_total(void * pb);
extern void gfx_maximize_win(void * win);
extern int32_t gfx_output_width(void * ren);
extern void gfx_stop_text(void);
extern void gfx_reset_viewport(void * ren);
extern int32_t gfx_get_win_id(void * win);
extern int32_t gfx_font_descent(void * font);
extern void gfx_pb_free_save(void * region);
extern int32_t gfx_get_clip_x(void * ren);
extern int32_t gfx_screen_width(void);
extern int32_t gfx_output_height(void * ren);
extern void * gfx_pb_save(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
extern void gfx_close_font(void * font);
extern void gfx_draw_text(void * ren, void * font, void * text, int32_t x, int32_t y, int32_t r, int32_t g, int32_t b, int32_t a);
extern int32_t gfx_font_ascent(void * font);
extern void * gfx_open_font(void * path, int32_t size);
extern void gfx_destroy_renderer(void * ren);
extern void gfx_quit(void);
extern int32_t gfx_buf_get(void * buf, int32_t offset);
extern void gfx_set_tex_color(void * tex, int32_t r, int32_t g, int32_t b);
extern void * gfx_open_font_physical(void * path, int32_t size);
extern int32_t gfx_get_win_height(void * win);
extern void gfx_clear_clip(void * ren);
extern void gfx_font_style(void * font, int32_t style);
extern void gfx_draw_texture_ex(void * ren, void * tex, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, int32_t dw, int32_t dh);
extern int32_t gfx_pb_save_png(void * pb, void * path);
extern int32_t gfx_wait_event(void);
extern void gfx_restore_win(void * win);
extern int32_t gfx_text_height(void * font, void * text);
extern void gfx_draw_point(void * ren, int32_t x, int32_t y);
extern int32_t gfx_get_win_width(void * win);
extern int32_t gfx_event_win_id(void);
extern void gfx_fill_rect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
extern void * gfx_create_renderer(void * win, int32_t flags);
extern void gfx_get_clipboard(void * buf, int32_t buflen);
extern void gfx_draw_texture(void * ren, void * tex, int32_t x, int32_t y);
extern int32_t gfx_ticks(void);
extern int32_t gfx_event_wheel_y(void);
extern int32_t gfx_event_mouse_x(void);
extern void gfx_pb_free(void * pb);
extern void gfx_pb_set_pal_alpha(void * pb, int32_t idx, int32_t r, int32_t g, int32_t b, int32_t a);
extern void gfx_pb_copy_pixels(void * src, void * dst);
extern void * gfx_load_bmp(void * ren, void * path);
extern int32_t gfx_font_line_skip(void * font);
extern void gfx_font_set_hinting(void * font, int32_t hint);
extern void gfx_pb_mark_dirty(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
extern void gfx_destroy_window(void * win);
extern int32_t gfx_get_clip_w(void * ren);
extern void * gfx_pb_load_png(void * path, int32_t ncolors);
extern void gfx_set_win_min_size(void * win, int32_t w, int32_t h);
extern int32_t gfx_poll_event(void);
extern void gfx_set_target(void * ren, void * tex);
extern int32_t gfx_display_count(void);
extern void * gfx_create_window(void * title, int32_t w, int32_t h, int32_t flags);
extern void gfx_reset_target(void * ren);
extern void gfx_set_win_size(void * win, int32_t w, int32_t h);
extern void gfx_show_cursor(int32_t show);
extern void gfx_set_fullscreen(void * win, int32_t mode);
extern void gfx_set_viewport(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
extern void gfx_set_cursor(int32_t cursorType);
extern void gfx_event_text(void * buf, int32_t buflen);
extern int32_t gfx_pb_get(void * pb, int32_t x, int32_t y);
extern int32_t gfx_pb_save_w(void * region);
extern int32_t gfx_event_key_repeat(void);
extern void * gfx_pb_load_png_pal(void * path, void * palPb, int32_t ncolors);
extern void gfx_draw_texture_rot(void * ren, void * tex, int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t angleDeg, int32_t flip);
extern void gfx_set_tex_blend(void * tex, int32_t mode);
extern int32_t gfx_dpi_scale(void);
extern int32_t gfx_pb_width(void * pb);
extern void gfx_pb_pal_to_screen(void * pb);
extern void gfx_pb_clear(void * pb, int32_t idx);
extern void gfx_draw_line(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2);
extern int32_t gfx_get_clip_h(void * ren);
extern void gfx_pb_restore(void * pb, void * region, int32_t x, int32_t y);
extern void gfx_pb_stamp_text(void * pb, void * ren, void * font, void * text, int32_t x, int32_t y, int32_t idx);
typedef void * ADDRESS;


/* Foreign C bindings: Sys */
extern void m2sys_exit(int32_t code);
extern int32_t m2sys_is_dir(void * path);
extern int32_t m2sys_tar_extract(void * archivePath, void * destDir);
extern int64_t m2sys_unix_time(void);
extern int32_t m2sys_fopen(void * path, void * mode);
extern void m2sys_home_dir(void * outBuf, int32_t outSize);
extern int32_t m2sys_read_byte(int32_t fd);
extern int32_t m2sys_str_starts_with(void * s, void * prefix);
extern int32_t m2sys_write_byte(int32_t fd);
extern int32_t m2sys_str_eq(void * a, void * b);
extern int32_t m2sys_str_append(void * dst, int32_t dstSize, void * src);
extern int32_t m2sys_is_symlink(void * path);
extern void m2sys_basename(void * path, void * outBuf, int32_t outSize);
extern int32_t m2sys_fclose(int32_t handle);
extern int32_t m2sys_strlen(void * s);
extern int32_t m2sys_tar_create_ex(void * archivePath, void * baseDir, void * excludePattern);
extern int32_t m2sys_mkdir_p(void * path);
extern int32_t m2sys_copy_file(void * src, void * dst);
extern int64_t m2sys_file_size(void * path);
extern int32_t m2sys_list_dir(void * dir, void * buf, int32_t bufSize);
extern int32_t m2sys_exec_output(void * cmdline, void * outBuf, int32_t outSize);
extern void m2sys_getenv(void * name, void * outBuf, int32_t outSize);
extern int32_t m2sys_tar_create(void * archivePath, void * baseDir);
extern int32_t m2sys_exec(void * cmdline);
extern void m2sys_getcwd(void * outBuf, int32_t outSize);
extern int64_t m2sys_thread_id(void);
extern int32_t m2sys_rename(void * oldPath, void * newPath);
extern int32_t m2sys_fseek(int32_t handle, int64_t offset, int32_t whence);
extern int32_t m2sys_fread_line(int32_t handle, void * buf, int32_t bufSize);
extern int32_t m2sys_flock(int32_t handle, int32_t exclusive);
extern int32_t m2sys_rmdir_r(void * path);
extern int32_t m2sys_chdir(void * path);
extern int32_t m2sys_fwrite_bytes(int32_t handle, void * data, int32_t len);
extern int32_t m2sys_set_nonblock(int32_t fd);
extern void m2sys_format_time(void * buf, int32_t bufSize);
extern int32_t m2sys_file_exists(void * path);
extern int32_t m2sys_str_contains_ci(void * haystack, void * needle);
extern int32_t m2sys_fwrite_str(int32_t handle, void * data);
extern int32_t m2sys_remove_file(void * path);
extern int64_t m2sys_file_mtime(void * path);
extern int32_t m2sys_pipe(void * fds);
extern int32_t m2sys_fread_bytes(int32_t handle, void * buf, int32_t maxLen);
extern int32_t m2sys_sha256_file(void * path, void * hexOut);
extern void m2sys_join_path(void * a, void * b, void * outBuf, int32_t outSize);
extern void m2sys_sha256_str(void * data, int32_t len, void * hexOut);
extern int32_t m2sys_funlock(int32_t handle);
extern void m2sys_dirname(void * path, void * outBuf, int32_t outSize);
typedef void * ADDRESS;


/* Definition-only module Font */
static const int32_t Font_HINT_MONO = 2;
static const int32_t Font_STYLE_UNDERLINE = 4;
static const int32_t Font_HINT_NORMAL = 0;
static const int32_t Font_STYLE_ITALIC = 2;
static const int32_t Font_STYLE_STRIKETHROUGH = 8;
static const int32_t Font_HINT_NONE = 3;
static const int32_t Font_STYLE_BOLD = 1;
static const int32_t Font_STYLE_NORMAL = 0;
static const int32_t Font_HINT_LIGHT = 1;
typedef void * Font_FontHandle;

typedef void * Font_Renderer;


/* Imported Module Strings */

static void Strings_Assign(char *s, uint32_t s_high, char *dst, uint32_t dst_high);
static void Strings_Insert(char *sub, uint32_t sub_high, char *dst, uint32_t dst_high, uint32_t pos);
static void Strings_Delete(char *s, uint32_t s_high, uint32_t pos, uint32_t len);
static uint32_t Strings_Pos(char *sub, uint32_t sub_high, char *s, uint32_t s_high);
static uint32_t Strings_Length(char *s, uint32_t s_high);
static void Strings_Copy(char *src, uint32_t src_high, uint32_t pos, uint32_t len, char *dst, uint32_t dst_high);
static void Strings_Concat(char *s1, uint32_t s1_high, char *s2, uint32_t s2_high, char *dst, uint32_t dst_high);
static int32_t Strings_CompareStr(char *s1, uint32_t s1_high, char *s2, uint32_t s2_high);
static void Strings_CAPS(char *s, uint32_t s_high);

static void Strings_Assign(char *s, uint32_t s_high, char *dst, uint32_t dst_high) {
    (void)s;
    (void)s_high;
    (void)dst;
    (void)dst_high;
    uint32_t cap;
    uint32_t slen;
#line 12 "/Users/aoesterer/.mx/lib/m2stdlib/src/Strings.mod"
    cap = (dst_high + 1);
#line 13
    slen = strlen(((void *)(s)));
    if (slen >= cap) goto L2; else goto L1;
  L1:;
#line 15
    memcpy(((void *)(dst)), ((void *)(s)), slen);
#line 16
    dst[slen] = ((char)(0));
    return;
  L2:;
#line 14
    slen = (cap - 1);
    goto L1;
}

static void Strings_Insert(char *sub, uint32_t sub_high, char *dst, uint32_t dst_high, uint32_t pos) {
    (void)sub;
    (void)sub_high;
    (void)dst;
    (void)dst_high;
    (void)pos;
    uint32_t cap;
    uint32_t slen;
    uint32_t dlen;
    uint32_t newLen;
    uint32_t tailDst;
    uint32_t tailKeep;
    uint32_t subCopy;
#line 23
    cap = (dst_high + 1);
#line 24
    slen = strlen(((void *)(sub)));
#line 25
    dlen = strlen(((void *)(dst)));
    if (pos > dlen) goto L2; else goto L1;
  L1:;
#line 27
    newLen = (dlen + slen);
    if (newLen >= cap) goto L4; else goto L3;
  L2:;
#line 26
    pos = dlen;
    goto L1;
  L3:;
#line 29
    tailDst = (pos + slen);
    if (tailDst < newLen) goto L6; else goto L7;
  L4:;
#line 28
    newLen = (cap - 1);
    goto L3;
  L5:;
    if (tailKeep > 0) goto L9; else goto L8;
  L6:;
#line 31
    tailKeep = (newLen - tailDst);
    goto L5;
  L7:;
#line 33
    tailKeep = 0;
    goto L5;
  L8:;
#line 38
    subCopy = slen;
    if ((pos + subCopy) > newLen) goto L11; else goto L10;
  L9:;
#line 36
    memmove(((void *)&(dst[tailDst])), ((void *)&(dst[pos])), tailKeep);
    goto L8;
  L10:;
    if (subCopy > 0) goto L13; else goto L12;
  L11:;
#line 39
    subCopy = (newLen - pos);
    goto L10;
  L12:;
#line 43
    dst[newLen] = ((char)(0));
    return;
  L13:;
#line 41
    memcpy(((void *)&(dst[pos])), ((void *)(sub)), subCopy);
    goto L12;
}

static void Strings_Delete(char *s, uint32_t s_high, uint32_t pos, uint32_t len) {
    (void)s;
    (void)s_high;
    (void)pos;
    (void)len;
    uint32_t slen;
#line 50
    slen = strlen(((void *)(s)));
    if (pos >= slen) goto L2; else goto L1;
  L1:;
    if ((pos + len) > slen) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 53
    memmove(((void *)&(s[pos])), ((void *)&(s[(pos + len)])), (((slen - pos) - len) + 1));
    return;
  L4:;
#line 52
    len = (slen - pos);
    goto L3;
}

static uint32_t Strings_Pos(char *sub, uint32_t sub_high, char *s, uint32_t s_high) {
    (void)sub;
    (void)sub_high;
    (void)s;
    (void)s_high;
    void * p;
    void * base;
#line 60
    base = ((void *)(s));
#line 61
    p = strstr(base, ((void *)(sub)));
    if (p == NULL) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return m2_max(CARDINAL);
  L3:;
    return ((uint32_t)((p - base)));
}

static uint32_t Strings_Length(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
    return strlen(((void *)(s)));
}

static void Strings_Copy(char *src, uint32_t src_high, uint32_t pos, uint32_t len, char *dst, uint32_t dst_high) {
    (void)src;
    (void)src_high;
    (void)pos;
    (void)len;
    (void)dst;
    (void)dst_high;
    uint32_t cap;
    uint32_t slen;
#line 79
    cap = (dst_high + 1);
#line 80
    slen = strlen(((void *)(src)));
    if (pos >= slen) goto L2; else goto L1;
  L1:;
    if ((pos + len) > slen) goto L4; else goto L3;
  L2:;
#line 82
    dst[0] = ((char)(0));
    return;
  L3:;
    if (len >= cap) goto L6; else goto L5;
  L4:;
#line 85
    len = (slen - pos);
    goto L3;
  L5:;
#line 87
    memcpy(((void *)(dst)), ((void *)&(src[pos])), len);
#line 88
    dst[len] = ((char)(0));
    return;
  L6:;
#line 86
    len = (cap - 1);
    goto L5;
}

static void Strings_Concat(char *s1, uint32_t s1_high, char *s2, uint32_t s2_high, char *dst, uint32_t dst_high) {
    (void)s1;
    (void)s1_high;
    (void)s2;
    (void)s2_high;
    (void)dst;
    (void)dst_high;
    uint32_t cap;
    uint32_t len1;
    uint32_t len2;
    uint32_t rem;
#line 96
    cap = (dst_high + 1);
#line 97
    len1 = strlen(((void *)(s1)));
#line 98
    len2 = strlen(((void *)(s2)));
    if (len1 >= cap) goto L2; else goto L1;
  L1:;
#line 100
    memcpy(((void *)(dst)), ((void *)(s1)), len1);
#line 101
    rem = ((cap - 1) - len1);
    if (len2 > rem) goto L4; else goto L3;
  L2:;
#line 99
    len1 = (cap - 1);
    goto L1;
  L3:;
#line 103
    memcpy(((void *)&(dst[len1])), ((void *)(s2)), len2);
#line 104
    dst[(len1 + len2)] = ((char)(0));
    return;
  L4:;
#line 102
    len2 = rem;
    goto L3;
}

static int32_t Strings_CompareStr(char *s1, uint32_t s1_high, char *s2, uint32_t s2_high) {
    (void)s1;
    (void)s1_high;
    (void)s2;
    (void)s2_high;
    return strcmp(((void *)(s1)), ((void *)(s2)));
}

static void Strings_CAPS(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
    uint32_t i;
#line 116
    i = 0;
    goto L1;
  L1:;
    if (i <= s_high) goto L4; else goto L3;
  L2:;
#line 118
    s[i] = ((char)(toupper(((int32_t)((unsigned char)(s[i]))))));
#line 119
    (i++);
    goto L1;
  L3:;
    return;
  L4:;
    if (s[i] != ((char)(0))) goto L2; else goto L3;
}

/* Imported Module InOut */

static void InOut_WriteString(char *s, uint32_t s_high);
static void InOut_WriteLn(void);
static void InOut_Write(char ch);
static void InOut_WriteChar(char ch);
static void InOut_WriteUnsigned(uint32_t val, uint32_t w);
static void InOut_WriteIntHelper(int32_t n, uint32_t w, int neg);
static void InOut_WriteInt(int32_t n, uint32_t w);
static void InOut_WriteCard(uint32_t n, uint32_t w);
static void InOut_WriteHex(uint32_t n, uint32_t w);
static void InOut_WriteOct(uint32_t n, uint32_t w);
static void InOut_Read(char *ch);
static void InOut_ReadChar(char *ch);
static void InOut_ReadString(char *s, uint32_t s_high);
static void InOut_ReadInt(int32_t *n);
static void InOut_ReadCard(uint32_t *n);
static void InOut_OpenInput(char *ext, uint32_t ext_high);
static void InOut_OpenOutput(char *ext, uint32_t ext_high);
static void InOut_CloseInput(void);
static void InOut_CloseOutput(void);

int InOut_Done;
int InOut_Done;
static void InOut_WriteString(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
    uint32_t i;
#line 12 "/Users/aoesterer/.mx/lib/m2stdlib/src/InOut.mod"
    i = 0;
    goto L1;
  L1:;
    if (i <= s_high) goto L4; else goto L3;
  L2:;
#line 14
    putchar(((int32_t)((unsigned char)(s[i]))));
#line 15
    (i++);
    goto L1;
  L3:;
    return;
  L4:;
    if (s[i] != ((char)(0))) goto L2; else goto L3;
}

static void InOut_WriteLn(void) {
#line 21
    putchar(10);
    return;
}

static void InOut_Write(char ch) {
    (void)ch;
#line 26
    putchar(((int32_t)((unsigned char)(ch))));
    return;
}

static void InOut_WriteChar(char ch) {
    (void)ch;
#line 31
    putchar(((int32_t)((unsigned char)(ch))));
    return;
}

static void InOut_WriteUnsigned(uint32_t val, uint32_t w) {
    (void)val;
    (void)w;
    char buf[11 + 1];
    uint32_t i;
    uint32_t len;
    uint32_t v;
    if (val == 0) goto L2; else goto L3;
  L1:;
    goto L7;
  L2:;
#line 43
    buf[0] = '0';
#line 44
    len = 1;
    goto L1;
  L3:;
#line 46
    len = 0;
#line 47
    v = val;
    goto L4;
  L4:;
    if (v > 0) goto L5; else goto L6;
  L5:;
#line 49
    buf[len] = ((char)((((int32_t)((unsigned char)('0'))) + (v % 10))));
#line 50
    v = (v / 10);
#line 51
    (len++);
    goto L4;
  L6:;
    goto L1;
  L7:;
    if (w > len) goto L8; else goto L9;
  L8:;
#line 56
    putchar(((int32_t)((unsigned char)(' '))));
#line 57
    (w--);
    goto L7;
  L9:;
#line 60
    i = len;
    goto L10;
  L10:;
    if (i > 0) goto L11; else goto L12;
  L11:;
#line 62
    (i--);
#line 63
    putchar(((int32_t)((unsigned char)(buf[i]))));
    goto L10;
  L12:;
    return;
}

static void InOut_WriteIntHelper(int32_t n, uint32_t w, int neg) {
    (void)n;
    (void)w;
    (void)neg;
    char buf[11 + 1];
    uint32_t len;
    uint32_t i;
    int32_t v;
    uint32_t lastDigit;
    if (n == 0) goto L2; else goto L3;
  L1:;
    if (neg) goto L19; else goto L20;
  L2:;
#line 78
    buf[0] = '0';
    len = 1;
    goto L1;
  L3:;
    if (n == m2_min(INTEGER)) goto L4; else goto L5;
  L4:;
#line 84
    lastDigit = ((uint32_t)(m2_mod(n, 10)));
    if (lastDigit > 0) goto L7; else goto L8;
  L5:;
#line 100
    len = 0;
    if (n < 0) goto L13; else goto L14;
  L6:;
#line 92
    buf[0] = ((char)((((int32_t)((unsigned char)('0'))) + lastDigit)));
#line 93
    len = 1;
    goto L9;
  L7:;
#line 86
    lastDigit = (10 - lastDigit);
#line 87
    v = ((-m2_div(n, 10)) - 1);
    goto L6;
  L8:;
#line 89
    lastDigit = 0;
#line 90
    v = (-m2_div(n, 10));
    goto L6;
  L9:;
    if (v > 0) goto L10; else goto L11;
  L10:;
#line 95
    buf[len] = ((char)((((int32_t)((unsigned char)('0'))) + ((uint32_t)(m2_mod(v, 10))))));
#line 96
    v = m2_div(v, 10);
#line 97
    (len++);
    goto L9;
  L11:;
    goto L1;
  L12:;
    goto L15;
  L13:;
#line 101
    v = (-n);
    goto L12;
  L14:;
    v = n;
    goto L12;
  L15:;
    if (v > 0) goto L16; else goto L17;
  L16:;
#line 103
    buf[len] = ((char)((((int32_t)((unsigned char)('0'))) + ((uint32_t)(m2_mod(v, 10))))));
#line 104
    v = m2_div(v, 10);
#line 105
    (len++);
    goto L15;
  L17:;
    goto L1;
  L18:;
#line 115
    i = len;
    goto L27;
  L19:;
    goto L21;
  L20:;
    goto L24;
  L21:;
    if (w > (len + 1)) goto L22; else goto L23;
  L22:;
#line 110
    putchar(((int32_t)((unsigned char)(' '))));
    (w--);
    goto L21;
  L23:;
#line 111
    putchar(((int32_t)((unsigned char)('-'))));
    goto L18;
  L24:;
    if (w > len) goto L25; else goto L26;
  L25:;
#line 113
    putchar(((int32_t)((unsigned char)(' '))));
    (w--);
    goto L24;
  L26:;
    goto L18;
  L27:;
    if (i > 0) goto L28; else goto L29;
  L28:;
#line 116
    (i--);
    putchar(((int32_t)((unsigned char)(buf[i]))));
    goto L27;
  L29:;
    return;
}

static void InOut_WriteInt(int32_t n, uint32_t w) {
    (void)n;
    (void)w;
    if (n < 0) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 122
    InOut_WriteIntHelper(n, w, 1);
    goto L1;
  L3:;
#line 124
    InOut_WriteIntHelper(n, w, 0);
    goto L1;
}

static void InOut_WriteCard(uint32_t n, uint32_t w) {
    (void)n;
    (void)w;
#line 130
    InOut_WriteUnsigned(n, w);
    return;
}

static void InOut_WriteHex(uint32_t n, uint32_t w) {
    (void)n;
    (void)w;
    char buf[7 + 1];
    uint32_t i;
    uint32_t len;
    uint32_t d;
    if (n == 0) goto L2; else goto L3;
  L1:;
    goto L10;
  L2:;
#line 140
    buf[0] = '0';
    len = 1;
    goto L1;
  L3:;
#line 142
    len = 0;
    goto L4;
  L4:;
    if (n > 0) goto L5; else goto L6;
  L5:;
#line 144
    d = (n % 16);
    if (d < 10) goto L8; else goto L9;
  L6:;
    goto L1;
  L7:;
#line 150
    n = (n / 16);
#line 151
    (len++);
    goto L4;
  L8:;
#line 146
    buf[len] = ((char)((((int32_t)((unsigned char)('0'))) + d)));
    goto L7;
  L9:;
#line 148
    buf[len] = ((char)(((((int32_t)((unsigned char)('A'))) + d) - 10)));
    goto L7;
  L10:;
    if (w > len) goto L11; else goto L12;
  L11:;
#line 154
    putchar(((int32_t)((unsigned char)(' '))));
    (w--);
    goto L10;
  L12:;
#line 155
    i = len;
    goto L13;
  L13:;
    if (i > 0) goto L14; else goto L15;
  L14:;
#line 156
    (i--);
    putchar(((int32_t)((unsigned char)(buf[i]))));
    goto L13;
  L15:;
    return;
}

static void InOut_WriteOct(uint32_t n, uint32_t w) {
    (void)n;
    (void)w;
    char buf[11 + 1];
    uint32_t i;
    uint32_t len;
    if (n == 0) goto L2; else goto L3;
  L1:;
    goto L7;
  L2:;
#line 165
    buf[0] = '0';
    len = 1;
    goto L1;
  L3:;
#line 167
    len = 0;
    goto L4;
  L4:;
    if (n > 0) goto L5; else goto L6;
  L5:;
#line 169
    buf[len] = ((char)((((int32_t)((unsigned char)('0'))) + (n % 8))));
#line 170
    n = (n / 8);
#line 171
    (len++);
    goto L4;
  L6:;
    goto L1;
  L7:;
    if (w > len) goto L8; else goto L9;
  L8:;
#line 174
    putchar(((int32_t)((unsigned char)(' '))));
    (w--);
    goto L7;
  L9:;
#line 175
    i = len;
    goto L10;
  L10:;
    if (i > 0) goto L11; else goto L12;
  L11:;
#line 176
    (i--);
    putchar(((int32_t)((unsigned char)(buf[i]))));
    goto L10;
  L12:;
    return;
}

static void InOut_Read(char *ch) {
    (void)ch;
    int32_t c;
#line 184
    c = getchar();
    if (c == (-1)) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 186
    (*ch) = ((char)(0));
#line 187
    InOut_Done = 0;
    goto L1;
  L3:;
#line 189
    (*ch) = ((char)(c));
#line 190
    InOut_Done = 1;
    goto L1;
}

static void InOut_ReadChar(char *ch) {
    (void)ch;
#line 196
    m2_Read(&(*ch));
    return;
}

static void InOut_ReadString(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
    int32_t c;
    uint32_t i;
#line 203
    c = getchar();
    goto L1;
  L1:;
    if (c == ((int32_t)((unsigned char)(' ')))) goto L2; else goto L6;
  L2:;
#line 205
    c = getchar();
    goto L1;
  L3:;
    if (c == (-1)) goto L8; else goto L7;
  L4:;
    if (c == 13) goto L2; else goto L3;
  L5:;
    if (c == 10) goto L2; else goto L4;
  L6:;
    if (c == 9) goto L2; else goto L5;
  L7:;
#line 212
    i = 0;
    goto L9;
  L8:;
#line 208
    s[0] = ((char)(0));
#line 209
    InOut_Done = 0;
    return;
  L9:;
    if (c != (-1)) goto L16; else goto L11;
  L10:;
#line 215
    s[i] = ((char)(c));
#line 216
    (i++);
#line 217
    c = getchar();
    goto L9;
  L11:;
    if (i <= s_high) goto L18; else goto L17;
  L12:;
    if (i <= s_high) goto L10; else goto L11;
  L13:;
    if (c != 13) goto L12; else goto L11;
  L14:;
    if (c != 10) goto L13; else goto L11;
  L15:;
    if (c != 9) goto L14; else goto L11;
  L16:;
    if (c != ((int32_t)((unsigned char)(' ')))) goto L15; else goto L11;
  L17:;
#line 220
    InOut_Done = 1;
    return;
  L18:;
#line 219
    s[i] = ((char)(0));
    goto L17;
}

static void InOut_ReadInt(int32_t *n) {
    (void)n;
    int32_t c;
    int neg;
    int32_t val;
#line 229
    neg = 0;
#line 231
    c = getchar();
    goto L1;
  L1:;
    if (c == ((int32_t)((unsigned char)(' ')))) goto L2; else goto L6;
  L2:;
#line 233
    c = getchar();
    goto L1;
  L3:;
    if (c == (-1)) goto L8; else goto L7;
  L4:;
    if (c == 13) goto L2; else goto L3;
  L5:;
    if (c == 10) goto L2; else goto L4;
  L6:;
    if (c == 9) goto L2; else goto L5;
  L7:;
    if (c == ((int32_t)((unsigned char)('-')))) goto L10; else goto L11;
  L8:;
#line 235
    InOut_Done = 0;
    (*n) = 0;
    return;
  L9:;
    if (c < ((int32_t)((unsigned char)('0')))) goto L14; else goto L15;
  L10:;
#line 236
    neg = 1;
    c = getchar();
    goto L9;
  L11:;
    if (c == ((int32_t)((unsigned char)('+')))) goto L12; else goto L9;
  L12:;
#line 237
    c = getchar();
    goto L9;
  L13:;
#line 242
    val = 0;
    goto L16;
  L14:;
#line 240
    InOut_Done = 0;
    (*n) = 0;
    return;
  L15:;
    if (c > ((int32_t)((unsigned char)('9')))) goto L14; else goto L13;
  L16:;
    if (c >= ((int32_t)((unsigned char)('0')))) goto L19; else goto L18;
  L17:;
#line 244
    val = ((val * 10) + (c - ((int32_t)((unsigned char)('0')))));
#line 245
    c = getchar();
    goto L16;
  L18:;
    if (neg) goto L21; else goto L22;
  L19:;
    if (c <= ((int32_t)((unsigned char)('9')))) goto L17; else goto L18;
  L20:;
#line 248
    InOut_Done = 1;
    return;
  L21:;
#line 247
    (*n) = (-val);
    goto L20;
  L22:;
    (*n) = val;
    goto L20;
}

static void InOut_ReadCard(uint32_t *n) {
    (void)n;
    int32_t c;
    uint32_t val;
#line 256
    c = getchar();
    goto L1;
  L1:;
    if (c == ((int32_t)((unsigned char)(' ')))) goto L2; else goto L6;
  L2:;
#line 258
    c = getchar();
    goto L1;
  L3:;
    if (c == (-1)) goto L8; else goto L10;
  L4:;
    if (c == 13) goto L2; else goto L3;
  L5:;
    if (c == 10) goto L2; else goto L4;
  L6:;
    if (c == 9) goto L2; else goto L5;
  L7:;
#line 263
    val = 0;
    goto L11;
  L8:;
#line 261
    InOut_Done = 0;
    (*n) = 0;
    return;
  L9:;
    if (c > ((int32_t)((unsigned char)('9')))) goto L8; else goto L7;
  L10:;
    if (c < ((int32_t)((unsigned char)('0')))) goto L8; else goto L9;
  L11:;
    if (c >= ((int32_t)((unsigned char)('0')))) goto L14; else goto L13;
  L12:;
#line 265
    val = ((val * 10) + ((uint32_t)((c - ((int32_t)((unsigned char)('0')))))));
#line 266
    c = getchar();
    goto L11;
  L13:;
#line 268
    (*n) = val;
#line 269
    InOut_Done = 1;
    return;
  L14:;
    if (c <= ((int32_t)((unsigned char)('9')))) goto L12; else goto L13;
}

static void InOut_OpenInput(char *ext, uint32_t ext_high) {
    (void)ext;
    (void)ext_high;
#line 276
    InOut_Done = 0;
    return;
}

static void InOut_OpenOutput(char *ext, uint32_t ext_high) {
    (void)ext;
    (void)ext_high;
#line 281
    InOut_Done = 0;
    return;
}

static void InOut_CloseInput(void) {
    return;
}

static void InOut_CloseOutput(void) {
    return;
}

static void InOut_init(void) {
#line 293
    InOut_Done = 1;
    return;
}

/* Imported Module Gfx */

static const int32_t Gfx_FULLSCREEN_DESKTOP = 2;
static const int32_t Gfx_FULLSCREEN_OFF = 0;
static const int32_t Gfx_FULLSCREEN_TRUE = 1;
static const int32_t Gfx_WIN_CENTERED = 1;
static const int32_t Gfx_WIN_HIGHDPI = 64;
static const int32_t Gfx_RENDER_ACCELERATED = 1;
static const int32_t Gfx_CURSOR_CROSSHAIR = 3;
static const int32_t Gfx_CURSOR_SIZE_NS = 8;
static const int32_t Gfx_RENDER_SOFTWARE = 4;
static const int32_t Gfx_CURSOR_WAIT = 2;
static const int32_t Gfx_CURSOR_SIZE_ALL = 9;
static const int32_t Gfx_CURSOR_SIZE_WE = 7;
static const int32_t Gfx_CURSOR_SIZE_NWSE = 5;
static const int32_t Gfx_CURSOR_HAND = 4;
static const int32_t Gfx_CURSOR_NO = 10;
static const int32_t Gfx_CURSOR_ARROW = 0;
static const int32_t Gfx_CURSOR_IBEAM = 1;
static const int32_t Gfx_WIN_RESIZABLE = 2;
static const int32_t Gfx_WIN_BORDERLESS = 4;
static const int32_t Gfx_CURSOR_SIZE_NESW = 6;
static const int32_t Gfx_WIN_MAXIMIZED = 32;
static const int32_t Gfx_WIN_HIDDEN = 16;
static const int32_t Gfx_WIN_FULLSCREEN = 8;
static const int32_t Gfx_RENDER_VSYNC = 2;
typedef void * Gfx_Window;

typedef void * Gfx_Renderer;

static int Gfx_Init(void);
static int Gfx_InitFont(void);
static void Gfx_Quit(void);
static void Gfx_QuitFont(void);
static void * Gfx_CreateWindow(char *title, uint32_t title_high, int32_t w, int32_t h, int32_t flags);
static void Gfx_DestroyWindow(void * win);
static void Gfx_SetTitle(void * win, char *title, uint32_t title_high);
static void Gfx_SetWindowSize(void * win, int32_t w, int32_t h);
static int32_t Gfx_GetWindowWidth(void * win);
static int32_t Gfx_GetWindowHeight(void * win);
static void Gfx_SetWindowPos(void * win, int32_t x, int32_t y);
static void Gfx_SetFullscreen(void * win, int32_t mode);
static void Gfx_ShowWindow(void * win);
static void Gfx_HideWindow(void * win);
static void Gfx_RaiseWindow(void * win);
static void Gfx_MinimizeWindow(void * win);
static void Gfx_MaximizeWindow(void * win);
static void Gfx_RestoreWindow(void * win);
static int32_t Gfx_GetWindowID(void * win);
static void Gfx_SetWindowMinSize(void * win, int32_t w, int32_t h);
static void Gfx_SetWindowMaxSize(void * win, int32_t w, int32_t h);
static void * Gfx_CreateRenderer(void * win, int32_t flags);
static void Gfx_UpdateLogicalSize(void * ren, void * win);
static void Gfx_DestroyRenderer(void * ren);
static void Gfx_Present(void * ren);
static int32_t Gfx_OutputWidth(void * ren);
static int32_t Gfx_OutputHeight(void * ren);
static int32_t Gfx_ScreenWidth(void);
static int32_t Gfx_ScreenHeight(void);
static int32_t Gfx_DisplayCount(void);
static void Gfx_SetClipboard(char *text, uint32_t text_high);
static void Gfx_GetClipboard(char *text, uint32_t text_high);
static int Gfx_HasClipboard(void);
static int32_t Gfx_Ticks(void);
static void Gfx_Delay(int32_t ms);
static void Gfx_SetCursor(int32_t cursorType);
static void Gfx_ShowCursor(int show);

static int Gfx_Init(void) {
    return gfx_init() != 0;
}

static int Gfx_InitFont(void) {
    return gfx_ttf_init() != 0;
}

static void Gfx_Quit(void) {
#line 31 "/Users/aoesterer/.mx/lib/m2gfx/src/Gfx.mod"
    gfx_quit();
    return;
}

static void Gfx_QuitFont(void) {
#line 36
    gfx_ttf_quit();
    return;
}

static void * Gfx_CreateWindow(char *title, uint32_t title_high, int32_t w, int32_t h, int32_t flags) {
    (void)title;
    (void)title_high;
    (void)w;
    (void)h;
    (void)flags;
    return gfx_create_window(((void *)(title)), w, h, flags);
}

static void Gfx_DestroyWindow(void * win) {
    (void)win;
#line 47
    gfx_destroy_window(win);
    return;
}

static void Gfx_SetTitle(void * win, char *title, uint32_t title_high) {
    (void)win;
    (void)title;
    (void)title_high;
#line 52
    gfx_set_title(win, ((void *)(title)));
    return;
}

static void Gfx_SetWindowSize(void * win, int32_t w, int32_t h) {
    (void)win;
    (void)w;
    (void)h;
#line 57
    gfx_set_win_size(win, w, h);
    return;
}

static int32_t Gfx_GetWindowWidth(void * win) {
    (void)win;
    return gfx_get_win_width(win);
}

static int32_t Gfx_GetWindowHeight(void * win) {
    (void)win;
    return gfx_get_win_height(win);
}

static void Gfx_SetWindowPos(void * win, int32_t x, int32_t y) {
    (void)win;
    (void)x;
    (void)y;
#line 72
    gfx_set_win_pos(win, x, y);
    return;
}

static void Gfx_SetFullscreen(void * win, int32_t mode) {
    (void)win;
    (void)mode;
#line 77
    gfx_set_fullscreen(win, mode);
    return;
}

static void Gfx_ShowWindow(void * win) {
    (void)win;
#line 82
    gfx_show_win(win);
    return;
}

static void Gfx_HideWindow(void * win) {
    (void)win;
#line 87
    gfx_hide_win(win);
    return;
}

static void Gfx_RaiseWindow(void * win) {
    (void)win;
#line 92
    gfx_raise_win(win);
    return;
}

static void Gfx_MinimizeWindow(void * win) {
    (void)win;
#line 97
    gfx_minimize_win(win);
    return;
}

static void Gfx_MaximizeWindow(void * win) {
    (void)win;
#line 102
    gfx_maximize_win(win);
    return;
}

static void Gfx_RestoreWindow(void * win) {
    (void)win;
#line 107
    gfx_restore_win(win);
    return;
}

static int32_t Gfx_GetWindowID(void * win) {
    (void)win;
    return gfx_get_win_id(win);
}

static void Gfx_SetWindowMinSize(void * win, int32_t w, int32_t h) {
    (void)win;
    (void)w;
    (void)h;
#line 117
    gfx_set_win_min_size(win, w, h);
    return;
}

static void Gfx_SetWindowMaxSize(void * win, int32_t w, int32_t h) {
    (void)win;
    (void)w;
    (void)h;
#line 122
    gfx_set_win_max_size(win, w, h);
    return;
}

static void * Gfx_CreateRenderer(void * win, int32_t flags) {
    (void)win;
    (void)flags;
    return gfx_create_renderer(win, flags);
}

static void Gfx_UpdateLogicalSize(void * ren, void * win) {
    (void)ren;
    (void)win;
#line 132
    gfx_update_logical_size(ren, win);
    return;
}

static void Gfx_DestroyRenderer(void * ren) {
    (void)ren;
#line 137
    gfx_destroy_renderer(ren);
    return;
}

static void Gfx_Present(void * ren) {
    (void)ren;
#line 142
    gfx_present(ren);
    return;
}

static int32_t Gfx_OutputWidth(void * ren) {
    (void)ren;
    return gfx_output_width(ren);
}

static int32_t Gfx_OutputHeight(void * ren) {
    (void)ren;
    return gfx_output_height(ren);
}

static int32_t Gfx_ScreenWidth(void) {
    return gfx_screen_width();
}

static int32_t Gfx_ScreenHeight(void) {
    return gfx_screen_height();
}

static int32_t Gfx_DisplayCount(void) {
    return gfx_display_count();
}

static void Gfx_SetClipboard(char *text, uint32_t text_high) {
    (void)text;
    (void)text_high;
#line 172
    gfx_set_clipboard(((void *)(text)));
    return;
}

static void Gfx_GetClipboard(char *text, uint32_t text_high) {
    (void)text;
    (void)text_high;
#line 177
    gfx_get_clipboard(((void *)(text)), (text_high + 1));
    return;
}

static int Gfx_HasClipboard(void) {
    return gfx_has_clipboard() != 0;
}

static int32_t Gfx_Ticks(void) {
    return gfx_ticks();
}

static void Gfx_Delay(int32_t ms) {
    (void)ms;
#line 192
    gfx_delay(ms);
    return;
}

static void Gfx_SetCursor(int32_t cursorType) {
    (void)cursorType;
#line 197
    gfx_set_cursor(cursorType);
    return;
}

static void Gfx_ShowCursor(int show) {
    (void)show;
    if (show) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 203
    gfx_show_cursor(1);
    goto L1;
  L3:;
#line 205
    gfx_show_cursor(0);
    goto L1;
}

/* Imported Module Events */

static const int32_t Events_KEY_F2 = 283;
static const int32_t Events_SCAN_T = 23;
static const int32_t Events_WEVT_MAXIMIZED = 7;
static const int32_t Events_KEY_INSERT = 260;
static const int32_t Events_SCAN_ESCAPE = 41;
static const int32_t Events_QUIT_EVENT = 1;
static const int32_t Events_TEXTINPUT = 8;
static const int32_t Events_WEVT_SHOWN = 1;
static const int32_t Events_KEY_DELETE = 127;
static const int32_t Events_KEY_F5 = 286;
static const int32_t Events_SCAN_O = 18;
static const int32_t Events_SCAN_4 = 33;
static const int32_t Events_SCAN_7 = 36;
static const int32_t Events_KEY_RETURN = 13;
static const int32_t Events_KEY_LCTRL = 306;
static const int32_t Events_KEY_LSHIFT = 304;
static const int32_t Events_SCAN_9 = 38;
static const int32_t Events_WEVT_CLOSE = 13;
static const int32_t Events_KEY_LGUI = 310;
static const int32_t Events_WEVT_MOVED = 4;
static const int32_t Events_SCAN_2 = 31;
static const int32_t Events_KEY_SPACE = 32;
static const int32_t Events_SCAN_K = 14;
static const int32_t Events_SCAN_P = 19;
static const int32_t Events_WEVT_FOCUS_GAINED = 11;
static const int32_t Events_KEY_ESCAPE = 27;
static const int32_t Events_KEY_BACKSPACE = 8;
static const int32_t Events_SCAN_TAB = 43;
static const int32_t Events_MOUSEDOWN = 4;
static const int32_t Events_SCAN_U = 24;
static const int32_t Events_NONE = 0;
static const int32_t Events_KEY_END = 263;
static const int32_t Events_KEY_F4 = 285;
static const int32_t Events_BUTTON_MIDDLE = 2;
static const int32_t Events_KEYUP = 3;
static const int32_t Events_WEVT_MINIMIZED = 6;
static const int32_t Events_SCAN_F = 9;
static const int32_t Events_KEY_F10 = 291;
static const int32_t Events_SCAN_H = 11;
static const int32_t Events_SCAN_5 = 34;
static const int32_t Events_MOUSEMOVE = 6;
static const int32_t Events_TEXTEDITING = 10;
static const int32_t Events_SCAN_6 = 35;
static const int32_t Events_KEY_RIGHT = 259;
static const int32_t Events_SCAN_UP = 82;
static const int32_t Events_SCAN_B = 5;
static const int32_t Events_SCAN_Z = 29;
static const int32_t Events_SCAN_D = 7;
static const int32_t Events_KEY_PAGEUP = 264;
static const int32_t Events_MOD_SHIFT = 1;
static const int32_t Events_MOD_GUI = 8;
static const int32_t Events_KEY_KP_4 = 324;
static const int32_t Events_KEY_TAB = 9;
static const int32_t Events_KEY_HOME = 262;
static const int32_t Events_SCAN_Q = 20;
static const int32_t Events_WEVT_RESTORED = 8;
static const int32_t Events_BUTTON_LEFT = 1;
static const int32_t Events_SCAN_C = 6;
static const int32_t Events_WINDOW_EVENT = 9;
static const int32_t Events_WEVT_EXPOSED = 3;
static const int32_t Events_KEY_F9 = 290;
static const int32_t Events_KEY_KP_1 = 321;
static const int32_t Events_SCAN_DOWN = 81;
static const int32_t Events_MOUSEWHEEL = 7;
static const int32_t Events_WEVT_FOCUS_LOST = 12;
static const int32_t Events_KEY_KP_6 = 326;
static const int32_t Events_KEY_RALT = 309;
static const int32_t Events_SCAN_W = 26;
static const int32_t Events_KEY_F6 = 287;
static const int32_t Events_BUTTON_RIGHT = 3;
static const int32_t Events_KEY_F3 = 284;
static const int32_t Events_KEY_CAPSLOCK = 301;
static const int32_t Events_WEVT_RESIZED = 5;
static const int32_t Events_KEY_KP_3 = 323;
static const int32_t Events_WEVT_HIDDEN = 2;
static const int32_t Events_KEY_F11 = 292;
static const int32_t Events_KEY_F8 = 289;
static const int32_t Events_KEY_RSHIFT = 305;
static const int32_t Events_SCAN_BACKSPACE = 42;
static const int32_t Events_WEVT_LEAVE = 10;
static const int32_t Events_KEY_F1 = 282;
static const int32_t Events_KEYDOWN = 2;
static const int32_t Events_KEY_RGUI = 311;
static const int32_t Events_KEY_KP_9 = 329;
static const int32_t Events_WEVT_ENTER = 9;
static const int32_t Events_KEY_KP_0 = 320;
static const int32_t Events_SCAN_J = 13;
static const int32_t Events_SCAN_8 = 37;
static const int32_t Events_SCAN_A = 4;
static const int32_t Events_KEY_NUMLOCK = 300;
static const int32_t Events_MOD_CTRL = 2;
static const int32_t Events_SCAN_RIGHT = 79;
static const int32_t Events_SCAN_G = 10;
static const int32_t Events_SCAN_E = 8;
static const int32_t Events_SCAN_S = 22;
static const int32_t Events_SCAN_LEFT = 80;
static const int32_t Events_SCAN_M = 16;
static const int32_t Events_SCAN_L = 15;
static const int32_t Events_KEY_F7 = 288;
static const int32_t Events_KEY_SCROLLLOCK = 302;
static const int32_t Events_SCAN_3 = 32;
static const int32_t Events_MOD_ALT = 4;
static const int32_t Events_SCAN_N = 17;
static const int32_t Events_SCAN_R = 21;
static const int32_t Events_KEY_F12 = 293;
static const int32_t Events_SCAN_X = 27;
static const int32_t Events_SCAN_0 = 39;
static const int32_t Events_SCAN_RETURN = 40;
static const int32_t Events_SCAN_V = 25;
static const int32_t Events_KEY_KP_8 = 328;
static const int32_t Events_KEY_KP_7 = 327;
static const int32_t Events_KEY_PAGEDOWN = 265;
static const int32_t Events_KEY_DOWN = 257;
static const int32_t Events_SCAN_I = 12;
static const int32_t Events_SCAN_1 = 30;
static const int32_t Events_KEY_KP_5 = 325;
static const int32_t Events_KEY_KP_ENTER = 271;
static const int32_t Events_SCAN_Y = 28;
static const int32_t Events_KEY_LALT = 308;
static const int32_t Events_KEY_RCTRL = 307;
static const int32_t Events_KEY_UP = 256;
static const int32_t Events_MOUSEUP = 5;
static const int32_t Events_KEY_KP_2 = 322;
static const int32_t Events_SCAN_SPACE = 44;
static const int32_t Events_KEY_LEFT = 258;
static int32_t Events_Poll(void);
static int32_t Events_Wait(void);
static int32_t Events_WaitTimeout(int32_t ms);
static int32_t Events_KeyCode(void);
static int32_t Events_ScanCode(void);
static int Events_KeyRepeat(void);
static int32_t Events_KeyMod(void);
static int32_t Events_MouseX(void);
static int32_t Events_MouseY(void);
static int32_t Events_MouseButton(void);
static int32_t Events_WheelX(void);
static int32_t Events_WheelY(void);
static int32_t Events_WindowID(void);
static int32_t Events_WindowEvent(void);
static void Events_TextInput(char *s, uint32_t s_high);
static int32_t Events_TextInputLen(void);
static void Events_StartTextInput(void);
static void Events_StopTextInput(void);
static int Events_IsTextInputActive(void);
static int Events_IsKeyPressed(int32_t scancode);
static int32_t Events_GetMouseState(int32_t *x, int32_t *y);
static int32_t Events_GetMouseGlobal(int32_t *x, int32_t *y);
static void Events_WarpMouse(void * win, int32_t x, int32_t y);

static int32_t Events_Poll(void) {
    return gfx_poll_event();
}

static int32_t Events_Wait(void) {
    return gfx_wait_event();
}

static int32_t Events_WaitTimeout(int32_t ms) {
    (void)ms;
    return gfx_wait_event_timeout(ms);
}

static int32_t Events_KeyCode(void) {
    return gfx_event_key();
}

static int32_t Events_ScanCode(void) {
    return gfx_event_scancode();
}

static int Events_KeyRepeat(void) {
    return gfx_event_key_repeat() != 0;
}

static int32_t Events_KeyMod(void) {
    return gfx_event_mod();
}

static int32_t Events_MouseX(void) {
    return gfx_event_mouse_x();
}

static int32_t Events_MouseY(void) {
    return gfx_event_mouse_y();
}

static int32_t Events_MouseButton(void) {
    return gfx_event_mouse_btn();
}

static int32_t Events_WheelX(void) {
    return gfx_event_wheel_x();
}

static int32_t Events_WheelY(void) {
    return gfx_event_wheel_y();
}

static int32_t Events_WindowID(void) {
    return gfx_event_win_id();
}

static int32_t Events_WindowEvent(void) {
    return gfx_event_win_event();
}

static void Events_TextInput(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
#line 85 "/Users/aoesterer/.mx/lib/m2gfx/src/Events.mod"
    gfx_event_text(((void *)(s)), (s_high + 1));
    return;
}

static int32_t Events_TextInputLen(void) {
    return gfx_event_text_len();
}

static void Events_StartTextInput(void) {
#line 95
    gfx_start_text();
    return;
}

static void Events_StopTextInput(void) {
#line 100
    gfx_stop_text();
    return;
}

static int Events_IsTextInputActive(void) {
    return gfx_is_text_active() != 0;
}

static int Events_IsKeyPressed(int32_t scancode) {
    (void)scancode;
    return gfx_key_state(scancode) != 0;
}

static int32_t Events_GetMouseState(int32_t *x, int32_t *y) {
    (void)x;
    (void)y;
    return gfx_mouse_state(&(*x), &(*y));
}

static int32_t Events_GetMouseGlobal(int32_t *x, int32_t *y) {
    (void)x;
    (void)y;
    return gfx_mouse_global(&(*x), &(*y));
}

static void Events_WarpMouse(void * win, int32_t x, int32_t y) {
    (void)win;
    (void)x;
    (void)y;
#line 125
    gfx_warp_mouse(win, x, y);
    return;
}

/* Imported Module DrawAlgo */

typedef void * DrawAlgo_Ctx;

typedef void (*DrawAlgo_PointFn)(void *, int32_t, int32_t);

typedef void (*DrawAlgo_HLineFn)(void *, int32_t, int32_t, int32_t);

typedef void (*DrawAlgo_LineFn)(void *, int32_t, int32_t, int32_t, int32_t);

static void DrawAlgo_Line(void * ctx, void (*pt)(void *, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2);
static void DrawAlgo_Circle(void * ctx, void (*pt)(void *, int32_t, int32_t), int32_t cx, int32_t cy, int32_t radius);
static void DrawAlgo_FillCircle(void * ctx, void (*hl)(void *, int32_t, int32_t, int32_t), int32_t cx, int32_t cy, int32_t radius);
static void DrawAlgo_Ellipse(void * ctx, void (*pt)(void *, int32_t, int32_t), int32_t cx, int32_t cy, int32_t rx, int32_t ry);
static void DrawAlgo_FillEllipse(void * ctx, void (*hl)(void *, int32_t, int32_t, int32_t), int32_t cx, int32_t cy, int32_t rx, int32_t ry);
static void DrawAlgo_Triangle(void * ctx, void (*ln)(void *, int32_t, int32_t, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3);
static void DrawAlgo_FillTriangle(void * ctx, void (*hl)(void *, int32_t, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3);
static void DrawAlgo_Bezier(void * ctx, void (*ln)(void *, int32_t, int32_t, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2, int32_t x2, int32_t y2, int32_t steps);

static void DrawAlgo_Line(void * ctx, void (*pt)(void *, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2) {
    (void)ctx;
    (void)pt;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    int32_t dx;
    int32_t dy;
    int32_t sx;
    int32_t sy;
    int32_t err;
    int32_t e2;
#line 9 "/Users/aoesterer/.mx/lib/m2gfx/src/DrawAlgo.mod"
    dx = (x2 - x1);
    if (dx < 0) goto L2; else goto L1;
  L1:;
#line 10
    dy = (y2 - m2_y1);
    if (dy < 0) goto L4; else goto L3;
  L2:;
#line 9
    dx = (-dx);
    goto L1;
  L3:;
    if (x1 < x2) goto L6; else goto L7;
  L4:;
#line 10
    dy = (-dy);
    goto L3;
  L5:;
    if (m2_y1 < y2) goto L9; else goto L10;
  L6:;
#line 11
    sx = 1;
    goto L5;
  L7:;
    sx = (-1);
    goto L5;
  L8:;
#line 13
    err = (dx - dy);
    goto L11;
  L9:;
#line 12
    sy = 1;
    goto L8;
  L10:;
    sy = (-1);
    goto L8;
  L11:;
#line 15
    pt(ctx, x1, m2_y1);
    if (x1 == x2) goto L15; else goto L13;
  L12:;
    return;
  L13:;
#line 17
    e2 = (2 * err);
    if (e2 > (-dy)) goto L17; else goto L16;
  L14:;
    goto L12;
  L15:;
    if (m2_y1 == y2) goto L14; else goto L13;
  L16:;
    if (e2 < dx) goto L19; else goto L18;
  L17:;
#line 18
    err = (err - dy);
    x1 = (x1 + sx);
    goto L16;
  L18:;
    goto L11;
  L19:;
#line 19
    err = (err + dx);
    m2_y1 = (m2_y1 + sy);
    goto L18;
}

static void DrawAlgo_Circle(void * ctx, void (*pt)(void *, int32_t, int32_t), int32_t cx, int32_t cy, int32_t radius) {
    (void)ctx;
    (void)pt;
    (void)cx;
    (void)cy;
    (void)radius;
    int32_t x;
    int32_t y;
    int32_t d;
    if (radius < 0) goto L2; else goto L1;
  L1:;
#line 30
    x = radius;
    y = 0;
    d = (1 - radius);
    goto L3;
  L2:;
    return;
  L3:;
    if (x >= y) goto L4; else goto L5;
  L4:;
#line 32
    pt(ctx, (cx + x), (cy + y));
#line 33
    pt(ctx, (cx - x), (cy + y));
#line 34
    pt(ctx, (cx + x), (cy - y));
#line 35
    pt(ctx, (cx - x), (cy - y));
#line 36
    pt(ctx, (cx + y), (cy + x));
#line 37
    pt(ctx, (cx - y), (cy + x));
#line 38
    pt(ctx, (cx + y), (cy - x));
#line 39
    pt(ctx, (cx - y), (cy - x));
#line 40
    (y++);
    if (d <= 0) goto L7; else goto L8;
  L5:;
    return;
  L6:;
    goto L3;
  L7:;
#line 42
    d = ((d + (2 * y)) + 1);
    goto L6;
  L8:;
#line 44
    (x--);
#line 45
    d = ((d + (2 * (y - x))) + 1);
    goto L6;
}

static void DrawAlgo_FillCircle(void * ctx, void (*hl)(void *, int32_t, int32_t, int32_t), int32_t cx, int32_t cy, int32_t radius) {
    (void)ctx;
    (void)hl;
    (void)cx;
    (void)cy;
    (void)radius;
    int32_t x;
    int32_t y;
    int32_t d;
    if (radius < 0) goto L2; else goto L1;
  L1:;
#line 55
    x = radius;
    y = 0;
    d = (1 - radius);
    goto L3;
  L2:;
    return;
  L3:;
    if (x >= y) goto L4; else goto L5;
  L4:;
#line 57
    hl(ctx, (cx - x), (cx + x), (cy + y));
#line 58
    hl(ctx, (cx - x), (cx + x), (cy - y));
#line 59
    hl(ctx, (cx - y), (cx + y), (cy + x));
#line 60
    hl(ctx, (cx - y), (cx + y), (cy - x));
#line 61
    (y++);
    if (d <= 0) goto L7; else goto L8;
  L5:;
    return;
  L6:;
    goto L3;
  L7:;
#line 63
    d = ((d + (2 * y)) + 1);
    goto L6;
  L8:;
#line 65
    (x--);
#line 66
    d = ((d + (2 * (y - x))) + 1);
    goto L6;
}

static void DrawAlgo_Ellipse(void * ctx, void (*pt)(void *, int32_t, int32_t), int32_t cx, int32_t cy, int32_t rx, int32_t ry) {
    (void)ctx;
    (void)pt;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    int32_t x;
    int32_t y;
    double rx2;
    double ry2;
    double twoRx2;
    double twoRy2;
    double px;
    double py;
    double d1;
    double d2;
    if (rx < 0) goto L2; else goto L3;
  L1:;
#line 79
    rx2 = (((double)(rx)) * ((double)(rx)));
#line 80
    ry2 = (((double)(ry)) * ((double)(ry)));
#line 81
    twoRx2 = (2.0 * rx2);
#line 82
    twoRy2 = (2.0 * ry2);
#line 83
    x = 0;
    y = ry;
#line 84
    px = 0.0;
    py = (twoRx2 * ((double)(y)));
#line 85
    d1 = ((ry2 - (rx2 * ((double)(ry)))) + ((double)(rx2) / (double)(4.0)));
    goto L4;
  L2:;
    return;
  L3:;
    if (ry < 0) goto L2; else goto L1;
  L4:;
    if (px < py) goto L5; else goto L6;
  L5:;
#line 87
    pt(ctx, (cx + x), (cy + y));
#line 88
    pt(ctx, (cx - x), (cy + y));
#line 89
    pt(ctx, (cx + x), (cy - y));
#line 90
    pt(ctx, (cx - x), (cy - y));
#line 91
    (x++);
#line 92
    px = (px + twoRy2);
    if (d1 < 0.0) goto L8; else goto L9;
  L6:;
#line 101
    d2 = ((((double)(((ry2 * ((double)(((2 * x) + 1)))) * ((double)(((2 * x) + 1))))) / (double)(4.0)) + ((rx2 * ((double)((y - 1)))) * ((double)((y - 1))))) - (rx2 * ry2));
    goto L10;
  L7:;
    goto L4;
  L8:;
#line 94
    d1 = ((d1 + ry2) + px);
    goto L7;
  L9:;
#line 96
    (y--);
#line 97
    py = (py - twoRx2);
#line 98
    d1 = (((d1 + ry2) + px) - py);
    goto L7;
  L10:;
    if (y >= 0) goto L11; else goto L12;
  L11:;
#line 105
    pt(ctx, (cx + x), (cy + y));
#line 106
    pt(ctx, (cx - x), (cy + y));
#line 107
    pt(ctx, (cx + x), (cy - y));
#line 108
    pt(ctx, (cx - x), (cy - y));
#line 109
    (y--);
#line 110
    py = (py - twoRx2);
    if (d2 > 0.0) goto L14; else goto L15;
  L12:;
    return;
  L13:;
    goto L10;
  L14:;
#line 112
    d2 = ((d2 + rx2) - py);
    goto L13;
  L15:;
#line 114
    (x++);
#line 115
    px = (px + twoRy2);
#line 116
    d2 = (((d2 + rx2) - py) + px);
    goto L13;
}

static void DrawAlgo_FillEllipse(void * ctx, void (*hl)(void *, int32_t, int32_t, int32_t), int32_t cx, int32_t cy, int32_t rx, int32_t ry) {
    (void)ctx;
    (void)hl;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    int32_t x;
    int32_t y;
    int32_t lastY;
    double rx2;
    double ry2;
    double twoRx2;
    double twoRy2;
    double px;
    double py;
    double d1;
    double d2;
    if (rx < 0) goto L2; else goto L3;
  L1:;
#line 127
    rx2 = (((double)(rx)) * ((double)(rx)));
#line 128
    ry2 = (((double)(ry)) * ((double)(ry)));
#line 129
    twoRx2 = (2.0 * rx2);
#line 130
    twoRy2 = (2.0 * ry2);
#line 131
    x = 0;
    y = ry;
#line 132
    px = 0.0;
    py = (twoRx2 * ((double)(y)));
#line 133
    lastY = (-1);
#line 134
    d1 = ((ry2 - (rx2 * ((double)(ry)))) + ((double)(rx2) / (double)(4.0)));
    goto L4;
  L2:;
    return;
  L3:;
    if (ry < 0) goto L2; else goto L1;
  L4:;
    if (px < py) goto L5; else goto L6;
  L5:;
    if (y != lastY) goto L8; else goto L7;
  L6:;
#line 154
    d2 = ((((double)(((ry2 * ((double)(((2 * x) + 1)))) * ((double)(((2 * x) + 1))))) / (double)(4.0)) + ((rx2 * ((double)((y - 1)))) * ((double)((y - 1))))) - (rx2 * ry2));
    goto L12;
  L7:;
#line 141
    (x++);
#line 142
    px = (px + twoRy2);
    if (d1 < 0.0) goto L10; else goto L11;
  L8:;
#line 137
    hl(ctx, (cx - x), (cx + x), (cy + y));
#line 138
    hl(ctx, (cx - x), (cx + x), (cy - y));
#line 139
    lastY = y;
    goto L7;
  L9:;
    goto L4;
  L10:;
#line 144
    d1 = ((d1 + ry2) + px);
    goto L9;
  L11:;
#line 146
    hl(ctx, (cx - x), (cx + x), (cy + y));
#line 147
    hl(ctx, (cx - x), (cx + x), (cy - y));
#line 148
    (y--);
#line 149
    py = (py - twoRx2);
#line 150
    d1 = (((d1 + ry2) + px) - py);
#line 151
    lastY = y;
    goto L9;
  L12:;
    if (y >= 0) goto L13; else goto L14;
  L13:;
#line 158
    hl(ctx, (cx - x), (cx + x), (cy + y));
#line 159
    hl(ctx, (cx - x), (cx + x), (cy - y));
#line 160
    (y--);
#line 161
    py = (py - twoRx2);
    if (d2 > 0.0) goto L16; else goto L17;
  L14:;
    return;
  L15:;
    goto L12;
  L16:;
#line 163
    d2 = ((d2 + rx2) - py);
    goto L15;
  L17:;
#line 165
    (x++);
#line 166
    px = (px + twoRy2);
#line 167
    d2 = (((d2 + rx2) - py) + px);
    goto L15;
}

static void DrawAlgo_Triangle(void * ctx, void (*ln)(void *, int32_t, int32_t, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3) {
    (void)ctx;
    (void)ln;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
#line 177
    ln(ctx, x1, m2_y1, x2, y2);
#line 178
    ln(ctx, x2, y2, x3, y3);
#line 179
    ln(ctx, x3, y3, x1, m2_y1);
    return;
}

static void DrawAlgo_FillTriangle(void * ctx, void (*hl)(void *, int32_t, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3) {
    (void)ctx;
    (void)hl;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    int32_t vx0;
    int32_t vy0;
    int32_t vx1;
    int32_t vy1;
    int32_t vx2;
    int32_t vy2;
    int32_t tmp;
    int32_t scanY;
    int32_t xa;
    int32_t xb;
    int32_t minX;
    int32_t maxX;
#line 187
    vx0 = x1;
    vy0 = m2_y1;
#line 188
    vx1 = x2;
    vy1 = y2;
#line 189
    vx2 = x3;
    vy2 = y3;
    if (vy0 > vy1) goto L2; else goto L1;
  L1:;
    if (vy0 > vy2) goto L4; else goto L3;
  L2:;
#line 191
    tmp = vx0;
    vx0 = vx1;
    vx1 = tmp;
#line 192
    tmp = vy0;
    vy0 = vy1;
    vy1 = tmp;
    goto L1;
  L3:;
    if (vy1 > vy2) goto L6; else goto L5;
  L4:;
#line 195
    tmp = vx0;
    vx0 = vx2;
    vx2 = tmp;
#line 196
    tmp = vy0;
    vy0 = vy2;
    vy2 = tmp;
    goto L3;
  L5:;
    if (vy0 == vy2) goto L8; else goto L7;
  L6:;
#line 199
    tmp = vx1;
    vx1 = vx2;
    vx2 = tmp;
#line 200
    tmp = vy1;
    vy1 = vy2;
    vy2 = tmp;
    goto L5;
  L7:;
    scanY = vy0;
    goto L17;
  L8:;
#line 203
    minX = vx0;
    if (vx1 < minX) goto L10; else goto L9;
  L9:;
    if (vx2 < minX) goto L12; else goto L11;
  L10:;
#line 204
    minX = vx1;
    goto L9;
  L11:;
#line 206
    maxX = vx0;
    if (vx1 > maxX) goto L14; else goto L13;
  L12:;
#line 205
    minX = vx2;
    goto L11;
  L13:;
    if (vx2 > maxX) goto L16; else goto L15;
  L14:;
#line 207
    maxX = vx1;
    goto L13;
  L15:;
#line 209
    hl(ctx, minX, maxX, vy0);
    return;
  L16:;
#line 208
    maxX = vx2;
    goto L15;
  L17:;
    if (scanY <= vy2) goto L18; else goto L20;
  L18:;
    if (vy2 != vy0) goto L22; else goto L23;
  L19:;
    scanY = (scanY + 1);
    goto L17;
  L20:;
    return;
  L21:;
    if (scanY <= vy1) goto L25; else goto L26;
  L22:;
#line 214
    xa = (vx0 + ((int32_t)(((double)((((double)((vx2 - vx0))) * ((double)((scanY - vy0))))) / (double)(((double)((vy2 - vy0))))))));
    goto L21;
  L23:;
#line 216
    xa = vx0;
    goto L21;
  L24:;
    if (xa > xb) goto L34; else goto L33;
  L25:;
    if (vy1 != vy0) goto L28; else goto L29;
  L26:;
    if (vy2 != vy1) goto L31; else goto L32;
  L27:;
    goto L24;
  L28:;
#line 220
    xb = (vx0 + ((int32_t)(((double)((((double)((vx1 - vx0))) * ((double)((scanY - vy0))))) / (double)(((double)((vy1 - vy0))))))));
    goto L27;
  L29:;
#line 222
    xb = vx1;
    goto L27;
  L30:;
    goto L24;
  L31:;
#line 226
    xb = (vx1 + ((int32_t)(((double)((((double)((vx2 - vx1))) * ((double)((scanY - vy1))))) / (double)(((double)((vy2 - vy1))))))));
    goto L30;
  L32:;
#line 228
    xb = vx2;
    goto L30;
  L33:;
#line 232
    hl(ctx, xa, xb, scanY);
    goto L19;
  L34:;
#line 231
    tmp = xa;
    xa = xb;
    xb = tmp;
    goto L33;
}

static void DrawAlgo_Bezier(void * ctx, void (*ln)(void *, int32_t, int32_t, int32_t, int32_t), int32_t x1, int32_t m2_y1, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2, int32_t x2, int32_t y2, int32_t steps) {
    (void)ctx;
    (void)ln;
    (void)x1;
    (void)m2_y1;
    (void)cx1;
    (void)cy1;
    (void)cx2;
    (void)cy2;
    (void)x2;
    (void)y2;
    (void)steps;
    int32_t i;
    int32_t prevX;
    int32_t prevY;
    int32_t nextX;
    int32_t nextY;
    float t;
    float u;
    float u2;
    float u3;
    float t2;
    float t3;
    float bezX;
    float bezY;
    if (steps < 1) goto L2; else goto L1;
  L1:;
#line 245
    prevX = x1;
    prevY = m2_y1;
    i = 1;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= steps) goto L4; else goto L6;
  L4:;
#line 247
    t = ((double)(((float)(i))) / (double)(((float)(steps))));
#line 248
    u = (1.0 - t);
#line 249
    u2 = (u * u);
    u3 = (u2 * u);
#line 250
    t2 = (t * t);
    t3 = (t2 * t);
#line 251
    bezX = ((((u3 * ((float)(x1))) + (((3.0 * u2) * t) * ((float)(cx1)))) + (((3.0 * u) * t2) * ((float)(cx2)))) + (t3 * ((float)(x2))));
#line 253
    bezY = ((((u3 * ((float)(m2_y1))) + (((3.0 * u2) * t) * ((float)(cy1)))) + (((3.0 * u) * t2) * ((float)(cy2)))) + (t3 * ((float)(y2))));
#line 255
    nextX = ((int32_t)((bezX + 0.5)));
#line 256
    nextY = ((int32_t)((bezY + 0.5)));
#line 257
    ln(ctx, prevX, prevY, nextX, nextY);
#line 258
    prevX = nextX;
#line 259
    prevY = nextY;
    goto L5;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
    return;
}

/* Imported Module MathLib0 */

static const int32_t MathLib0_RandMax = 2147483647;
static float MathLib0_sqrt(float x);
static float MathLib0_sin(float x);
static float MathLib0_cos(float x);
static float MathLib0_exp(float x);
static float MathLib0_ln(float x);
static float MathLib0_arctan(float x);
static int32_t MathLib0_entier(float x);
static float MathLib0_Random(void);
static void MathLib0_Randomize(uint32_t seed);

static float MathLib0_sqrt(float x) {
    (void)x;
    return sqrtf(x);
}

static float MathLib0_sin(float x) {
    (void)x;
    return sinf(x);
}

static float MathLib0_cos(float x) {
    (void)x;
    return cosf(x);
}

static float MathLib0_exp(float x) {
    (void)x;
    return expf(x);
}

static float MathLib0_ln(float x) {
    (void)x;
    return logf(x);
}

static float MathLib0_arctan(float x) {
    (void)x;
    return atanf(x);
}

static int32_t MathLib0_entier(float x) {
    (void)x;
    return ((int32_t)(floorf(x)));
}

static float MathLib0_Random(void) {
    return ((double)(((float)(rand()))) / (double)((((float)(2147483647)) + 1.0)));
}

static void MathLib0_Randomize(uint32_t seed) {
    (void)seed;
#line 35 "/Users/aoesterer/.mx/lib/m2stdlib/src/MathLib0.mod"
    srand(seed);
    return;
}

/* Imported Module Canvas */

static const int32_t Canvas_BLEND_MOD = 4;
static const int32_t Canvas_BLEND_ALPHA = 1;
static const int32_t Canvas_BLEND_ADD = 2;
static const int32_t Canvas_BLEND_NONE = 0;
static void Canvas_RenPt(void * ctx, int32_t x, int32_t y);
static void Canvas_RenHL(void * ctx, int32_t x1, int32_t x2, int32_t y);
static void Canvas_RenLn(void * ctx, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2);
static void Canvas_SetColor(void * ren, int32_t r, int32_t g, int32_t b, int32_t a);
static void Canvas_GetColor(void * ren, int32_t *r, int32_t *g, int32_t *b, int32_t *a);
static void Canvas_Clear(void * ren);
static void Canvas_DrawRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
static void Canvas_FillRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
static void Canvas_DrawLine(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2);
static void Canvas_DrawPoint(void * ren, int32_t x, int32_t y);
static void Canvas_DrawCircle(void * ren, int32_t cx, int32_t cy, int32_t radius);
static void Canvas_FillCircle(void * ren, int32_t cx, int32_t cy, int32_t radius);
static void Canvas_DrawEllipse(void * ren, int32_t cx, int32_t cy, int32_t rx, int32_t ry);
static void Canvas_FillEllipse(void * ren, int32_t cx, int32_t cy, int32_t rx, int32_t ry);
static void Canvas_DrawTriangle(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3);
static void Canvas_FillTriangle(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3);
static void Canvas_DrawBezier(void * ren, int32_t x1, int32_t m2_y1, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2, int32_t x2, int32_t y2, int32_t steps);
static void Canvas_DrawRoundRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius);
static void Canvas_FillRoundRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius);
static void Canvas_DrawThickLine(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t thickness);
static void Canvas_DrawArc(void * ren, int32_t cx, int32_t cy, int32_t radius, int32_t startDeg, int32_t endDeg);
static void Canvas_SetClip(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
static void Canvas_ClearClip(void * ren);
static int32_t Canvas_GetClipX(void * ren);
static int32_t Canvas_GetClipY(void * ren);
static int32_t Canvas_GetClipW(void * ren);
static int32_t Canvas_GetClipH(void * ren);
static void Canvas_SetBlendMode(void * ren, int32_t mode);
static void Canvas_SetViewport(void * ren, int32_t x, int32_t y, int32_t w, int32_t h);
static void Canvas_ResetViewport(void * ren);

static void Canvas_RenPt(void * ctx, int32_t x, int32_t y) {
    (void)ctx;
    (void)x;
    (void)y;
#line 18 "/Users/aoesterer/.mx/lib/m2gfx/src/Canvas.mod"
    gfx_draw_point(ctx, x, y);
    return;
}

static void Canvas_RenHL(void * ctx, int32_t x1, int32_t x2, int32_t y) {
    (void)ctx;
    (void)x1;
    (void)x2;
    (void)y;
#line 21
    gfx_draw_line(ctx, x1, y, x2, y);
    return;
}

static void Canvas_RenLn(void * ctx, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2) {
    (void)ctx;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
#line 24
    gfx_draw_line(ctx, x1, m2_y1, x2, y2);
    return;
}

static void Canvas_SetColor(void * ren, int32_t r, int32_t g, int32_t b, int32_t a) {
    (void)ren;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
#line 29
    gfx_set_color(ren, r, g, b, a);
    return;
}

static void Canvas_GetColor(void * ren, int32_t *r, int32_t *g, int32_t *b, int32_t *a) {
    (void)ren;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
#line 32
    gfx_get_color(ren, &(*r), &(*g), &(*b), &(*a));
    return;
}

static void Canvas_Clear(void * ren) {
    (void)ren;
#line 35
    gfx_clear(ren);
    return;
}

static void Canvas_DrawRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)ren;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#line 40
    gfx_draw_rect(ren, x, y, w, h);
    return;
}

static void Canvas_FillRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)ren;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#line 43
    gfx_fill_rect(ren, x, y, w, h);
    return;
}

static void Canvas_DrawLine(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2) {
    (void)ren;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
#line 48
    gfx_draw_line(ren, x1, m2_y1, x2, y2);
    return;
}

static void Canvas_DrawPoint(void * ren, int32_t x, int32_t y) {
    (void)ren;
    (void)x;
    (void)y;
#line 51
    gfx_draw_point(ren, x, y);
    return;
}

static void Canvas_DrawCircle(void * ren, int32_t cx, int32_t cy, int32_t radius) {
    (void)ren;
    (void)cx;
    (void)cy;
    (void)radius;
#line 56
    DrawAlgo_Circle(ren, Canvas_RenPt, cx, cy, radius);
    return;
}

static void Canvas_FillCircle(void * ren, int32_t cx, int32_t cy, int32_t radius) {
    (void)ren;
    (void)cx;
    (void)cy;
    (void)radius;
#line 59
    DrawAlgo_FillCircle(ren, Canvas_RenHL, cx, cy, radius);
    return;
}

static void Canvas_DrawEllipse(void * ren, int32_t cx, int32_t cy, int32_t rx, int32_t ry) {
    (void)ren;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
#line 62
    DrawAlgo_Ellipse(ren, Canvas_RenPt, cx, cy, rx, ry);
    return;
}

static void Canvas_FillEllipse(void * ren, int32_t cx, int32_t cy, int32_t rx, int32_t ry) {
    (void)ren;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
#line 65
    DrawAlgo_FillEllipse(ren, Canvas_RenHL, cx, cy, rx, ry);
    return;
}

static void Canvas_DrawTriangle(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3) {
    (void)ren;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
#line 68
    DrawAlgo_Triangle(ren, Canvas_RenLn, x1, m2_y1, x2, y2, x3, y3);
    return;
}

static void Canvas_FillTriangle(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3) {
    (void)ren;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
#line 71
    DrawAlgo_FillTriangle(ren, Canvas_RenHL, x1, m2_y1, x2, y2, x3, y3);
    return;
}

static void Canvas_DrawBezier(void * ren, int32_t x1, int32_t m2_y1, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2, int32_t x2, int32_t y2, int32_t steps) {
    (void)ren;
    (void)x1;
    (void)m2_y1;
    (void)cx1;
    (void)cy1;
    (void)cx2;
    (void)cy2;
    (void)x2;
    (void)y2;
    (void)steps;
#line 76
    DrawAlgo_Bezier(ren, Canvas_RenLn, x1, m2_y1, cx1, cy1, cx2, cy2, x2, y2, steps);
    return;
}

static void Canvas_DrawRoundRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius) {
    (void)ren;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)radius;
    int32_t rad;
    int32_t tlCx;
    int32_t tlCy;
    int32_t trCx;
    int32_t trCy;
    int32_t blCx;
    int32_t blCy;
    int32_t brCx;
    int32_t brCy;
    int32_t cx;
    int32_t cy;
    int32_t d;
#line 85
    rad = radius;
    if (rad > m2_div(w, 2)) goto L2; else goto L1;
  L1:;
    if (rad > m2_div(h, 2)) goto L4; else goto L3;
  L2:;
#line 86
    rad = m2_div(w, 2);
    goto L1;
  L3:;
    if (rad < 0) goto L6; else goto L5;
  L4:;
#line 87
    rad = m2_div(h, 2);
    goto L3;
  L5:;
#line 89
    tlCx = (x + rad);
    tlCy = (y + rad);
#line 90
    trCx = (((x + w) - 1) - rad);
    trCy = (y + rad);
#line 91
    blCx = (x + rad);
    blCy = (((y + h) - 1) - rad);
#line 92
    brCx = (((x + w) - 1) - rad);
    brCy = (((y + h) - 1) - rad);
#line 93
    gfx_draw_line(ren, tlCx, y, trCx, y);
#line 94
    gfx_draw_line(ren, tlCx, ((y + h) - 1), brCx, ((y + h) - 1));
#line 95
    gfx_draw_line(ren, x, tlCy, x, blCy);
#line 96
    gfx_draw_line(ren, ((x + w) - 1), trCy, ((x + w) - 1), brCy);
#line 97
    cx = rad;
    cy = 0;
    d = (1 - rad);
    goto L7;
  L6:;
#line 88
    rad = 0;
    goto L5;
  L7:;
    if (cx >= cy) goto L8; else goto L9;
  L8:;
#line 99
    gfx_draw_point(ren, (trCx + cx), (trCy - cy));
#line 100
    gfx_draw_point(ren, (trCx + cy), (trCy - cx));
#line 101
    gfx_draw_point(ren, (tlCx - cx), (tlCy - cy));
#line 102
    gfx_draw_point(ren, (tlCx - cy), (tlCy - cx));
#line 103
    gfx_draw_point(ren, (brCx + cx), (brCy + cy));
#line 104
    gfx_draw_point(ren, (brCx + cy), (brCy + cx));
#line 105
    gfx_draw_point(ren, (blCx - cx), (blCy + cy));
#line 106
    gfx_draw_point(ren, (blCx - cy), (blCy + cx));
#line 107
    (cy++);
    if (d <= 0) goto L11; else goto L12;
  L9:;
    return;
  L10:;
    goto L7;
  L11:;
#line 109
    d = ((d + (2 * cy)) + 1);
    goto L10;
  L12:;
#line 111
    (cx--);
#line 112
    d = ((d + (2 * (cy - cx))) + 1);
    goto L10;
}

static void Canvas_FillRoundRect(void * ren, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius) {
    (void)ren;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)radius;
    int32_t rad;
    int32_t tlCx;
    int32_t tlCy;
    int32_t trCx;
    int32_t trCy;
    int32_t blCx;
    int32_t blCy;
    int32_t brCx;
    int32_t brCy;
    int32_t cx;
    int32_t cy;
    int32_t d;
#line 122
    rad = radius;
    if (rad > m2_div(w, 2)) goto L2; else goto L1;
  L1:;
    if (rad > m2_div(h, 2)) goto L4; else goto L3;
  L2:;
#line 123
    rad = m2_div(w, 2);
    goto L1;
  L3:;
    if (rad < 0) goto L6; else goto L5;
  L4:;
#line 124
    rad = m2_div(h, 2);
    goto L3;
  L5:;
#line 126
    gfx_fill_rect(ren, x, (y + rad), w, (h - (2 * rad)));
#line 127
    gfx_fill_rect(ren, (x + rad), y, (w - (2 * rad)), rad);
#line 128
    gfx_fill_rect(ren, (x + rad), ((y + h) - rad), (w - (2 * rad)), rad);
#line 129
    tlCx = (x + rad);
    tlCy = (y + rad);
#line 130
    trCx = (((x + w) - 1) - rad);
    trCy = (y + rad);
#line 131
    blCx = (x + rad);
    blCy = (((y + h) - 1) - rad);
#line 132
    brCx = (((x + w) - 1) - rad);
    brCy = (((y + h) - 1) - rad);
#line 133
    cx = rad;
    cy = 0;
    d = (1 - rad);
    goto L7;
  L6:;
#line 125
    rad = 0;
    goto L5;
  L7:;
    if (cx >= cy) goto L8; else goto L9;
  L8:;
#line 135
    gfx_draw_line(ren, (tlCx - cx), (tlCy - cy), (trCx + cx), (trCy - cy));
#line 136
    gfx_draw_line(ren, (tlCx - cy), (tlCy - cx), (trCx + cy), (trCy - cx));
#line 137
    gfx_draw_line(ren, (blCx - cx), (blCy + cy), (brCx + cx), (brCy + cy));
#line 138
    gfx_draw_line(ren, (blCx - cy), (blCy + cx), (brCx + cy), (brCy + cx));
#line 139
    (cy++);
    if (d <= 0) goto L11; else goto L12;
  L9:;
    return;
  L10:;
    goto L7;
  L11:;
#line 141
    d = ((d + (2 * cy)) + 1);
    goto L10;
  L12:;
#line 143
    (cx--);
#line 144
    d = ((d + (2 * (cy - cx))) + 1);
    goto L10;
}

static void Canvas_DrawThickLine(void * ren, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t thickness) {
    (void)ren;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)thickness;
    float dx;
    float dy;
    float len;
    float half;
    float px;
    float py;
    int32_t qx0;
    int32_t qy0;
    int32_t qx1;
    int32_t qy1;
    int32_t qx2;
    int32_t qy2;
    int32_t qx3;
    int32_t qy3;
    if (thickness <= 0) goto L2; else goto L1;
  L1:;
    if (thickness == 1) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 158
    dx = ((float)((x2 - x1)));
#line 159
    dy = ((float)((y2 - m2_y1)));
#line 160
    len = sqrtf(((dx * dx) + (dy * dy)));
    if (len < 0.001) goto L6; else goto L5;
  L4:;
#line 155
    gfx_draw_line(ren, x1, m2_y1, x2, y2);
    return;
  L5:;
#line 165
    half = ((double)(((float)(thickness))) / (double)(2.0));
#line 166
    px = (((double)((-dy)) / (double)(len)) * half);
#line 167
    py = (((double)(dx) / (double)(len)) * half);
#line 168
    qx0 = ((int32_t)(((((float)(x1)) + px) + 0.5)));
#line 169
    qy0 = ((int32_t)(((((float)(m2_y1)) + py) + 0.5)));
#line 170
    qx1 = ((int32_t)(((((float)(x1)) - px) + 0.5)));
#line 171
    qy1 = ((int32_t)(((((float)(m2_y1)) - py) + 0.5)));
#line 172
    qx2 = ((int32_t)(((((float)(x2)) - px) + 0.5)));
#line 173
    qy2 = ((int32_t)(((((float)(y2)) - py) + 0.5)));
#line 174
    qx3 = ((int32_t)(((((float)(x2)) + px) + 0.5)));
#line 175
    qy3 = ((int32_t)(((((float)(y2)) + py) + 0.5)));
#line 176
    Canvas_FillTriangle(ren, qx0, qy0, qx1, qy1, qx2, qy2);
#line 177
    Canvas_FillTriangle(ren, qx0, qy0, qx2, qy2, qx3, qy3);
    return;
  L6:;
#line 162
    Canvas_FillCircle(ren, x1, m2_y1, m2_div(thickness, 2));
    return;
}

static void Canvas_DrawArc(void * ren, int32_t cx, int32_t cy, int32_t radius, int32_t startDeg, int32_t endDeg) {
    (void)ren;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)startDeg;
    (void)endDeg;
    static const float PI = 3.14159265;
    int32_t steps;
    int32_t i;
    int32_t prevX;
    int32_t prevY;
    int32_t nextX;
    int32_t nextY;
    int32_t sDeg;
    int32_t eDeg;
    float startRad;
    float endRad;
    float step;
    float angle;
    if (radius <= 0) goto L2; else goto L1;
  L1:;
#line 186
    sDeg = startDeg;
    eDeg = endDeg;
    goto L3;
  L2:;
    return;
  L3:;
    if (sDeg < 0) goto L4; else goto L5;
  L4:;
#line 187
    (sDeg += 360);
    goto L3;
  L5:;
    goto L6;
  L6:;
    if (eDeg < 0) goto L7; else goto L8;
  L7:;
#line 188
    (eDeg += 360);
    goto L6;
  L8:;
#line 189
    sDeg = m2_mod(sDeg, 360);
#line 190
    eDeg = m2_mod(eDeg, 360);
#line 191
    steps = ((int32_t)(((2.0 * 3.14159265) * ((float)(radius)))));
    if (steps < 36) goto L10; else goto L9;
  L9:;
#line 193
    startRad = ((double)((((float)(sDeg)) * 3.14159265)) / (double)(180.0));
#line 194
    endRad = ((double)((((float)(eDeg)) * 3.14159265)) / (double)(180.0));
    if (endRad <= startRad) goto L12; else goto L11;
  L10:;
#line 192
    steps = 36;
    goto L9;
  L11:;
#line 196
    step = ((double)((endRad - startRad)) / (double)(((float)(steps))));
#line 197
    prevX = (cx + ((int32_t)(((cosf(startRad) * ((float)(radius))) + 0.5))));
#line 198
    prevY = (cy - ((int32_t)(((sinf(startRad) * ((float)(radius))) + 0.5))));
    i = 1;
    goto L13;
  L12:;
#line 195
    endRad = (endRad + (2.0 * 3.14159265));
    goto L11;
  L13:;
    if (i <= steps) goto L14; else goto L16;
  L14:;
#line 200
    angle = (startRad + (step * ((float)(i))));
#line 201
    nextX = (cx + ((int32_t)(((cosf(angle) * ((float)(radius))) + 0.5))));
#line 202
    nextY = (cy - ((int32_t)(((sinf(angle) * ((float)(radius))) + 0.5))));
#line 203
    gfx_draw_line(ren, prevX, prevY, nextX, nextY);
#line 204
    prevX = nextX;
#line 205
    prevY = nextY;
    goto L15;
  L15:;
    i = (i + 1);
    goto L13;
  L16:;
    return;
}

static void Canvas_SetClip(void * ren, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)ren;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#line 212
    gfx_set_clip(ren, x, y, w, h);
    return;
}

static void Canvas_ClearClip(void * ren) {
    (void)ren;
#line 215
    gfx_clear_clip(ren);
    return;
}

static int32_t Canvas_GetClipX(void * ren) {
    (void)ren;
    return gfx_get_clip_x(ren);
}

static int32_t Canvas_GetClipY(void * ren) {
    (void)ren;
    return gfx_get_clip_y(ren);
}

static int32_t Canvas_GetClipW(void * ren) {
    (void)ren;
    return gfx_get_clip_w(ren);
}

static int32_t Canvas_GetClipH(void * ren) {
    (void)ren;
    return gfx_get_clip_h(ren);
}

static void Canvas_SetBlendMode(void * ren, int32_t mode) {
    (void)ren;
    (void)mode;
#line 232
    gfx_set_blend(ren, mode);
    return;
}

static void Canvas_SetViewport(void * ren, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)ren;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#line 237
    gfx_set_viewport(ren, x, y, w, h);
    return;
}

static void Canvas_ResetViewport(void * ren) {
    (void)ren;
#line 240
    gfx_reset_viewport(ren);
    return;
}

/* Imported Module Texture */

static const int32_t Texture_FLIP_NONE = 0;
static const int32_t Texture_FLIP_HORIZONTAL = 1;
static const int32_t Texture_FLIP_BOTH = 3;
static const int32_t Texture_FLIP_VERTICAL = 2;
typedef void * Texture_Tex;

static void * Texture_LoadBMP(void * ren, char *path, uint32_t path_high);
static void * Texture_LoadBMPKeyed(void * ren, char *path, uint32_t path_high, int32_t kr, int32_t kg, int32_t kb);
static void * Texture_Create(void * ren, int32_t w, int32_t h);
static void * Texture_FromText(void * ren, void * font, char *text, uint32_t text_high, int32_t r, int32_t g, int32_t b, int32_t a);
static void Texture_Destroy(void * tex);
static void Texture_Draw(void * ren, void * tex, int32_t x, int32_t y);
static void Texture_DrawRegion(void * ren, void * tex, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, int32_t dw, int32_t dh);
static void Texture_DrawRotated(void * ren, void * tex, int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t angleDeg, int32_t flip);
static int32_t Texture_Width(void * tex);
static int32_t Texture_Height(void * tex);
static void Texture_SetAlpha(void * tex, int32_t alpha);
static void Texture_SetBlendMode(void * tex, int32_t mode);
static void Texture_SetColorMod(void * tex, int32_t r, int32_t g, int32_t b);
static void Texture_SetTarget(void * ren, void * tex);
static void Texture_ResetTarget(void * ren);

static void * Texture_LoadBMP(void * ren, char *path, uint32_t path_high) {
    (void)ren;
    (void)path;
    (void)path_high;
    return gfx_load_bmp(ren, ((void *)(path)));
}

static void * Texture_LoadBMPKeyed(void * ren, char *path, uint32_t path_high, int32_t kr, int32_t kg, int32_t kb) {
    (void)ren;
    (void)path;
    (void)path_high;
    (void)kr;
    (void)kg;
    (void)kb;
    return gfx_load_bmp_keyed(ren, ((void *)(path)), kr, kg, kb);
}

static void * Texture_Create(void * ren, int32_t w, int32_t h) {
    (void)ren;
    (void)w;
    (void)h;
    return gfx_create_texture(ren, w, h);
}

static void * Texture_FromText(void * ren, void * font, char *text, uint32_t text_high, int32_t r, int32_t g, int32_t b, int32_t a) {
    (void)ren;
    (void)font;
    (void)text;
    (void)text_high;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    return gfx_text_texture(ren, font, ((void *)(text)), r, g, b, a);
}

static void Texture_Destroy(void * tex) {
    (void)tex;
#line 36 "/Users/aoesterer/.mx/lib/m2gfx/src/Texture.mod"
    gfx_destroy_texture(tex);
    return;
}

static void Texture_Draw(void * ren, void * tex, int32_t x, int32_t y) {
    (void)ren;
    (void)tex;
    (void)x;
    (void)y;
#line 41
    gfx_draw_texture(ren, tex, x, y);
    return;
}

static void Texture_DrawRegion(void * ren, void * tex, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
    (void)ren;
    (void)tex;
    (void)sx;
    (void)sy;
    (void)sw;
    (void)sh;
    (void)dx;
    (void)dy;
    (void)dw;
    (void)dh;
#line 48
    gfx_draw_texture_ex(ren, tex, sx, sy, sw, sh, dx, dy, dw, dh);
    return;
}

static void Texture_DrawRotated(void * ren, void * tex, int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t angleDeg, int32_t flip) {
    (void)ren;
    (void)tex;
    (void)dx;
    (void)dy;
    (void)dw;
    (void)dh;
    (void)angleDeg;
    (void)flip;
#line 55
    gfx_draw_texture_rot(ren, tex, dx, dy, dw, dh, angleDeg, flip);
    return;
}

static int32_t Texture_Width(void * tex) {
    (void)tex;
    return gfx_tex_width(tex);
}

static int32_t Texture_Height(void * tex) {
    (void)tex;
    return gfx_tex_height(tex);
}

static void Texture_SetAlpha(void * tex, int32_t alpha) {
    (void)tex;
    (void)alpha;
#line 70
    gfx_set_tex_alpha(tex, alpha);
    return;
}

static void Texture_SetBlendMode(void * tex, int32_t mode) {
    (void)tex;
    (void)mode;
#line 75
    gfx_set_tex_blend(tex, mode);
    return;
}

static void Texture_SetColorMod(void * tex, int32_t r, int32_t g, int32_t b) {
    (void)tex;
    (void)r;
    (void)g;
    (void)b;
#line 80
    gfx_set_tex_color(tex, r, g, b);
    return;
}

static void Texture_SetTarget(void * ren, void * tex) {
    (void)ren;
    (void)tex;
#line 85
    gfx_set_target(ren, tex);
    return;
}

static void Texture_ResetTarget(void * ren) {
    (void)ren;
#line 90
    gfx_reset_target(ren);
    return;
}

/* Imported Module Platform */

typedef struct Platform_InputState Platform_InputState;
static const int32_t Platform_DirE = 2;
static const int32_t Platform_DirSE = 3;
static const int32_t Platform_DirW = 6;
static const int32_t Platform_DirNW = 7;
static const int32_t Platform_DirS = 4;
static const int32_t Platform_DirN = 0;
static const int32_t Platform_TextH = 57;
static const int32_t Platform_DirSW = 5;
static const int32_t Platform_ScreenW = 320;
static const int32_t Platform_PlayW = 320;
static const int32_t Platform_DirNone = 8;
static const int32_t Platform_ScreenH = 200;
static const int32_t Platform_PlayH = 143;
static const int32_t Platform_DirNE = 1;
static const int32_t Platform_Scale = 3;
typedef struct Platform_InputState Platform_InputState;
struct Platform_InputState {
    int32_t dirKey;
    int attack;
    int quit;
    int usePotion;
    int useFood;
    int talk;
    int toggleMap;
    char menuKey;
    int32_t mouseX;
    int32_t mouseY;
    int mouseClick;
    int mouseMove;
};

static const int32_t Platform_FrameW = 960;
static const int32_t Platform_FrameH = 600;
static void Platform_UpdateViewport(void);
static void Platform_ToggleFullscreen(void);
static int Platform_Init(void);
static void Platform_Shutdown(void);
static int Platform_MapMouse(int32_t mx, int32_t my, int32_t *frameX, int32_t *frameY);
static void Platform_PollInput(Platform_InputState *inp);
static void Platform_BeginFrame(void);
static void Platform_EndFrame(void);
static int32_t Platform_GetTicks(void);
static void Platform_DelayMs(int32_t ms);
static void * Platform_LoadBMPTexture(char *path, uint32_t path_high);
static void * Platform_LoadBMPKeyedTexture(char *path, uint32_t path_high, int32_t kr, int32_t kg, int32_t kb);
static void * Platform_LoadBMPScaled(char *path, uint32_t path_high, int32_t dw, int32_t dh);
static void Platform_DrawTexRegion(void * tex, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, int32_t dw, int32_t dh);

int Platform_cheatKeys;
void * Platform_ren;
void * Platform_win;
void * Platform_frameTex;
int Platform_fullscreen;
int32_t Platform_viewX;
int32_t Platform_viewY;
int32_t Platform_viewW;
int32_t Platform_viewH;
static void Platform_UpdateViewport(void) {
    int32_t winW;
    int32_t winH;
#line 37 "src/Platform.mod"
    winW = Gfx_GetWindowWidth(Platform_win);
#line 38
    winH = Gfx_GetWindowHeight(Platform_win);
    if (winW <= 0) goto L2; else goto L3;
  L1:;
    if ((winW * 600) > (winH * 960)) goto L5; else goto L6;
  L2:;
#line 40
    Platform_viewX = 0;
    Platform_viewY = 0;
    Platform_viewW = 960;
    Platform_viewH = 600;
    return;
  L3:;
    if (winH <= 0) goto L2; else goto L1;
  L4:;
#line 50
    Platform_viewX = m2_div((winW - Platform_viewW), 2);
#line 51
    Platform_viewY = m2_div((winH - Platform_viewH), 2);
    return;
  L5:;
#line 44
    Platform_viewH = winH;
#line 45
    Platform_viewW = m2_div((winH * 960), 600);
    goto L4;
  L6:;
#line 47
    Platform_viewW = winW;
#line 48
    Platform_viewH = m2_div((winW * 600), 960);
    goto L4;
}

static void Platform_ToggleFullscreen(void) {
#line 56
    Platform_fullscreen = (!Platform_fullscreen);
    if (Platform_fullscreen) goto L2; else goto L3;
  L1:;
#line 62
    Gfx_UpdateLogicalSize(Platform_ren, Platform_win);
#line 63
    Platform_UpdateViewport();
    return;
  L2:;
#line 58
    Gfx_SetFullscreen(Platform_win, 2);
    goto L1;
  L3:;
#line 60
    Gfx_SetFullscreen(Platform_win, 0);
    goto L1;
}

static int Platform_Init(void) {
    int ok;
#line 69
    ok = Gfx_Init();
    if (ok) goto L1; else goto L2;
  L1:;
#line 71
    Platform_win = Gfx_CreateWindow("Faery Tale Adventure", 20, (320 * 3), (200 * 3), (1 + 2));
    if (Platform_win == NULL) goto L4; else goto L3;
  L2:;
    return 0;
  L3:;
#line 75
    Platform_ren = Gfx_CreateRenderer(Platform_win, (1 + 2));
    if (Platform_ren == NULL) goto L6; else goto L5;
  L4:;
#line 74
    Gfx_Quit();
    return 0;
  L5:;
#line 77
    Platform_frameTex = Texture_Create(Platform_ren, 960, 600);
    if (Platform_frameTex == NULL) goto L8; else goto L7;
  L6:;
#line 76
    Gfx_DestroyWindow(Platform_win);
    Gfx_Quit();
    return 0;
  L7:;
#line 81
    Platform_fullscreen = 0;
#line 82
    Platform_UpdateViewport();
    return 1;
  L8:;
#line 79
    Gfx_DestroyRenderer(Platform_ren);
    Gfx_DestroyWindow(Platform_win);
    Gfx_Quit();
    return 0;
}

static void Platform_Shutdown(void) {
#line 88
    Texture_Destroy(Platform_frameTex);
#line 89
    Gfx_DestroyRenderer(Platform_ren);
#line 90
    Gfx_DestroyWindow(Platform_win);
#line 91
    Gfx_Quit();
    return;
}

static int Platform_MapMouse(int32_t mx, int32_t my, int32_t *frameX, int32_t *frameY) {
    (void)mx;
    (void)my;
    (void)frameX;
    (void)frameY;
    if (mx >= Platform_viewX) goto L5; else goto L1;
  L1:;
    return 0;
  L2:;
#line 98
    (*frameX) = m2_div(((mx - Platform_viewX) * 960), Platform_viewW);
#line 99
    (*frameY) = m2_div(((my - Platform_viewY) * 600), Platform_viewH);
    return 1;
  L3:;
    if (my < (Platform_viewY + Platform_viewH)) goto L2; else goto L1;
  L4:;
    if (my >= Platform_viewY) goto L3; else goto L1;
  L5:;
    if (mx < (Platform_viewX + Platform_viewW)) goto L4; else goto L1;
}

static void Platform_PollInput(Platform_InputState *inp) {
    (void)inp;
    int32_t evt;
    int32_t kc;
    int32_t mx;
    int32_t my;
    int32_t buttons;
#line 108
    (*inp).menuKey = '\0';
#line 109
    (*inp).mouseClick = 0;
#line 110
    (*inp).mouseMove = 0;
    goto L1;
  L1:;
#line 112
    evt = Events_Poll();
    if (evt == 0) goto L4; else goto L3;
  L2:;
    if (Events_IsKeyPressed(82)) goto L36; else goto L38;
  L3:;
    if (evt == 1) goto L6; else goto L7;
  L4:;
    goto L2;
  L5:;
    goto L1;
  L6:;
#line 114
    (*inp).quit = 1;
    goto L5;
  L7:;
    if (evt == 2) goto L8; else goto L9;
  L8:;
#line 116
    kc = Events_KeyCode();
    if (kc == 292) goto L11; else goto L13;
  L9:;
    if (evt == 4) goto L27; else goto L28;
  L10:;
    goto L5;
  L11:;
#line 119
    Platform_ToggleFullscreen();
    goto L10;
  L12:;
    if (kc == ((int32_t)((unsigned char)('m')))) goto L15; else goto L16;
  L13:;
    if (kc == 13) goto L14; else goto L12;
  L14:;
    if (m2_mod(m2_div(Events_KeyMod(), 4), 2) == 1) goto L11; else goto L12;
  L15:;
#line 120
    (*inp).toggleMap = 1;
    goto L10;
  L16:;
    if (kc == ((int32_t)((unsigned char)('0')))) goto L17; else goto L18;
  L17:;
#line 121
    (*inp).menuKey = '0';
    goto L10;
  L18:;
    if (kc == ((int32_t)((unsigned char)('9')))) goto L19; else goto L20;
  L19:;
#line 122
    (*inp).menuKey = '9';
    goto L10;
  L20:;
    if (kc == ((int32_t)((unsigned char)('8')))) goto L21; else goto L22;
  L21:;
#line 123
    (*inp).menuKey = '8';
    goto L10;
  L22:;
    if (kc == 27) goto L23; else goto L24;
  L23:;
#line 124
    (*inp).menuKey = ((char)(27));
    goto L10;
  L24:;
    if (kc >= ((int32_t)((unsigned char)('a')))) goto L26; else goto L10;
  L25:;
#line 126
    (*inp).menuKey = toupper(((char)(kc)));
    goto L10;
  L26:;
    if (kc <= ((int32_t)((unsigned char)('z')))) goto L25; else goto L10;
  L27:;
    if (Events_MouseButton() == 1) goto L30; else goto L29;
  L28:;
    if (evt == 9) goto L34; else goto L5;
  L29:;
    goto L5;
  L30:;
#line 130
    mx = Events_MouseX();
    my = Events_MouseY();
    if (Platform_MapMouse(mx, my, &(*inp).mouseX, &(*inp).mouseY)) goto L32; else goto L31;
  L31:;
    goto L29;
  L32:;
#line 132
    (*inp).mouseClick = 1;
    goto L31;
  L33:;
#line 136
    Gfx_UpdateLogicalSize(Platform_ren, Platform_win);
#line 137
    Platform_UpdateViewport();
    goto L5;
  L34:;
    if (Events_WindowEvent() == 5) goto L33; else goto L5;
  L35:;
#line 165
    buttons = Events_GetMouseState(&mx, &my);
    if (Platform_MapMouse(mx, my, &(*inp).mouseX, &(*inp).mouseY)) goto L63; else goto L62;
  L36:;
    if (Events_IsKeyPressed(79)) goto L40; else goto L42;
  L37:;
    if (Events_IsKeyPressed(81)) goto L46; else goto L48;
  L38:;
    if (Events_IsKeyPressed(26)) goto L36; else goto L37;
  L39:;
    goto L35;
  L40:;
#line 143
    (*inp).dirKey = 1;
    goto L39;
  L41:;
    if (Events_IsKeyPressed(80)) goto L43; else goto L45;
  L42:;
    if (Events_IsKeyPressed(7)) goto L40; else goto L41;
  L43:;
#line 145
    (*inp).dirKey = 7;
    goto L39;
  L44:;
#line 147
    (*inp).dirKey = 0;
    goto L39;
  L45:;
    if (Events_IsKeyPressed(4)) goto L43; else goto L44;
  L46:;
    if (Events_IsKeyPressed(79)) goto L50; else goto L52;
  L47:;
    if (Events_IsKeyPressed(79)) goto L56; else goto L58;
  L48:;
    if (Events_IsKeyPressed(22)) goto L46; else goto L47;
  L49:;
    goto L35;
  L50:;
#line 151
    (*inp).dirKey = 3;
    goto L49;
  L51:;
    if (Events_IsKeyPressed(80)) goto L53; else goto L55;
  L52:;
    if (Events_IsKeyPressed(7)) goto L50; else goto L51;
  L53:;
#line 153
    (*inp).dirKey = 5;
    goto L49;
  L54:;
#line 155
    (*inp).dirKey = 4;
    goto L49;
  L55:;
    if (Events_IsKeyPressed(4)) goto L53; else goto L54;
  L56:;
#line 158
    (*inp).dirKey = 2;
    goto L35;
  L57:;
    if (Events_IsKeyPressed(80)) goto L59; else goto L61;
  L58:;
    if (Events_IsKeyPressed(7)) goto L56; else goto L57;
  L59:;
#line 160
    (*inp).dirKey = 6;
    goto L35;
  L60:;
#line 162
    (*inp).dirKey = 8;
    goto L35;
  L61:;
    if (Events_IsKeyPressed(4)) goto L59; else goto L60;
  L62:;
#line 170
    (*inp).attack = (Events_IsKeyPressed(44) || ((uint32_t)(((uint32_t)(buttons))) & (uint32_t)(4)) != 0);
#line 172
    (*inp).usePotion = Events_IsKeyPressed(19);
#line 173
    (*inp).useFood = Events_IsKeyPressed(9);
#line 174
    (*inp).talk = Events_IsKeyPressed(23);
    return;
  L63:;
#line 167
    (*inp).mouseMove = (((uint32_t)(((uint32_t)(buttons))) & (uint32_t)(1)) != 0 && (*inp).mouseY < (143 * 3));
    goto L62;
}

static void Platform_BeginFrame(void) {
#line 179
    Texture_SetTarget(Platform_ren, Platform_frameTex);
#line 180
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 181
    Canvas_Clear(Platform_ren);
    return;
}

static void Platform_EndFrame(void) {
#line 186
    Texture_ResetTarget(Platform_ren);
#line 187
    Platform_UpdateViewport();
#line 188
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 189
    Canvas_Clear(Platform_ren);
#line 190
    Texture_DrawRegion(Platform_ren, Platform_frameTex, 0, 0, 960, 600, Platform_viewX, Platform_viewY, Platform_viewW, Platform_viewH);
#line 192
    Gfx_Present(Platform_ren);
    return;
}

static int32_t Platform_GetTicks(void) {
    return Gfx_Ticks();
}

static void Platform_DelayMs(int32_t ms) {
    (void)ms;
#line 202
    Gfx_Delay(ms);
    return;
}

static void * Platform_LoadBMPTexture(char *path, uint32_t path_high) {
    (void)path;
    (void)path_high;
    return Texture_LoadBMP(Platform_ren, path, path_high);
}

static void * Platform_LoadBMPKeyedTexture(char *path, uint32_t path_high, int32_t kr, int32_t kg, int32_t kb) {
    (void)path;
    (void)path_high;
    (void)kr;
    (void)kg;
    (void)kb;
    return Texture_LoadBMPKeyed(Platform_ren, path, path_high, kr, kg, kb);
}

static void * Platform_LoadBMPScaled(char *path, uint32_t path_high, int32_t dw, int32_t dh) {
    (void)path;
    (void)path_high;
    (void)dw;
    (void)dh;
    void * src;
    void * scaled;
    int32_t sw;
    int32_t sh;
#line 220
    src = Texture_LoadBMP(Platform_ren, path, path_high);
    if (src == NULL) goto L2; else goto L1;
  L1:;
#line 222
    sw = Texture_Width(src);
#line 223
    sh = Texture_Height(src);
#line 225
    scaled = Texture_Create(Platform_ren, dw, dh);
    if (scaled == NULL) goto L4; else goto L3;
  L2:;
    return NULL;
  L3:;
#line 227
    Texture_SetTarget(Platform_ren, scaled);
#line 228
    Canvas_SetColor(Platform_ren, 0, 0, 0, 0);
#line 229
    Canvas_Clear(Platform_ren);
#line 231
    Texture_DrawRegion(Platform_ren, src, 0, 0, sw, sh, 0, 0, dw, dh);
#line 232
    Texture_ResetTarget(Platform_ren);
#line 233
    Texture_Destroy(src);
    return scaled;
  L4:;
    return src;
}

static void Platform_DrawTexRegion(void * tex, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
    (void)tex;
    (void)sx;
    (void)sy;
    (void)sw;
    (void)sh;
    (void)dx;
    (void)dy;
    (void)dw;
    (void)dh;
#line 241
    Texture_DrawRegion(Platform_ren, tex, sx, sy, sw, sh, dx, dy, dw, dh);
    return;
}

/* Imported Module Color */

static uint32_t Color_Pack(int32_t r, int32_t g, int32_t b);
static uint32_t Color_PackAlpha(int32_t r, int32_t g, int32_t b, int32_t a);
static int32_t Color_UnpackR(uint32_t c);
static int32_t Color_UnpackG(uint32_t c);
static int32_t Color_UnpackB(uint32_t c);
static int32_t Color_Blend(int32_t base, int32_t target, int32_t pct);

static uint32_t Color_Pack(int32_t r, int32_t g, int32_t b) {
    (void)r;
    (void)g;
    (void)b;
    return ((uint32_t)(((uint32_t)(((uint32_t)(((uint32_t)(((uint32_t)(r))) << (24))) | (uint32_t)(((uint32_t)(((uint32_t)(g))) << (16))))) | (uint32_t)(((uint32_t)(((uint32_t)(b))) << (8))))) | (uint32_t)(255));
}

static uint32_t Color_PackAlpha(int32_t r, int32_t g, int32_t b, int32_t a) {
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    return ((uint32_t)(((uint32_t)(((uint32_t)(((uint32_t)(((uint32_t)(r))) << (24))) | (uint32_t)(((uint32_t)(((uint32_t)(g))) << (16))))) | (uint32_t)(((uint32_t)(((uint32_t)(b))) << (8))))) | (uint32_t)(((uint32_t)(a))));
}

static int32_t Color_UnpackR(uint32_t c) {
    (void)c;
    return ((int32_t)(((uint32_t)(((uint32_t)(c) >> (24))) & (uint32_t)(255))));
}

static int32_t Color_UnpackG(uint32_t c) {
    (void)c;
    return ((int32_t)(((uint32_t)(((uint32_t)(c) >> (16))) & (uint32_t)(255))));
}

static int32_t Color_UnpackB(uint32_t c) {
    (void)c;
    return ((int32_t)(((uint32_t)(((uint32_t)(c) >> (8))) & (uint32_t)(255))));
}

static int32_t Color_Blend(int32_t base, int32_t target, int32_t pct) {
    (void)base;
    (void)target;
    (void)pct;
    return (base + m2_div(((target - base) * pct), 100));
}

/* Imported Module FileSystem */

typedef void * FileSystem_File;

static void FileSystem_Lookup(FileSystem_File *f, char *name, uint32_t name_high, int newFile);
static void FileSystem_Close(FileSystem_File *f);
static void FileSystem_ReadChar(FileSystem_File *f, char *ch);
static void FileSystem_WriteChar(FileSystem_File *f, char ch);

int FileSystem_Done;
int FileSystem_Done;
static void FileSystem_Lookup(FileSystem_File *f, char *name, uint32_t name_high, int newFile) {
    (void)f;
    (void)name;
    (void)name_high;
    (void)newFile;
    char mode[2 + 1];
    if (newFile) goto L2; else goto L3;
  L1:;
#line 17 "/Users/aoesterer/.mx/lib/m2stdlib/src/FileSystem.mod"
    (*f) = fopen(((void *)(name)), ((void *)&(mode)));
    if ((*f) == NULL) goto L6; else goto L4;
  L2:;
#line 13
    mode[0] = 'w';
    mode[1] = '+';
    mode[2] = ((char)(0));
    goto L1;
  L3:;
#line 15
    mode[0] = 'r';
    mode[1] = '+';
    mode[2] = ((char)(0));
    goto L1;
  L4:;
#line 22
    FileSystem_Done = (*f) != NULL;
    return;
  L5:;
#line 19
    mode[0] = 'r';
    mode[1] = ((char)(0));
#line 20
    (*f) = fopen(((void *)(name)), ((void *)&(mode)));
    goto L4;
  L6:;
    if (newFile) goto L4; else goto L5;
}

static void FileSystem_Close(FileSystem_File *f) {
    (void)f;
    if ((*f) != NULL) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 28
    fclose((*f));
#line 29
    (*f) = NULL;
    goto L1;
}

static void FileSystem_ReadChar(FileSystem_File *f, char *ch) {
    (void)f;
    (void)ch;
    int32_t c;
#line 36
    c = fgetc((*f));
    if (c == (-1)) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 38
    (*ch) = ((char)(0));
#line 39
    FileSystem_Done = 0;
    goto L1;
  L3:;
#line 41
    (*ch) = ((char)(c));
#line 42
    FileSystem_Done = 1;
    goto L1;
}

static void FileSystem_WriteChar(FileSystem_File *f, char ch) {
    (void)f;
    (void)ch;
#line 48
    fputc(((int32_t)((unsigned char)(ch))), (*f));
    return;
}

static void FileSystem_init(void) {
#line 52
    FileSystem_Done = 1;
    return;
}

/* Imported Module PixBuf */

typedef void * PixBuf_Region;

typedef void * PixBuf_PBuf;

static const int32_t PixBuf_MaxLayers = 16;
static const int32_t PixBuf_MaxFrames = 256;
static void * PixBuf_Create(int32_t w, int32_t h);
static void PixBuf_Free(void * pb);
static void PixBuf_Clear(void * pb, int32_t idx);
static int32_t PixBuf_Width(void * pb);
static int32_t PixBuf_Height(void * pb);
static void PixBuf_SetPal(void * pb, int32_t idx, int32_t r, int32_t g, int32_t b);
static void PixBuf_SetPalAlpha(void * pb, int32_t idx, int32_t r, int32_t g, int32_t b, int32_t a);
static uint32_t PixBuf_PalPacked(void * pb, int32_t idx);
static int32_t PixBuf_PalR(void * pb, int32_t idx);
static int32_t PixBuf_PalG(void * pb, int32_t idx);
static int32_t PixBuf_PalB(void * pb, int32_t idx);
static void PixBuf_SetPix(void * pb, int32_t x, int32_t y, int32_t idx);
static int32_t PixBuf_GetPix(void * pb, int32_t x, int32_t y);
static void PixBuf_PBPoint(void * ctx, int32_t x, int32_t y);
static void PixBuf_PBHLine(void * ctx, int32_t x1, int32_t x2, int32_t y);
static void PixBuf_PBLine(void * ctx, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2);
static void PixBuf_Line(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t idx);
static void PixBuf_ThickLine(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t idx, int32_t thick);
static void PixBuf_Rect(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t idx);
static void PixBuf_FillRect(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t idx);
static void PixBuf_Circle(void * pb, int32_t cx, int32_t cy, int32_t radius, int32_t idx);
static void PixBuf_FillCircle(void * pb, int32_t cx, int32_t cy, int32_t radius, int32_t idx);
static void PixBuf_Ellipse(void * pb, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t idx);
static void PixBuf_FillEllipse(void * pb, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t idx);
static void PixBuf_Triangle(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t idx);
static void PixBuf_FillTriangle(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t idx);
static void PixBuf_LinePerfect(void * pb, int32_t x0, int32_t m2_y0, int32_t x1, int32_t m2_y1, int32_t idx);
static int32_t PixBuf_NearestColor(void * pb, int32_t r, int32_t g, int32_t b, int32_t ncolors);
static void PixBuf_ReplaceColor(void * pb, int32_t oldIdx, int32_t newIdx);
static void PixBuf_FloodFill(void * pb, int32_t x, int32_t y, int32_t idx);
static void PixBuf_Gradient(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c1, int32_t c2, int horiz, int32_t ncolors);
static void PixBuf_GradientAngle(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c1, int32_t c2, int32_t angleDeg, int32_t ncolors);
static void PixBuf_PatternFill(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fg, int32_t bg, int32_t pattern);
static void PixBuf_DitherFill(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fg, int32_t bg, int32_t matrixType, int32_t threshold);
static void PixBuf_Bezier(void * pb, int32_t x1, int32_t m2_y1, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2, int32_t x2, int32_t y2, int32_t idx, int32_t steps);
static void PixBuf_FlipH(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
static void PixBuf_FlipV(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
static void PixBuf_Rotate90(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
static void PixBuf_Rotate180(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
static void PixBuf_Rotate270(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
static void PixBuf_CopyRegion(void * pb, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t dx, int32_t dy);
static void PixBuf_AntiAlias(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t ncolors);
static void PixBuf_PolyReset(void);
static void PixBuf_PolyAdd(int32_t x, int32_t y);
static int32_t PixBuf_PolyCount(void);
static int32_t PixBuf_PolyX(int32_t i);
static int32_t PixBuf_PolyY(int32_t i);
static void PixBuf_PolyDraw(void * pb, int32_t idx);
static void PixBuf_PolyFill(void * pb, int32_t idx);
static void PixBuf_CopyPal(void * src, void * dst);
static void PixBuf_LayerInit(void * pb);
static int32_t PixBuf_LayerCount(void);
static int32_t PixBuf_LayerActive(void);
static void PixBuf_LayerSetActive(int32_t idx);
static void * PixBuf_LayerGet(int32_t idx);
static void * PixBuf_LayerGetActive(void);
static int32_t PixBuf_LayerAdd(int32_t w, int32_t h);
static void PixBuf_LayerRemove(int32_t idx);
static int PixBuf_LayerVisible(int32_t idx);
static void PixBuf_LayerSetVisible(int32_t idx, int vis);
static void PixBuf_LayerMoveUp(int32_t idx);
static void PixBuf_LayerMoveDown(int32_t idx);
static void PixBuf_LayerFlatten(void * dst, int32_t transparentIdx);
static void PixBuf_StampText(void * pb, void * ren, void * font, char *text, uint32_t text_high, int32_t x, int32_t y, int32_t idx);
static void PixBuf_Render(void * ren, void * tex, void * pb);
static void * PixBuf_Save(void * pb, int32_t x, int32_t y, int32_t w, int32_t h);
static void PixBuf_Restore(void * pb, void * region, int32_t x, int32_t y);
static int32_t PixBuf_SaveW(void * region);
static int32_t PixBuf_SaveH(void * region);
static void PixBuf_FreeSave(void * region);
static void PixBuf_WriteByteF(void * *f, int32_t v);
static void PixBuf_Write16LE(void * *f, int32_t v);
static void PixBuf_Write32LE(void * *f, int32_t v);
static void PixBuf_ReadByteF(void * *f, int32_t *v);
static void PixBuf_Read16LE(void * *f, int32_t *v);
static void PixBuf_Read32LE(void * *f, int32_t *v);
static int PixBuf_SaveBMP(void * pb, char *path, uint32_t path_high);
static int PixBuf_SavePNG(void * pb, char *path, uint32_t path_high);
static void * PixBuf_LoadPNG(char *path, uint32_t path_high, int32_t ncolors);
static void * PixBuf_LoadPNGPal(char *path, uint32_t path_high, void * palSrc, int32_t ncolors);
static int PixBuf_SaveDP2(char *path, uint32_t path_high);
static int PixBuf_LoadDP2(char *path, uint32_t path_high);
static void PixBuf_WriteIntF(void * *f, int32_t n);
static void PixBuf_SkipWhite(void * *f, char *ch);
static int PixBuf_ReadIntF(void * *f, int32_t *n);
static int PixBuf_SavePal(void * pb, char *path, uint32_t path_high);
static int PixBuf_LoadPal(void * pb, char *path, uint32_t path_high);
static void PixBuf_FrameInit(void * pb);
static int32_t PixBuf_FrameCount(void);
static int32_t PixBuf_FrameCurrent(void);
static int32_t PixBuf_FrameNew(int32_t w, int32_t h);
static void PixBuf_FrameDelete(int32_t idx);
static void PixBuf_FrameSet(int32_t idx);
static void * PixBuf_FrameGet(int32_t idx);
static void * PixBuf_FrameGetCurrent(void);
static int32_t PixBuf_FrameTiming(int32_t idx);
static void PixBuf_FrameSetTiming(int32_t idx, int32_t ms);
static void * PixBuf_FrameDuplicate(int32_t idx);
static void PixBuf_RenderAlpha(void * ren, void * tex, void * pb, int32_t alpha);
static void * PixBuf_FramesToSheet(int32_t cols);
static void PixBuf_RenderHAM(void * ren, void * tex, void * pb, int32_t mode);
static void PixBuf_CopperGradient(void * ren, void * tex, void * pb, int32_t startLine, int32_t endLine, int32_t c1, int32_t c2);
static void PixBuf_WriteStr(void * *f, char *s, uint32_t s_high);
static int PixBuf_ConfigSave(char *path, uint32_t path_high, int32_t *keys, uint32_t keys_high, int32_t *vals, uint32_t vals_high, int32_t count);
static int32_t PixBuf_ConfigLoad(char *path, uint32_t path_high, int32_t *keys, uint32_t keys_high, int32_t *vals, uint32_t vals_high, int32_t maxCount);
static void PixBuf_Log(char *path, uint32_t path_high, char *msg, uint32_t msg_high);

int32_t PixBuf_polyXs[255 + 1];
int32_t PixBuf_polyYs[255 + 1];
int32_t PixBuf_polyN;
int32_t PixBuf_drawIdx;
void * PixBuf_layers[15 + 1];
int32_t PixBuf_layerVis[15 + 1];
int32_t PixBuf_layerCount;
int32_t PixBuf_layerActive;
void * PixBuf_frames[255 + 1];
int32_t PixBuf_frameTiming[255 + 1];
int32_t PixBuf_nFrames;
int32_t PixBuf_currentFrame;
static void * PixBuf_Create(int32_t w, int32_t h) {
    (void)w;
    (void)h;
    return gfx_pb_create(w, h);
}

static void PixBuf_Free(void * pb) {
    (void)pb;
#line 58 "/Users/aoesterer/.mx/lib/m2gfx/src/PixBuf.mod"
    gfx_pb_free(pb);
    return;
}

static void PixBuf_Clear(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
#line 61
    gfx_pb_clear(pb, idx);
    return;
}

static int32_t PixBuf_Width(void * pb) {
    (void)pb;
    return gfx_pb_width(pb);
}

static int32_t PixBuf_Height(void * pb) {
    (void)pb;
    return gfx_pb_height(pb);
}

static void PixBuf_SetPal(void * pb, int32_t idx, int32_t r, int32_t g, int32_t b) {
    (void)pb;
    (void)idx;
    (void)r;
    (void)g;
    (void)b;
#line 74
    gfx_pb_set_pal(pb, idx, r, g, b);
    return;
}

static void PixBuf_SetPalAlpha(void * pb, int32_t idx, int32_t r, int32_t g, int32_t b, int32_t a) {
    (void)pb;
    (void)idx;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
#line 77
    gfx_pb_set_pal_alpha(pb, idx, r, g, b, a);
    return;
}

static uint32_t PixBuf_PalPacked(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
    return ((uint32_t)(gfx_pb_pal_packed(pb, idx)));
}

static int32_t PixBuf_PalR(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
    return Color_UnpackR(PixBuf_PalPacked(pb, idx));
}

static int32_t PixBuf_PalG(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
    return Color_UnpackG(PixBuf_PalPacked(pb, idx));
}

static int32_t PixBuf_PalB(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
    return Color_UnpackB(PixBuf_PalPacked(pb, idx));
}

static void PixBuf_SetPix(void * pb, int32_t x, int32_t y, int32_t idx) {
    (void)pb;
    (void)x;
    (void)y;
    (void)idx;
#line 96
    gfx_pb_set(pb, x, y, idx);
    return;
}

static int32_t PixBuf_GetPix(void * pb, int32_t x, int32_t y) {
    (void)pb;
    (void)x;
    (void)y;
    return gfx_pb_get(pb, x, y);
}

static void PixBuf_PBPoint(void * ctx, int32_t x, int32_t y) {
    (void)ctx;
    (void)x;
    (void)y;
#line 108
    gfx_pb_set(ctx, x, y, PixBuf_drawIdx);
    return;
}

static void PixBuf_PBHLine(void * ctx, int32_t x1, int32_t x2, int32_t y) {
    (void)ctx;
    (void)x1;
    (void)x2;
    (void)y;
    int32_t left;
    int32_t w;
    if (x1 <= x2) goto L2; else goto L3;
  L1:;
#line 115
    gfx_pb_fill_row(ctx, left, y, w, PixBuf_drawIdx);
    return;
  L2:;
#line 113
    left = x1;
    w = ((x2 - x1) + 1);
    goto L1;
  L3:;
#line 114
    left = x2;
    w = ((x1 - x2) + 1);
    goto L1;
}

static void PixBuf_PBLine(void * ctx, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2) {
    (void)ctx;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
#line 119
    DrawAlgo_Line(ctx, PixBuf_PBPoint, x1, m2_y1, x2, y2);
    return;
}

static void PixBuf_Line(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t idx) {
    (void)pb;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)idx;
#line 122
    PixBuf_drawIdx = idx;
    DrawAlgo_Line(pb, PixBuf_PBPoint, x1, m2_y1, x2, y2);
    return;
}

static void PixBuf_ThickLine(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t idx, int32_t thick) {
    (void)pb;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)idx;
    (void)thick;
    int32_t dx;
    int32_t dy;
    int32_t sx;
    int32_t sy;
    int32_t err;
    int32_t i;
    int32_t r;
    int32_t cx;
    int32_t cy;
#line 129
    PixBuf_drawIdx = idx;
    if (thick <= 1) goto L2; else goto L1;
  L1:;
#line 131
    r = m2_div(thick, 2);
#line 132
    cx = x1;
    cy = m2_y1;
#line 133
    dx = (x2 - x1);
    if (dx < 0) goto L4; else goto L3;
  L2:;
#line 130
    DrawAlgo_Line(pb, PixBuf_PBPoint, x1, m2_y1, x2, y2);
    return;
  L3:;
#line 134
    dy = (y2 - m2_y1);
    if (dy < 0) goto L6; else goto L5;
  L4:;
#line 133
    dx = (-dx);
    goto L3;
  L5:;
    if (x1 < x2) goto L8; else goto L9;
  L6:;
#line 134
    dy = (-dy);
    goto L5;
  L7:;
    if (m2_y1 < y2) goto L11; else goto L12;
  L8:;
#line 135
    sx = 1;
    goto L7;
  L9:;
    sx = (-1);
    goto L7;
  L10:;
    if (dx >= dy) goto L14; else goto L15;
  L11:;
#line 136
    sy = 1;
    goto L10;
  L12:;
    sy = (-1);
    goto L10;
  L13:;
    return;
  L14:;
#line 138
    err = m2_div(dx, 2);
    i = 0;
    goto L16;
  L15:;
#line 146
    err = m2_div(dy, 2);
    i = 0;
    goto L22;
  L16:;
    if (i <= dx) goto L17; else goto L19;
  L17:;
#line 140
    DrawAlgo_FillCircle(pb, PixBuf_PBHLine, cx, cy, r);
#line 141
    err = (err - dy);
    if (err < 0) goto L21; else goto L20;
  L18:;
    i = (i + 1);
    goto L16;
  L19:;
    goto L13;
  L20:;
#line 143
    cx = (cx + sx);
    goto L18;
  L21:;
#line 142
    cy = (cy + sy);
    err = (err + dx);
    goto L20;
  L22:;
    if (i <= dy) goto L23; else goto L25;
  L23:;
#line 148
    DrawAlgo_FillCircle(pb, PixBuf_PBHLine, cx, cy, r);
#line 149
    err = (err - dx);
    if (err < 0) goto L27; else goto L26;
  L24:;
    i = (i + 1);
    goto L22;
  L25:;
    goto L13;
  L26:;
#line 151
    cy = (cy + sy);
    goto L24;
  L27:;
#line 150
    cx = (cx + sx);
    err = (err + dy);
    goto L26;
}

static void PixBuf_Rect(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t idx) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)idx;
#line 158
    PixBuf_drawIdx = idx;
#line 159
    DrawAlgo_Line(pb, PixBuf_PBPoint, x, y, ((x + w) - 1), y);
#line 160
    DrawAlgo_Line(pb, PixBuf_PBPoint, x, ((y + h) - 1), ((x + w) - 1), ((y + h) - 1));
#line 161
    DrawAlgo_Line(pb, PixBuf_PBPoint, x, y, x, ((y + h) - 1));
#line 162
    DrawAlgo_Line(pb, PixBuf_PBPoint, ((x + w) - 1), y, ((x + w) - 1), ((y + h) - 1));
    return;
}

static void PixBuf_FillRect(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t idx) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)idx;
    int32_t row;
    int32_t pw;
    int32_t ph;
    int32_t x0;
    int32_t m2_y0;
    int32_t x1;
    int32_t m2_y1;
#line 168
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
#line 169
    x0 = x;
    if (x0 < 0) goto L2; else goto L1;
  L1:;
#line 170
    m2_y0 = y;
    if (m2_y0 < 0) goto L4; else goto L3;
  L2:;
#line 169
    x0 = 0;
    goto L1;
  L3:;
#line 171
    x1 = (x + w);
    if (x1 > pw) goto L6; else goto L5;
  L4:;
#line 170
    m2_y0 = 0;
    goto L3;
  L5:;
#line 172
    m2_y1 = (y + h);
    if (m2_y1 > ph) goto L8; else goto L7;
  L6:;
#line 171
    x1 = pw;
    goto L5;
  L7:;
    row = m2_y0;
    goto L9;
  L8:;
#line 172
    m2_y1 = ph;
    goto L7;
  L9:;
    if (row <= (m2_y1 - 1)) goto L10; else goto L12;
  L10:;
#line 174
    gfx_pb_fill_row(pb, x0, row, (x1 - x0), idx);
    goto L11;
  L11:;
    row = (row + 1);
    goto L9;
  L12:;
    return;
}

static void PixBuf_Circle(void * pb, int32_t cx, int32_t cy, int32_t radius, int32_t idx) {
    (void)pb;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)idx;
#line 180
    PixBuf_drawIdx = idx;
#line 181
    DrawAlgo_Circle(pb, PixBuf_PBPoint, cx, cy, radius);
    return;
}

static void PixBuf_FillCircle(void * pb, int32_t cx, int32_t cy, int32_t radius, int32_t idx) {
    (void)pb;
    (void)cx;
    (void)cy;
    (void)radius;
    (void)idx;
#line 186
    PixBuf_drawIdx = idx;
#line 187
    DrawAlgo_FillCircle(pb, PixBuf_PBHLine, cx, cy, radius);
    return;
}

static void PixBuf_Ellipse(void * pb, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t idx) {
    (void)pb;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    (void)idx;
#line 192
    PixBuf_drawIdx = idx;
#line 193
    DrawAlgo_Ellipse(pb, PixBuf_PBPoint, cx, cy, rx, ry);
    return;
}

static void PixBuf_FillEllipse(void * pb, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t idx) {
    (void)pb;
    (void)cx;
    (void)cy;
    (void)rx;
    (void)ry;
    (void)idx;
#line 198
    PixBuf_drawIdx = idx;
#line 199
    DrawAlgo_FillEllipse(pb, PixBuf_PBHLine, cx, cy, rx, ry);
    return;
}

static void PixBuf_Triangle(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t idx) {
    (void)pb;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    (void)idx;
#line 204
    PixBuf_drawIdx = idx;
#line 205
    DrawAlgo_Triangle(pb, PixBuf_PBLine, x1, m2_y1, x2, y2, x3, y3);
    return;
}

static void PixBuf_FillTriangle(void * pb, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t idx) {
    (void)pb;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    (void)idx;
#line 210
    PixBuf_drawIdx = idx;
#line 211
    DrawAlgo_FillTriangle(pb, PixBuf_PBHLine, x1, m2_y1, x2, y2, x3, y3);
    return;
}

static void PixBuf_LinePerfect(void * pb, int32_t x0, int32_t m2_y0, int32_t x1, int32_t m2_y1, int32_t idx) {
    (void)pb;
    (void)x0;
    (void)m2_y0;
    (void)x1;
    (void)m2_y1;
    (void)idx;
    int32_t dx;
    int32_t dy;
    int32_t adx;
    int32_t ady;
    int32_t sx;
    int32_t sy;
    int32_t err;
    int32_t x;
    int32_t y;
#line 217
    dx = (x1 - x0);
    dy = (m2_y1 - m2_y0);
#line 218
    adx = dx;
    if (adx < 0) goto L2; else goto L1;
  L1:;
#line 219
    ady = dy;
    if (ady < 0) goto L4; else goto L3;
  L2:;
#line 218
    adx = (-adx);
    goto L1;
  L3:;
    if (dx > 0) goto L6; else goto L7;
  L4:;
#line 219
    ady = (-ady);
    goto L3;
  L5:;
    if (dy > 0) goto L9; else goto L10;
  L6:;
#line 220
    sx = 1;
    goto L5;
  L7:;
    sx = (-1);
    goto L5;
  L8:;
#line 222
    x = x0;
    y = m2_y0;
    if (adx >= ady) goto L12; else goto L13;
  L9:;
#line 221
    sy = 1;
    goto L8;
  L10:;
    sy = (-1);
    goto L8;
  L11:;
    return;
  L12:;
#line 224
    err = m2_div(adx, 2);
    goto L14;
  L13:;
#line 233
    err = m2_div(ady, 2);
    goto L20;
  L14:;
#line 226
    gfx_pb_set(pb, x, y, idx);
    if (x == x1) goto L17; else goto L16;
  L15:;
    goto L11;
  L16:;
#line 228
    err = (err - ady);
    if (err < 0) goto L19; else goto L18;
  L17:;
    goto L15;
  L18:;
#line 230
    x = (x + sx);
    goto L14;
  L19:;
#line 229
    y = (y + sy);
    err = (err + adx);
    goto L18;
  L20:;
#line 235
    gfx_pb_set(pb, x, y, idx);
    if (y == m2_y1) goto L23; else goto L22;
  L21:;
    goto L11;
  L22:;
#line 237
    err = (err - adx);
    if (err < 0) goto L25; else goto L24;
  L23:;
    goto L21;
  L24:;
#line 239
    y = (y + sy);
    goto L20;
  L25:;
#line 238
    x = (x + sx);
    err = (err + ady);
    goto L24;
}

static int32_t PixBuf_NearestColor(void * pb, int32_t r, int32_t g, int32_t b, int32_t ncolors) {
    (void)pb;
    (void)r;
    (void)g;
    (void)b;
    (void)ncolors;
    int32_t i;
    int32_t best;
    int32_t bestd;
    int32_t dr;
    int32_t dg;
    int32_t db;
    int32_t d;
#line 251
    best = 0;
    bestd = 2147483647;
    i = 0;
    goto L1;
  L1:;
    if (i <= (ncolors - 1)) goto L2; else goto L4;
  L2:;
#line 253
    dr = (r - PixBuf_PalR(pb, i));
#line 254
    dg = (g - PixBuf_PalG(pb, i));
#line 255
    db = (b - PixBuf_PalB(pb, i));
#line 256
    d = (((dr * dr) + (dg * dg)) + (db * db));
    if (d < bestd) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return best;
  L5:;
    goto L3;
  L6:;
#line 257
    bestd = d;
    best = i;
    goto L5;
}

static void PixBuf_ReplaceColor(void * pb, int32_t oldIdx, int32_t newIdx) {
    (void)pb;
    (void)oldIdx;
    (void)newIdx;
    int32_t pw;
    int32_t ph;
    int32_t x;
    int32_t y;
#line 265
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
    y = 0;
    goto L1;
  L1:;
    if (y <= (ph - 1)) goto L2; else goto L4;
  L2:;
    x = 0;
    goto L5;
  L3:;
    y = (y + 1);
    goto L1;
  L4:;
    return;
  L5:;
    if (x <= (pw - 1)) goto L6; else goto L8;
  L6:;
    if (PixBuf_GetPix(pb, x, y) == oldIdx) goto L10; else goto L9;
  L7:;
    x = (x + 1);
    goto L5;
  L8:;
    goto L3;
  L9:;
    goto L7;
  L10:;
#line 269
    gfx_pb_set(pb, x, y, newIdx);
    goto L9;
}

static void PixBuf_FloodFill(void * pb, int32_t x, int32_t y, int32_t idx) {
    (void)pb;
    (void)x;
    (void)y;
    (void)idx;
    int32_t stkX[8191 + 1];
    int32_t stkY[8191 + 1];
    int32_t sp;
    int32_t target;
    int32_t pw;
    int32_t ph;
    int32_t cx;
    int32_t cy;
    int32_t left;
    int32_t right;
    int32_t xi;
    int32_t ny;
    int32_t dir;
    int inSpan;
#line 281
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
    if (x < 0) goto L2; else goto L5;
  L1:;
#line 283
    target = PixBuf_GetPix(pb, x, y);
    if (target == idx) goto L7; else goto L6;
  L2:;
    return;
  L3:;
    if (y >= ph) goto L2; else goto L1;
  L4:;
    if (y < 0) goto L2; else goto L3;
  L5:;
    if (x >= pw) goto L2; else goto L4;
  L6:;
#line 285
    sp = 0;
#line 286
    stkX[sp] = x;
    stkY[sp] = y;
    (sp++);
    goto L8;
  L7:;
    return;
  L8:;
    if (sp > 0) goto L9; else goto L10;
  L9:;
#line 288
    (sp--);
    cx = stkX[sp];
    cy = stkY[sp];
    if (cx < 0) goto L12; else goto L16;
  L10:;
#line 317
    gfx_pb_mark_dirty(pb, 0, 0, pw, ph);
    return;
  L11:;
    goto L8;
  L12:;
    goto L11;
  L13:;
    if (PixBuf_GetPix(pb, cx, cy) != target) goto L17; else goto L18;
  L14:;
    if (cy >= ph) goto L12; else goto L13;
  L15:;
    if (cy < 0) goto L12; else goto L14;
  L16:;
    if (cx >= pw) goto L12; else goto L15;
  L17:;
    goto L11;
  L18:;
#line 292
    left = cx;
    goto L19;
  L19:;
    if (left > 0) goto L22; else goto L21;
  L20:;
#line 293
    (left--);
    goto L19;
  L21:;
#line 294
    right = cx;
    goto L23;
  L22:;
    if (PixBuf_GetPix(pb, (left - 1), cy) == target) goto L20; else goto L21;
  L23:;
    if (right < (pw - 1)) goto L26; else goto L25;
  L24:;
#line 295
    (right++);
    goto L23;
  L25:;
#line 296
    gfx_pb_fill_row(pb, left, cy, ((right - left) + 1), idx);
    dir = 0;
    goto L27;
  L26:;
    if (PixBuf_GetPix(pb, (right + 1), cy) == target) goto L24; else goto L25;
  L27:;
    if (dir <= 1) goto L28; else goto L30;
  L28:;
    if (dir == 0) goto L32; else goto L33;
  L29:;
    dir = (dir + 1);
    goto L27;
  L30:;
    goto L11;
  L31:;
    if (ny >= 0) goto L36; else goto L34;
  L32:;
#line 298
    ny = (cy - 1);
    goto L31;
  L33:;
    ny = (cy + 1);
    goto L31;
  L34:;
    goto L29;
  L35:;
#line 300
    inSpan = 0;
    xi = left;
    goto L37;
  L36:;
    if (ny < ph) goto L35; else goto L34;
  L37:;
    if (xi <= right) goto L38; else goto L40;
  L38:;
    if (PixBuf_GetPix(pb, xi, ny) == target) goto L42; else goto L43;
  L39:;
    xi = (xi + 1);
    goto L37;
  L40:;
    goto L34;
  L41:;
    goto L39;
  L42:;
    if (inSpan) goto L44; else goto L45;
  L43:;
#line 310
    inSpan = 0;
    goto L41;
  L44:;
    goto L41;
  L45:;
    if (sp < 8192) goto L47; else goto L46;
  L46:;
#line 307
    inSpan = 1;
    goto L44;
  L47:;
#line 305
    stkX[sp] = xi;
    stkY[sp] = ny;
    (sp++);
    goto L46;
}

static void PixBuf_Gradient(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c1, int32_t c2, int horiz, int32_t ncolors) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c1;
    (void)c2;
    (void)horiz;
    (void)ncolors;
    int32_t r1;
    int32_t g1;
    int32_t b1;
    int32_t r2;
    int32_t g2;
    int32_t b2;
    int32_t n;
    int32_t col;
    int32_t row;
    int32_t mr;
    int32_t mg;
    int32_t mb;
    int32_t ci;
    if (ncolors < 1) goto L2; else goto L1;
  L1:;
#line 326
    r1 = PixBuf_PalR(pb, m2_mod(c1, 256));
    g1 = PixBuf_PalG(pb, m2_mod(c1, 256));
    b1 = PixBuf_PalB(pb, m2_mod(c1, 256));
#line 327
    r2 = PixBuf_PalR(pb, m2_mod(c2, 256));
    g2 = PixBuf_PalG(pb, m2_mod(c2, 256));
    b2 = PixBuf_PalB(pb, m2_mod(c2, 256));
    if (horiz) goto L4; else goto L5;
  L2:;
#line 325
    ncolors = 32;
    goto L1;
  L3:;
    return;
  L4:;
#line 329
    n = (w - 1);
    if (n < 1) goto L7; else goto L6;
  L5:;
#line 340
    n = (h - 1);
    if (n < 1) goto L17; else goto L16;
  L6:;
    col = 0;
    goto L8;
  L7:;
#line 329
    n = 1;
    goto L6;
  L8:;
    if (col <= (w - 1)) goto L9; else goto L11;
  L9:;
#line 331
    mr = (r1 + m2_div(((r2 - r1) * col), n));
#line 332
    mg = (g1 + m2_div(((g2 - g1) * col), n));
#line 333
    mb = (b1 + m2_div(((b2 - b1) * col), n));
#line 334
    ci = PixBuf_NearestColor(pb, mr, mg, mb, ncolors);
    row = y;
    goto L12;
  L10:;
    col = (col + 1);
    goto L8;
  L11:;
    goto L3;
  L12:;
    if (row <= ((y + h) - 1)) goto L13; else goto L15;
  L13:;
#line 336
    gfx_pb_set(pb, (x + col), row, ci);
    goto L14;
  L14:;
    row = (row + 1);
    goto L12;
  L15:;
    goto L10;
  L16:;
    row = 0;
    goto L18;
  L17:;
#line 340
    n = 1;
    goto L16;
  L18:;
    if (row <= (h - 1)) goto L19; else goto L21;
  L19:;
#line 342
    mr = (r1 + m2_div(((r2 - r1) * row), n));
#line 343
    mg = (g1 + m2_div(((g2 - g1) * row), n));
#line 344
    mb = (b1 + m2_div(((b2 - b1) * row), n));
#line 345
    ci = PixBuf_NearestColor(pb, mr, mg, mb, ncolors);
    col = x;
    goto L22;
  L20:;
    row = (row + 1);
    goto L18;
  L21:;
    goto L3;
  L22:;
    if (col <= ((x + w) - 1)) goto L23; else goto L25;
  L23:;
#line 347
    gfx_pb_set(pb, col, (y + row), ci);
    goto L24;
  L24:;
    col = (col + 1);
    goto L22;
  L25:;
    goto L20;
}

static void PixBuf_GradientAngle(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c1, int32_t c2, int32_t angleDeg, int32_t ncolors) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c1;
    (void)c2;
    (void)angleDeg;
    (void)ncolors;
    int32_t r1;
    int32_t g1;
    int32_t b1;
    int32_t r2;
    int32_t g2;
    int32_t b2;
    float rad;
    float cosA;
    float sinA;
    float corners[3 + 1];
    float pmin;
    float pmax;
    float range;
    float proj;
    float t;
    int32_t row;
    int32_t col;
    int32_t mr;
    int32_t mg;
    int32_t mb;
    int32_t ci;
    int32_t i;
    if (w <= 0) goto L2; else goto L3;
  L1:;
    if (ncolors < 1) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (h <= 0) goto L2; else goto L1;
  L4:;
#line 363
    rad = ((double)((((float)(angleDeg)) * 3.14159265)) / (double)(180.0));
#line 364
    cosA = cosf(rad);
    sinA = sinf(rad);
#line 365
    corners[0] = 0.0;
#line 366
    corners[1] = (((float)((w - 1))) * cosA);
#line 367
    corners[2] = (((float)((h - 1))) * sinA);
#line 368
    corners[3] = ((((float)((w - 1))) * cosA) + (((float)((h - 1))) * sinA));
#line 369
    pmin = corners[0];
    pmax = corners[0];
    i = 1;
    goto L6;
  L5:;
#line 362
    ncolors = 32;
    goto L4;
  L6:;
    if (i <= 3) goto L7; else goto L9;
  L7:;
    if (corners[i] < pmin) goto L11; else goto L10;
  L8:;
    i = (i + 1);
    goto L6;
  L9:;
#line 374
    range = (pmax - pmin);
    if (range < 1.0) goto L15; else goto L14;
  L10:;
    if (corners[i] > pmax) goto L13; else goto L12;
  L11:;
#line 371
    pmin = corners[i];
    goto L10;
  L12:;
    goto L8;
  L13:;
#line 372
    pmax = corners[i];
    goto L12;
  L14:;
#line 376
    r1 = PixBuf_PalR(pb, m2_mod(c1, 256));
    g1 = PixBuf_PalG(pb, m2_mod(c1, 256));
    b1 = PixBuf_PalB(pb, m2_mod(c1, 256));
#line 377
    r2 = PixBuf_PalR(pb, m2_mod(c2, 256));
    g2 = PixBuf_PalG(pb, m2_mod(c2, 256));
    b2 = PixBuf_PalB(pb, m2_mod(c2, 256));
    row = 0;
    goto L16;
  L15:;
#line 375
    range = 1.0;
    goto L14;
  L16:;
    if (row <= (h - 1)) goto L17; else goto L19;
  L17:;
    col = 0;
    goto L20;
  L18:;
    row = (row + 1);
    goto L16;
  L19:;
    return;
  L20:;
    if (col <= (w - 1)) goto L21; else goto L23;
  L21:;
#line 380
    proj = ((((float)(col)) * cosA) + (((float)(row)) * sinA));
#line 381
    t = ((double)((proj - pmin)) / (double)(range));
    if (t < 0.0) goto L25; else goto L24;
  L22:;
    col = (col + 1);
    goto L20;
  L23:;
    goto L18;
  L24:;
    if (t > 1.0) goto L27; else goto L26;
  L25:;
#line 382
    t = 0.0;
    goto L24;
  L26:;
#line 384
    mr = ((int32_t)((((float)(r1)) + (((float)((r2 - r1))) * t))));
#line 385
    mg = ((int32_t)((((float)(g1)) + (((float)((g2 - g1))) * t))));
#line 386
    mb = ((int32_t)((((float)(b1)) + (((float)((b2 - b1))) * t))));
#line 387
    ci = PixBuf_NearestColor(pb, mr, mg, mb, ncolors);
#line 388
    gfx_pb_set(pb, (x + col), (y + row), ci);
    goto L22;
  L27:;
#line 383
    t = 1.0;
    goto L26;
}

static void PixBuf_PatternFill(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fg, int32_t bg, int32_t pattern) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)fg;
    (void)bg;
    (void)pattern;
    static const int32_t B00 = 0;
    static const int32_t B01 = 8;
    static const int32_t B02 = 2;
    static const int32_t B03 = 10;
    static const int32_t B10 = 12;
    static const int32_t B11 = 4;
    static const int32_t B12 = 14;
    static const int32_t B13 = 6;
    static const int32_t B20 = 3;
    static const int32_t B21 = 11;
    static const int32_t B22 = 1;
    static const int32_t B23 = 9;
    static const int32_t B30 = 15;
    static const int32_t B31 = 7;
    static const int32_t B32 = 13;
    static const int32_t B33 = 5;
    int32_t row;
    int32_t col;
    int32_t thr;
    int32_t val;
    int32_t ci;
    int32_t bayer4[3 + 1][3 + 1];
#line 402
    bayer4[0][0] = 0;
    bayer4[0][1] = 8;
    bayer4[0][2] = 2;
    bayer4[0][3] = 10;
#line 403
    bayer4[1][0] = 12;
    bayer4[1][1] = 4;
    bayer4[1][2] = 14;
    bayer4[1][3] = 6;
#line 404
    bayer4[2][0] = 3;
    bayer4[2][1] = 11;
    bayer4[2][2] = 1;
    bayer4[2][3] = 9;
#line 405
    bayer4[3][0] = 15;
    bayer4[3][1] = 7;
    bayer4[3][2] = 13;
    bayer4[3][3] = 5;
#line 406
    thr = pattern;
    if (thr < 0) goto L2; else goto L1;
  L1:;
    if (thr > 16) goto L4; else goto L3;
  L2:;
#line 407
    thr = 0;
    goto L1;
  L3:;
    row = y;
    goto L5;
  L4:;
#line 408
    thr = 16;
    goto L3;
  L5:;
    if (row <= ((y + h) - 1)) goto L6; else goto L8;
  L6:;
    col = x;
    goto L9;
  L7:;
    row = (row + 1);
    goto L5;
  L8:;
    return;
  L9:;
    if (col <= ((x + w) - 1)) goto L10; else goto L12;
  L10:;
#line 411
    val = bayer4[(row % 4)][(col % 4)];
    if (val < thr) goto L14; else goto L15;
  L11:;
    col = (col + 1);
    goto L9;
  L12:;
    goto L7;
  L13:;
#line 413
    gfx_pb_set(pb, col, row, ci);
    goto L11;
  L14:;
#line 412
    ci = fg;
    goto L13;
  L15:;
    ci = bg;
    goto L13;
}

static void PixBuf_DitherFill(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fg, int32_t bg, int32_t matrixType, int32_t threshold) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)fg;
    (void)bg;
    (void)matrixType;
    (void)threshold;
    int32_t row;
    int32_t col;
    int32_t val;
    int32_t ci;
    int32_t thr;
    int32_t bayer2[1 + 1][1 + 1];
    int32_t bayer4[3 + 1][3 + 1];
    int32_t bayer8[7 + 1][7 + 1];
#line 425
    bayer2[0][0] = 0;
    bayer2[0][1] = 2;
#line 426
    bayer2[1][0] = 3;
    bayer2[1][1] = 1;
#line 428
    bayer4[0][0] = 0;
    bayer4[0][1] = 8;
    bayer4[0][2] = 2;
    bayer4[0][3] = 10;
#line 429
    bayer4[1][0] = 12;
    bayer4[1][1] = 4;
    bayer4[1][2] = 14;
    bayer4[1][3] = 6;
#line 430
    bayer4[2][0] = 3;
    bayer4[2][1] = 11;
    bayer4[2][2] = 1;
    bayer4[2][3] = 9;
#line 431
    bayer4[3][0] = 15;
    bayer4[3][1] = 7;
    bayer4[3][2] = 13;
    bayer4[3][3] = 5;
#line 433
    bayer8[0][0] = 0;
    bayer8[0][1] = 32;
    bayer8[0][2] = 8;
    bayer8[0][3] = 40;
#line 434
    bayer8[0][4] = 2;
    bayer8[0][5] = 34;
    bayer8[0][6] = 10;
    bayer8[0][7] = 42;
#line 435
    bayer8[1][0] = 48;
    bayer8[1][1] = 16;
    bayer8[1][2] = 56;
    bayer8[1][3] = 24;
#line 436
    bayer8[1][4] = 50;
    bayer8[1][5] = 18;
    bayer8[1][6] = 58;
    bayer8[1][7] = 26;
#line 437
    bayer8[2][0] = 12;
    bayer8[2][1] = 44;
    bayer8[2][2] = 4;
    bayer8[2][3] = 36;
#line 438
    bayer8[2][4] = 14;
    bayer8[2][5] = 46;
    bayer8[2][6] = 6;
    bayer8[2][7] = 38;
#line 439
    bayer8[3][0] = 60;
    bayer8[3][1] = 28;
    bayer8[3][2] = 52;
    bayer8[3][3] = 20;
#line 440
    bayer8[3][4] = 62;
    bayer8[3][5] = 30;
    bayer8[3][6] = 54;
    bayer8[3][7] = 22;
#line 441
    bayer8[4][0] = 3;
    bayer8[4][1] = 35;
    bayer8[4][2] = 11;
    bayer8[4][3] = 43;
#line 442
    bayer8[4][4] = 1;
    bayer8[4][5] = 33;
    bayer8[4][6] = 9;
    bayer8[4][7] = 41;
#line 443
    bayer8[5][0] = 51;
    bayer8[5][1] = 19;
    bayer8[5][2] = 59;
    bayer8[5][3] = 27;
#line 444
    bayer8[5][4] = 49;
    bayer8[5][5] = 17;
    bayer8[5][6] = 57;
    bayer8[5][7] = 25;
#line 445
    bayer8[6][0] = 15;
    bayer8[6][1] = 47;
    bayer8[6][2] = 7;
    bayer8[6][3] = 39;
#line 446
    bayer8[6][4] = 13;
    bayer8[6][5] = 45;
    bayer8[6][6] = 5;
    bayer8[6][7] = 37;
#line 447
    bayer8[7][0] = 63;
    bayer8[7][1] = 31;
    bayer8[7][2] = 55;
    bayer8[7][3] = 23;
#line 448
    bayer8[7][4] = 61;
    bayer8[7][5] = 29;
    bayer8[7][6] = 53;
    bayer8[7][7] = 21;
#line 450
    thr = threshold;
    row = y;
    goto L1;
  L1:;
    if (row <= ((y + h) - 1)) goto L2; else goto L4;
  L2:;
    col = x;
    goto L5;
  L3:;
    row = (row + 1);
    goto L1;
  L4:;
    return;
  L5:;
    if (col <= ((x + w) - 1)) goto L6; else goto L8;
  L6:;
    if (matrixType == 0) goto L10; else goto L11;
  L7:;
    col = (col + 1);
    goto L5;
  L8:;
    goto L3;
  L9:;
    if (val < thr) goto L27; else goto L28;
  L10:;
#line 454
    val = bayer2[(row % 2)][(col % 2)];
    if (thr < 0) goto L13; else goto L12;
  L11:;
    if (matrixType == 2) goto L16; else goto L17;
  L12:;
    if (thr > 4) goto L15; else goto L14;
  L13:;
#line 455
    thr = 0;
    goto L12;
  L14:;
    goto L9;
  L15:;
#line 456
    thr = 4;
    goto L14;
  L16:;
#line 458
    val = bayer8[(row % 8)][(col % 8)];
    if (thr < 0) goto L19; else goto L18;
  L17:;
#line 462
    val = bayer4[(row % 4)][(col % 4)];
    if (thr < 0) goto L23; else goto L22;
  L18:;
    if (thr > 64) goto L21; else goto L20;
  L19:;
#line 459
    thr = 0;
    goto L18;
  L20:;
    goto L9;
  L21:;
#line 460
    thr = 64;
    goto L20;
  L22:;
    if (thr > 16) goto L25; else goto L24;
  L23:;
#line 463
    thr = 0;
    goto L22;
  L24:;
    goto L9;
  L25:;
#line 464
    thr = 16;
    goto L24;
  L26:;
#line 467
    gfx_pb_set(pb, col, row, ci);
    goto L7;
  L27:;
#line 466
    ci = fg;
    goto L26;
  L28:;
    ci = bg;
    goto L26;
}

static void PixBuf_Bezier(void * pb, int32_t x1, int32_t m2_y1, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2, int32_t x2, int32_t y2, int32_t idx, int32_t steps) {
    (void)pb;
    (void)x1;
    (void)m2_y1;
    (void)cx1;
    (void)cy1;
    (void)cx2;
    (void)cy2;
    (void)x2;
    (void)y2;
    (void)idx;
    (void)steps;
    int32_t st;
#line 476
    st = steps;
    if (st < 4) goto L2; else goto L1;
  L1:;
#line 478
    PixBuf_drawIdx = idx;
#line 479
    DrawAlgo_Bezier(pb, PixBuf_PBLine, x1, m2_y1, cx1, cy1, cx2, cy2, x2, y2, st);
    return;
  L2:;
#line 477
    st = 32;
    goto L1;
}

static void PixBuf_FlipH(void * pb, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    int32_t row;
    int32_t c;
    int32_t lx;
    int32_t rx;
    int32_t t;
    int32_t pw;
    int32_t ph;
#line 485
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
    row = y;
    goto L1;
  L1:;
    if (row <= ((y + h) - 1)) goto L2; else goto L4;
  L2:;
    if (row >= 0) goto L7; else goto L5;
  L3:;
    row = (row + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
    c = 0;
    goto L8;
  L7:;
    if (row < ph) goto L6; else goto L5;
  L8:;
    if (c <= (m2_div(w, 2) - 1)) goto L9; else goto L11;
  L9:;
#line 489
    lx = (x + c);
    rx = (((x + w) - 1) - c);
    if (lx >= 0) goto L16; else goto L12;
  L10:;
    c = (c + 1);
    goto L8;
  L11:;
    goto L5;
  L12:;
    goto L10;
  L13:;
#line 491
    t = PixBuf_GetPix(pb, lx, row);
#line 492
    gfx_pb_set(pb, lx, row, PixBuf_GetPix(pb, rx, row));
#line 493
    gfx_pb_set(pb, rx, row, t);
    goto L12;
  L14:;
    if (rx < pw) goto L13; else goto L12;
  L15:;
    if (rx >= 0) goto L14; else goto L12;
  L16:;
    if (lx < pw) goto L15; else goto L12;
}

static void PixBuf_FlipV(void * pb, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    int32_t r;
    int32_t c;
    int32_t topY;
    int32_t botY;
    int32_t t;
    int32_t pw;
    int32_t ph;
#line 503
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
    r = 0;
    goto L1;
  L1:;
    if (r <= (m2_div(h, 2) - 1)) goto L2; else goto L4;
  L2:;
#line 505
    topY = (y + r);
    botY = (((y + h) - 1) - r);
    if (topY >= 0) goto L9; else goto L5;
  L3:;
    r = (r + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
    c = x;
    goto L10;
  L7:;
    if (botY < ph) goto L6; else goto L5;
  L8:;
    if (botY >= 0) goto L7; else goto L5;
  L9:;
    if (topY < ph) goto L8; else goto L5;
  L10:;
    if (c <= ((x + w) - 1)) goto L11; else goto L13;
  L11:;
    if (c >= 0) goto L16; else goto L14;
  L12:;
    c = (c + 1);
    goto L10;
  L13:;
    goto L5;
  L14:;
    goto L12;
  L15:;
#line 509
    t = PixBuf_GetPix(pb, c, topY);
#line 510
    gfx_pb_set(pb, c, topY, PixBuf_GetPix(pb, c, botY));
#line 511
    gfx_pb_set(pb, c, botY, t);
    goto L14;
  L16:;
    if (c < pw) goto L15; else goto L14;
}

static void PixBuf_Rotate90(void * pb, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    void * buf;
    int32_t r;
    int32_t c;
    int32_t sx;
    int32_t sy;
    int32_t dx;
    int32_t dy;
    int32_t pw;
    int32_t ph;
    int32_t m;
    if (w <= 0) goto L2; else goto L3;
  L1:;
#line 527
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
#line 528
    buf = gfx_alloc((w * h));
    if (buf == NULL) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (h <= 0) goto L2; else goto L1;
  L4:;
    r = 0;
    goto L6;
  L5:;
    return;
  L6:;
    if (r <= (h - 1)) goto L7; else goto L9;
  L7:;
    c = 0;
    goto L10;
  L8:;
    r = (r + 1);
    goto L6;
  L9:;
    r = 0;
    goto L20;
  L10:;
    if (c <= (w - 1)) goto L11; else goto L13;
  L11:;
#line 532
    sx = (x + c);
    sy = (y + r);
    if (sx >= 0) goto L19; else goto L16;
  L12:;
    c = (c + 1);
    goto L10;
  L13:;
    goto L8;
  L14:;
    goto L12;
  L15:;
#line 534
    gfx_buf_set(buf, ((r * w) + c), PixBuf_GetPix(pb, sx, sy));
    goto L14;
  L16:;
#line 536
    gfx_buf_set(buf, ((r * w) + c), 0);
    goto L14;
  L17:;
    if (sy < ph) goto L15; else goto L16;
  L18:;
    if (sy >= 0) goto L17; else goto L16;
  L19:;
    if (sx < pw) goto L18; else goto L16;
  L20:;
    if (r <= (h - 1)) goto L21; else goto L23;
  L21:;
    c = 0;
    goto L24;
  L22:;
    r = (r + 1);
    goto L20;
  L23:;
#line 549
    gfx_dealloc(buf);
#line 550
    m = w;
    if (h > m) goto L34; else goto L33;
  L24:;
    if (c <= (w - 1)) goto L25; else goto L27;
  L25:;
#line 543
    dx = (x + ((h - 1) - r));
    dy = (y + c);
    if (dx >= 0) goto L32; else goto L28;
  L26:;
    c = (c + 1);
    goto L24;
  L27:;
    goto L22;
  L28:;
    goto L26;
  L29:;
#line 545
    gfx_pb_set(pb, dx, dy, gfx_buf_get(buf, ((r * w) + c)));
    goto L28;
  L30:;
    if (dy < ph) goto L29; else goto L28;
  L31:;
    if (dy >= 0) goto L30; else goto L28;
  L32:;
    if (dx < pw) goto L31; else goto L28;
  L33:;
#line 551
    gfx_pb_mark_dirty(pb, x, y, m, m);
    return;
  L34:;
#line 550
    m = h;
    goto L33;
}

static void PixBuf_Rotate180(void * pb, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    int32_t r;
    int32_t c;
    int32_t dx;
    int32_t dy;
    int32_t pw;
    int32_t ph;
    int32_t t;
    void * buf;
    if (w <= 0) goto L2; else goto L3;
  L1:;
#line 559
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
#line 560
    buf = gfx_alloc((w * h));
    if (buf == NULL) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (h <= 0) goto L2; else goto L1;
  L4:;
    r = 0;
    goto L6;
  L5:;
    return;
  L6:;
    if (r <= (h - 1)) goto L7; else goto L9;
  L7:;
    c = 0;
    goto L10;
  L8:;
    r = (r + 1);
    goto L6;
  L9:;
    r = 0;
    goto L20;
  L10:;
    if (c <= (w - 1)) goto L11; else goto L13;
  L11:;
    if ((x + c) >= 0) goto L19; else goto L16;
  L12:;
    c = (c + 1);
    goto L10;
  L13:;
    goto L8;
  L14:;
    goto L12;
  L15:;
#line 565
    gfx_buf_set(buf, ((r * w) + c), PixBuf_GetPix(pb, (x + c), (y + r)));
    goto L14;
  L16:;
#line 567
    gfx_buf_set(buf, ((r * w) + c), 0);
    goto L14;
  L17:;
    if ((y + r) < ph) goto L15; else goto L16;
  L18:;
    if ((y + r) >= 0) goto L17; else goto L16;
  L19:;
    if ((x + c) < pw) goto L18; else goto L16;
  L20:;
    if (r <= (h - 1)) goto L21; else goto L23;
  L21:;
    c = 0;
    goto L24;
  L22:;
    r = (r + 1);
    goto L20;
  L23:;
#line 579
    gfx_dealloc(buf);
#line 580
    gfx_pb_mark_dirty(pb, x, y, w, h);
    return;
  L24:;
    if (c <= (w - 1)) goto L25; else goto L27;
  L25:;
#line 573
    dx = (x + ((w - 1) - c));
    dy = (y + ((h - 1) - r));
    if (dx >= 0) goto L32; else goto L28;
  L26:;
    c = (c + 1);
    goto L24;
  L27:;
    goto L22;
  L28:;
    goto L26;
  L29:;
#line 575
    gfx_pb_set(pb, dx, dy, gfx_buf_get(buf, ((r * w) + c)));
    goto L28;
  L30:;
    if (dy < ph) goto L29; else goto L28;
  L31:;
    if (dy >= 0) goto L30; else goto L28;
  L32:;
    if (dx < pw) goto L31; else goto L28;
}

static void PixBuf_Rotate270(void * pb, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    void * buf;
    int32_t r;
    int32_t c;
    int32_t sx;
    int32_t sy;
    int32_t dx;
    int32_t dy;
    int32_t pw;
    int32_t ph;
    int32_t m;
    if (w <= 0) goto L2; else goto L3;
  L1:;
#line 588
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
#line 589
    buf = gfx_alloc((w * h));
    if (buf == NULL) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (h <= 0) goto L2; else goto L1;
  L4:;
    r = 0;
    goto L6;
  L5:;
    return;
  L6:;
    if (r <= (h - 1)) goto L7; else goto L9;
  L7:;
    c = 0;
    goto L10;
  L8:;
    r = (r + 1);
    goto L6;
  L9:;
    r = 0;
    goto L20;
  L10:;
    if (c <= (w - 1)) goto L11; else goto L13;
  L11:;
#line 593
    sx = (x + c);
    sy = (y + r);
    if (sx >= 0) goto L19; else goto L16;
  L12:;
    c = (c + 1);
    goto L10;
  L13:;
    goto L8;
  L14:;
    goto L12;
  L15:;
#line 595
    gfx_buf_set(buf, ((r * w) + c), PixBuf_GetPix(pb, sx, sy));
    goto L14;
  L16:;
#line 597
    gfx_buf_set(buf, ((r * w) + c), 0);
    goto L14;
  L17:;
    if (sy < ph) goto L15; else goto L16;
  L18:;
    if (sy >= 0) goto L17; else goto L16;
  L19:;
    if (sx < pw) goto L18; else goto L16;
  L20:;
    if (r <= (h - 1)) goto L21; else goto L23;
  L21:;
    c = 0;
    goto L24;
  L22:;
    r = (r + 1);
    goto L20;
  L23:;
#line 610
    gfx_dealloc(buf);
#line 611
    m = w;
    if (h > m) goto L34; else goto L33;
  L24:;
    if (c <= (w - 1)) goto L25; else goto L27;
  L25:;
#line 604
    dx = (x + r);
    dy = (y + ((w - 1) - c));
    if (dx >= 0) goto L32; else goto L28;
  L26:;
    c = (c + 1);
    goto L24;
  L27:;
    goto L22;
  L28:;
    goto L26;
  L29:;
#line 606
    gfx_pb_set(pb, dx, dy, gfx_buf_get(buf, ((r * w) + c)));
    goto L28;
  L30:;
    if (dy < ph) goto L29; else goto L28;
  L31:;
    if (dy >= 0) goto L30; else goto L28;
  L32:;
    if (dx < pw) goto L31; else goto L28;
  L33:;
#line 612
    gfx_pb_mark_dirty(pb, x, y, m, m);
    return;
  L34:;
#line 611
    m = h;
    goto L33;
}

static void PixBuf_CopyRegion(void * pb, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t dx, int32_t dy) {
    (void)pb;
    (void)sx;
    (void)sy;
    (void)w;
    (void)h;
    (void)dx;
    (void)dy;
    void * buf;
    int32_t r;
    int32_t c;
    int32_t rx;
    int32_t ry;
    int32_t cx;
    int32_t pw;
    int32_t ph;
    if (w <= 0) goto L2; else goto L3;
  L1:;
#line 620
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
#line 621
    buf = gfx_alloc((w * h));
    if (buf == NULL) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (h <= 0) goto L2; else goto L1;
  L4:;
    r = 0;
    goto L6;
  L5:;
    return;
  L6:;
    if (r <= (h - 1)) goto L7; else goto L9;
  L7:;
#line 624
    ry = (sy + r);
    c = 0;
    goto L10;
  L8:;
    r = (r + 1);
    goto L6;
  L9:;
    r = 0;
    goto L20;
  L10:;
    if (c <= (w - 1)) goto L11; else goto L13;
  L11:;
#line 626
    cx = (sx + c);
    if (cx >= 0) goto L19; else goto L16;
  L12:;
    c = (c + 1);
    goto L10;
  L13:;
    goto L8;
  L14:;
    goto L12;
  L15:;
#line 628
    gfx_buf_set(buf, ((r * w) + c), PixBuf_GetPix(pb, cx, ry));
    goto L14;
  L16:;
#line 630
    gfx_buf_set(buf, ((r * w) + c), 0);
    goto L14;
  L17:;
    if (ry < ph) goto L15; else goto L16;
  L18:;
    if (ry >= 0) goto L17; else goto L16;
  L19:;
    if (cx < pw) goto L18; else goto L16;
  L20:;
    if (r <= (h - 1)) goto L21; else goto L23;
  L21:;
#line 635
    ry = (dy + r);
    if (ry >= 0) goto L26; else goto L24;
  L22:;
    r = (r + 1);
    goto L20;
  L23:;
#line 645
    gfx_dealloc(buf);
#line 646
    gfx_pb_mark_dirty(pb, dx, dy, w, h);
    return;
  L24:;
    goto L22;
  L25:;
    c = 0;
    goto L27;
  L26:;
    if (ry < ph) goto L25; else goto L24;
  L27:;
    if (c <= (w - 1)) goto L28; else goto L30;
  L28:;
#line 638
    cx = (dx + c);
    if (cx >= 0) goto L33; else goto L31;
  L29:;
    c = (c + 1);
    goto L27;
  L30:;
    goto L24;
  L31:;
    goto L29;
  L32:;
#line 640
    gfx_pb_set(pb, cx, ry, gfx_buf_get(buf, ((r * w) + c)));
    goto L31;
  L33:;
    if (cx < pw) goto L32; else goto L31;
}

static void PixBuf_AntiAlias(void * pb, int32_t x, int32_t y, int32_t w, int32_t h, int32_t ncolors) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ncolors;
    void * buf;
    int32_t r;
    int32_t c;
    int32_t sx;
    int32_t sy;
    int32_t pw;
    int32_t ph;
    int32_t center;
    int32_t diff;
    int32_t tr;
    int32_t tg;
    int32_t tb;
    int32_t cnt;
    int32_t ci;
    int32_t dr;
    int32_t dc;
    int32_t pi;
    if (w <= 0) goto L2; else goto L3;
  L1:;
#line 656
    pw = PixBuf_Width(pb);
    ph = PixBuf_Height(pb);
    if (ncolors < 1) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (h <= 0) goto L2; else goto L1;
  L4:;
#line 658
    buf = gfx_alloc((w * h));
    if (buf == NULL) goto L7; else goto L6;
  L5:;
#line 657
    ncolors = 32;
    goto L4;
  L6:;
    r = 0;
    goto L8;
  L7:;
    return;
  L8:;
    if (r <= (h - 1)) goto L9; else goto L11;
  L9:;
    c = 0;
    goto L12;
  L10:;
    r = (r + 1);
    goto L8;
  L11:;
    r = 1;
    goto L22;
  L12:;
    if (c <= (w - 1)) goto L13; else goto L15;
  L13:;
#line 662
    sx = (x + c);
    sy = (y + r);
    if (sx >= 0) goto L21; else goto L18;
  L14:;
    c = (c + 1);
    goto L12;
  L15:;
    goto L10;
  L16:;
    goto L14;
  L17:;
#line 664
    gfx_buf_set(buf, ((r * w) + c), PixBuf_GetPix(pb, sx, sy));
    goto L16;
  L18:;
#line 666
    gfx_buf_set(buf, ((r * w) + c), 0);
    goto L16;
  L19:;
    if (sy < ph) goto L17; else goto L18;
  L20:;
    if (sy >= 0) goto L19; else goto L18;
  L21:;
    if (sx < pw) goto L20; else goto L18;
  L22:;
    if (r <= (h - 2)) goto L23; else goto L25;
  L23:;
    c = 1;
    goto L26;
  L24:;
    r = (r + 1);
    goto L22;
  L25:;
#line 694
    gfx_dealloc(buf);
    return;
  L26:;
    if (c <= (w - 2)) goto L27; else goto L29;
  L27:;
#line 672
    center = gfx_buf_get(buf, ((r * w) + c));
#line 673
    diff = 0;
    if (gfx_buf_get(buf, (((r - 1) * w) + c)) != center) goto L31; else goto L30;
  L28:;
    c = (c + 1);
    goto L26;
  L29:;
    goto L24;
  L30:;
    if (gfx_buf_get(buf, (((r + 1) * w) + c)) != center) goto L33; else goto L32;
  L31:;
#line 674
    (diff++);
    goto L30;
  L32:;
    if (gfx_buf_get(buf, (((r * w) + c) - 1)) != center) goto L35; else goto L34;
  L33:;
#line 675
    (diff++);
    goto L32;
  L34:;
    if (gfx_buf_get(buf, (((r * w) + c) + 1)) != center) goto L37; else goto L36;
  L35:;
#line 676
    (diff++);
    goto L34;
  L36:;
    if (diff >= 2) goto L39; else goto L38;
  L37:;
#line 677
    (diff++);
    goto L36;
  L38:;
    goto L28;
  L39:;
#line 679
    tr = 0;
    tg = 0;
    tb = 0;
    cnt = 0;
    dr = (-1);
    goto L40;
  L40:;
    if (dr <= 1) goto L41; else goto L43;
  L41:;
    dc = (-1);
    goto L44;
  L42:;
    dr = (dr + 1);
    goto L40;
  L43:;
#line 689
    ci = PixBuf_NearestColor(pb, m2_div(tr, cnt), m2_div(tg, cnt), m2_div(tb, cnt), ncolors);
#line 690
    gfx_pb_set(pb, (x + c), (y + r), ci);
    goto L38;
  L44:;
    if (dc <= 1) goto L45; else goto L47;
  L45:;
#line 682
    pi = gfx_buf_get(buf, (((r + dr) * w) + (c + dc)));
#line 683
    tr = (tr + PixBuf_PalR(pb, pi));
#line 684
    tg = (tg + PixBuf_PalG(pb, pi));
#line 685
    tb = (tb + PixBuf_PalB(pb, pi));
#line 686
    (cnt++);
    goto L46;
  L46:;
    dc = (dc + 1);
    goto L44;
  L47:;
    goto L42;
}

static void PixBuf_PolyReset(void) {
#line 702
    PixBuf_polyN = 0;
    return;
}

static void PixBuf_PolyAdd(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    if (PixBuf_polyN < 256) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 707
    PixBuf_polyXs[PixBuf_polyN] = x;
    PixBuf_polyYs[PixBuf_polyN] = y;
#line 708
    (PixBuf_polyN++);
    goto L1;
}

static int32_t PixBuf_PolyCount(void) {
    return PixBuf_polyN;
}

static int32_t PixBuf_PolyX(int32_t i) {
    (void)i;
    if (i >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_polyXs[i];
  L3:;
    return 0;
  L4:;
    if (i < PixBuf_polyN) goto L2; else goto L3;
}

static int32_t PixBuf_PolyY(int32_t i) {
    (void)i;
    if (i >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_polyYs[i];
  L3:;
    return 0;
  L4:;
    if (i < PixBuf_polyN) goto L2; else goto L3;
}

static void PixBuf_PolyDraw(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
    int32_t i;
    if (PixBuf_polyN < 2) goto L2; else goto L1;
  L1:;
#line 731
    PixBuf_drawIdx = idx;
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= (PixBuf_polyN - 2)) goto L4; else goto L6;
  L4:;
#line 733
    DrawAlgo_Line(pb, PixBuf_PBPoint, PixBuf_polyXs[i], PixBuf_polyYs[i], PixBuf_polyXs[(i + 1)], PixBuf_polyYs[(i + 1)]);
    goto L5;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 735
    DrawAlgo_Line(pb, PixBuf_PBPoint, PixBuf_polyXs[(PixBuf_polyN - 1)], PixBuf_polyYs[(PixBuf_polyN - 1)], PixBuf_polyXs[0], PixBuf_polyYs[0]);
    return;
}

static void PixBuf_PolyFill(void * pb, int32_t idx) {
    (void)pb;
    (void)idx;
    int32_t nx[255 + 1];
    int32_t ymin;
    int32_t ymax;
    int32_t sy;
    int32_t nodes;
    int32_t i;
    int32_t j;
    int32_t k;
    int32_t c;
    int32_t t;
    if (PixBuf_polyN < 3) goto L2; else goto L1;
  L1:;
#line 744
    ymin = PixBuf_polyYs[0];
    ymax = PixBuf_polyYs[0];
    i = 1;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= (PixBuf_polyN - 1)) goto L4; else goto L6;
  L4:;
    if (PixBuf_polyYs[i] < ymin) goto L8; else goto L7;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
    sy = ymin;
    goto L11;
  L7:;
    if (PixBuf_polyYs[i] > ymax) goto L10; else goto L9;
  L8:;
#line 746
    ymin = PixBuf_polyYs[i];
    goto L7;
  L9:;
    goto L5;
  L10:;
#line 747
    ymax = PixBuf_polyYs[i];
    goto L9;
  L11:;
    if (sy <= ymax) goto L12; else goto L14;
  L12:;
#line 750
    nodes = 0;
    j = (PixBuf_polyN - 1);
    i = 0;
    goto L15;
  L13:;
    sy = (sy + 1);
    goto L11;
  L14:;
    return;
  L15:;
    if (i <= (PixBuf_polyN - 1)) goto L16; else goto L18;
  L16:;
    if (PixBuf_polyYs[i] < sy) goto L22; else goto L21;
  L17:;
    i = (i + 1);
    goto L15;
  L18:;
    i = 0;
    goto L26;
  L19:;
#line 761
    j = i;
    goto L17;
  L20:;
    if (nodes < 256) goto L25; else goto L24;
  L21:;
    if (PixBuf_polyYs[j] < sy) goto L23; else goto L19;
  L22:;
    if (PixBuf_polyYs[j] >= sy) goto L20; else goto L21;
  L23:;
    if (PixBuf_polyYs[i] >= sy) goto L20; else goto L19;
  L24:;
    goto L19;
  L25:;
#line 755
    nx[nodes] = (PixBuf_polyXs[i] + m2_div(((sy - PixBuf_polyYs[i]) * (PixBuf_polyXs[j] - PixBuf_polyXs[i])), (PixBuf_polyYs[j] - PixBuf_polyYs[i])));
#line 758
    (nodes++);
    goto L24;
  L26:;
    if (i <= (nodes - 2)) goto L27; else goto L29;
  L27:;
    k = (i + 1);
    goto L30;
  L28:;
    i = (i + 1);
    goto L26;
  L29:;
#line 770
    i = 0;
    goto L36;
  L30:;
    if (k <= (nodes - 1)) goto L31; else goto L33;
  L31:;
    if (nx[k] < nx[i]) goto L35; else goto L34;
  L32:;
    k = (k + 1);
    goto L30;
  L33:;
    goto L28;
  L34:;
    goto L32;
  L35:;
#line 766
    t = nx[i];
    nx[i] = nx[k];
    nx[k] = t;
    goto L34;
  L36:;
    if (i < (nodes - 1)) goto L37; else goto L38;
  L37:;
    c = nx[i];
    goto L39;
  L38:;
    goto L13;
  L39:;
    if (c <= nx[(i + 1)]) goto L40; else goto L42;
  L40:;
#line 773
    gfx_pb_set(pb, c, sy, idx);
    goto L41;
  L41:;
    c = (c + 1);
    goto L39;
  L42:;
#line 775
    i = (i + 2);
    goto L36;
}

static void PixBuf_CopyPal(void * src, void * dst) {
    (void)src;
    (void)dst;
    int32_t i;
    uint32_t packed;
    i = 0;
    goto L1;
  L1:;
    if (i <= 255) goto L2; else goto L4;
  L2:;
#line 788
    packed = ((uint32_t)(gfx_pb_pal_packed(src, i)));
#line 789
    gfx_pb_set_pal(dst, i, Color_UnpackR(packed), Color_UnpackG(packed), Color_UnpackB(packed));
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

static void PixBuf_LayerInit(void * pb) {
    (void)pb;
#line 796
    PixBuf_layers[0] = pb;
#line 797
    PixBuf_layerVis[0] = 1;
#line 798
    PixBuf_layerCount = 1;
#line 799
    PixBuf_layerActive = 0;
    return;
}

static int32_t PixBuf_LayerCount(void) {
    return PixBuf_layerCount;
}

static int32_t PixBuf_LayerActive(void) {
    return PixBuf_layerActive;
}

static void PixBuf_LayerSetActive(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 810
    PixBuf_layerActive = idx;
    goto L1;
  L3:;
    if (idx < PixBuf_layerCount) goto L2; else goto L1;
}

static void * PixBuf_LayerGet(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_layers[idx];
  L3:;
    return NULL;
  L4:;
    if (idx < PixBuf_layerCount) goto L2; else goto L3;
}

static void * PixBuf_LayerGetActive(void) {
    return PixBuf_layers[PixBuf_layerActive];
}

static int32_t PixBuf_LayerAdd(int32_t w, int32_t h) {
    (void)w;
    (void)h;
    void * p;
    int32_t idx;
    if (PixBuf_layerCount >= 16) goto L2; else goto L1;
  L1:;
#line 826
    p = gfx_pb_create(w, h);
    if (p == NULL) goto L4; else goto L3;
  L2:;
    return (-1);
  L3:;
    if (PixBuf_layers[0] != NULL) goto L6; else goto L5;
  L4:;
    return (-1);
  L5:;
#line 829
    gfx_pb_clear(p, 0);
#line 830
    idx = PixBuf_layerCount;
#line 831
    PixBuf_layers[idx] = p;
#line 832
    PixBuf_layerVis[idx] = 1;
#line 833
    (PixBuf_layerCount++);
    return idx;
  L6:;
#line 828
    PixBuf_CopyPal(PixBuf_layers[0], p);
    goto L5;
}

static void PixBuf_LayerRemove(int32_t idx) {
    (void)idx;
    int32_t i;
    if (idx <= 0) goto L2; else goto L3;
  L1:;
#line 841
    gfx_pb_free(PixBuf_layers[idx]);
    i = idx;
    goto L4;
  L2:;
    return;
  L3:;
    if (idx >= PixBuf_layerCount) goto L2; else goto L1;
  L4:;
    if (i <= (PixBuf_layerCount - 2)) goto L5; else goto L7;
  L5:;
#line 843
    PixBuf_layers[i] = PixBuf_layers[(i + 1)];
#line 844
    PixBuf_layerVis[i] = PixBuf_layerVis[(i + 1)];
    goto L6;
  L6:;
    i = (i + 1);
    goto L4;
  L7:;
#line 846
    (PixBuf_layerCount--);
    if (PixBuf_layerActive >= PixBuf_layerCount) goto L9; else goto L8;
  L8:;
    return;
  L9:;
#line 847
    PixBuf_layerActive = (PixBuf_layerCount - 1);
    goto L8;
}

static int PixBuf_LayerVisible(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_layerVis[idx] != 0;
  L3:;
    return 0;
  L4:;
    if (idx < PixBuf_layerCount) goto L2; else goto L3;
}

static void PixBuf_LayerSetVisible(int32_t idx, int vis) {
    (void)idx;
    (void)vis;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
    if (vis) goto L5; else goto L6;
  L3:;
    if (idx < PixBuf_layerCount) goto L2; else goto L1;
  L4:;
    goto L1;
  L5:;
#line 859
    PixBuf_layerVis[idx] = 1;
    goto L4;
  L6:;
    PixBuf_layerVis[idx] = 0;
    goto L4;
}

static void PixBuf_LayerMoveUp(int32_t idx) {
    (void)idx;
    void * tmp;
    int32_t tv;
    if (idx <= 0) goto L2; else goto L3;
  L1:;
#line 867
    tmp = PixBuf_layers[idx];
    PixBuf_layers[idx] = PixBuf_layers[(idx - 1)];
    PixBuf_layers[(idx - 1)] = tmp;
#line 868
    tv = PixBuf_layerVis[idx];
    PixBuf_layerVis[idx] = PixBuf_layerVis[(idx - 1)];
    PixBuf_layerVis[(idx - 1)] = tv;
    if (PixBuf_layerActive == idx) goto L5; else goto L6;
  L2:;
    return;
  L3:;
    if (idx >= PixBuf_layerCount) goto L2; else goto L1;
  L4:;
    return;
  L5:;
#line 869
    PixBuf_layerActive = (idx - 1);
    goto L4;
  L6:;
    if (PixBuf_layerActive == (idx - 1)) goto L7; else goto L4;
  L7:;
#line 870
    PixBuf_layerActive = idx;
    goto L4;
}

static void PixBuf_LayerMoveDown(int32_t idx) {
    (void)idx;
    if (idx < 0) goto L2; else goto L3;
  L1:;
#line 876
    PixBuf_LayerMoveUp((idx + 1));
    return;
  L2:;
    return;
  L3:;
    if (idx >= (PixBuf_layerCount - 1)) goto L2; else goto L1;
}

static void PixBuf_LayerFlatten(void * dst, int32_t transparentIdx) {
    (void)dst;
    (void)transparentIdx;
    int32_t li;
    if (PixBuf_layerCount > 0) goto L5; else goto L3;
  L1:;
    li = 1;
    goto L6;
  L2:;
#line 883
    gfx_pb_copy_pixels(PixBuf_layers[0], dst);
    goto L1;
  L3:;
#line 885
    gfx_pb_clear(dst, 0);
    goto L1;
  L4:;
    if (PixBuf_layers[0] != NULL) goto L2; else goto L3;
  L5:;
    if (PixBuf_layerVis[0] != 0) goto L4; else goto L3;
  L6:;
    if (li <= (PixBuf_layerCount - 1)) goto L7; else goto L9;
  L7:;
    if (PixBuf_layerVis[li] != 0) goto L12; else goto L10;
  L8:;
    li = (li + 1);
    goto L6;
  L9:;
    return;
  L10:;
    goto L8;
  L11:;
#line 889
    gfx_pb_composite(dst, PixBuf_layers[li], transparentIdx);
    goto L10;
  L12:;
    if (PixBuf_layers[li] != NULL) goto L11; else goto L10;
}

static void PixBuf_StampText(void * pb, void * ren, void * font, char *text, uint32_t text_high, int32_t x, int32_t y, int32_t idx) {
    (void)pb;
    (void)ren;
    (void)font;
    (void)text;
    (void)text_high;
    (void)x;
    (void)y;
    (void)idx;
#line 900
    gfx_pb_stamp_text(pb, ren, font, ((void *)(text)), x, y, idx);
    return;
}

static void PixBuf_Render(void * ren, void * tex, void * pb) {
    (void)ren;
    (void)tex;
    (void)pb;
#line 907
    gfx_pb_render(ren, tex, pb);
    return;
}

static void * PixBuf_Save(void * pb, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)pb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return gfx_pb_save(pb, x, y, w, h);
}

static void PixBuf_Restore(void * pb, void * region, int32_t x, int32_t y) {
    (void)pb;
    (void)region;
    (void)x;
    (void)y;
#line 917
    gfx_pb_restore(pb, region, x, y);
    return;
}

static int32_t PixBuf_SaveW(void * region) {
    (void)region;
    return gfx_pb_save_w(region);
}

static int32_t PixBuf_SaveH(void * region) {
    (void)region;
    return gfx_pb_save_h(region);
}

static void PixBuf_FreeSave(void * region) {
    (void)region;
#line 926
    gfx_pb_free_save(region);
    return;
}

static void PixBuf_WriteByteF(void * *f, int32_t v) {
    (void)f;
    (void)v;
#line 935
    m2_WriteChar(&(*f), ((char)(((uint32_t)(((uint32_t)(v))) & (uint32_t)(255)))));
    return;
}

static void PixBuf_Write16LE(void * *f, int32_t v) {
    (void)f;
    (void)v;
#line 939
    PixBuf_WriteByteF(&(*f), ((uint32_t)(((uint32_t)(v))) & (uint32_t)(255)));
#line 940
    PixBuf_WriteByteF(&(*f), ((uint32_t)(((uint32_t)(((uint32_t)(v))) >> (8))) & (uint32_t)(255)));
    return;
}

static void PixBuf_Write32LE(void * *f, int32_t v) {
    (void)f;
    (void)v;
#line 945
    PixBuf_WriteByteF(&(*f), ((uint32_t)(((uint32_t)(v))) & (uint32_t)(255)));
#line 946
    PixBuf_WriteByteF(&(*f), ((uint32_t)(((uint32_t)(((uint32_t)(v))) >> (8))) & (uint32_t)(255)));
#line 947
    PixBuf_WriteByteF(&(*f), ((uint32_t)(((uint32_t)(((uint32_t)(v))) >> (16))) & (uint32_t)(255)));
#line 948
    PixBuf_WriteByteF(&(*f), ((uint32_t)(((uint32_t)(((uint32_t)(v))) >> (24))) & (uint32_t)(255)));
    return;
}

static void PixBuf_ReadByteF(void * *f, int32_t *v) {
    (void)f;
    (void)v;
    char ch;
#line 954
    m2_ReadChar(&(*f), &ch);
    if (FileSystem_Done) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 955
    (*v) = ((int32_t)((unsigned char)(ch)));
    goto L1;
  L3:;
    (*v) = 0;
    goto L1;
}

static void PixBuf_Read16LE(void * *f, int32_t *v) {
    (void)f;
    (void)v;
    int32_t lo;
    int32_t hi;
#line 961
    PixBuf_ReadByteF(&(*f), &lo);
    PixBuf_ReadByteF(&(*f), &hi);
#line 962
    (*v) = ((uint32_t)(((uint32_t)(lo))) | (uint32_t)(((uint32_t)(((uint32_t)(hi))) << (8))));
    return;
}

static void PixBuf_Read32LE(void * *f, int32_t *v) {
    (void)f;
    (void)v;
    int32_t b0;
    int32_t b1;
    int32_t b2;
    int32_t b3;
#line 968
    PixBuf_ReadByteF(&(*f), &b0);
    PixBuf_ReadByteF(&(*f), &b1);
#line 969
    PixBuf_ReadByteF(&(*f), &b2);
    PixBuf_ReadByteF(&(*f), &b3);
#line 970
    (*v) = ((uint32_t)(((uint32_t)(((uint32_t)(b0))) | (uint32_t)(((uint32_t)(((uint32_t)(b1))) << (8))))) | (uint32_t)(((uint32_t)(((uint32_t)(((uint32_t)(b2))) << (16))) | (uint32_t)(((uint32_t)(((uint32_t)(b3))) << (24))))));
    return;
}

static int PixBuf_SaveBMP(void * pb, char *path, uint32_t path_high) {
    (void)pb;
    (void)path;
    (void)path_high;
    void * f;
    int32_t w;
    int32_t h;
    int32_t rowSz;
    int32_t pixSz;
    int32_t off;
    int32_t fsz;
    int32_t padN;
    int32_t row;
    int32_t col;
    int32_t i;
    uint32_t packed;
    void * pix;
    if (pb == NULL) goto L2; else goto L1;
  L1:;
#line 984
    w = gfx_pb_width(pb);
    h = gfx_pb_height(pb);
#line 985
    rowSz = ((uint32_t)((w + 3)) & (uint32_t)(((int32_t)((~(uint32_t)(3))))));
#line 986
    pixSz = (rowSz * h);
#line 987
    off = ((14 + 40) + 1024);
#line 988
    fsz = (off + pixSz);
#line 989
    padN = (rowSz - w);
#line 991
    m2_Lookup(&f, path, 1);
    if (FileSystem_Done) goto L3; else goto L4;
  L2:;
    return 0;
  L3:;
#line 995
    m2_WriteChar(&f, 'B');
    m2_WriteChar(&f, 'M');
#line 996
    PixBuf_Write32LE(&f, fsz);
#line 997
    PixBuf_Write16LE(&f, 0);
    PixBuf_Write16LE(&f, 0);
#line 998
    PixBuf_Write32LE(&f, off);
#line 1001
    PixBuf_Write32LE(&f, 40);
#line 1002
    PixBuf_Write32LE(&f, w);
#line 1003
    PixBuf_Write32LE(&f, h);
#line 1004
    PixBuf_Write16LE(&f, 1);
#line 1005
    PixBuf_Write16LE(&f, 8);
#line 1006
    PixBuf_Write32LE(&f, 0);
#line 1007
    PixBuf_Write32LE(&f, pixSz);
#line 1008
    PixBuf_Write32LE(&f, 0);
#line 1009
    PixBuf_Write32LE(&f, 0);
#line 1010
    PixBuf_Write32LE(&f, 0);
#line 1011
    PixBuf_Write32LE(&f, 0);
    i = 0;
    goto L5;
  L4:;
    return 0;
  L5:;
    if (i <= 255) goto L6; else goto L8;
  L6:;
#line 1015
    packed = PixBuf_PalPacked(pb, i);
#line 1016
    PixBuf_WriteByteF(&f, Color_UnpackB(packed));
#line 1017
    PixBuf_WriteByteF(&f, Color_UnpackG(packed));
#line 1018
    PixBuf_WriteByteF(&f, Color_UnpackR(packed));
#line 1019
    PixBuf_WriteByteF(&f, 0);
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 1023
    pix = gfx_pb_pixel_ptr(pb);
    row = (h - 1);
    goto L9;
  L9:;
    if (row >= 0) goto L10; else goto L12;
  L10:;
    col = 0;
    goto L13;
  L11:;
    row = (row + (-1));
    goto L9;
  L12:;
#line 1030
    m2_Close(&f);
    return 1;
  L13:;
    if (col <= (w - 1)) goto L14; else goto L16;
  L14:;
#line 1026
    PixBuf_WriteByteF(&f, gfx_buf_get(pix, ((row * w) + col)));
    goto L15;
  L15:;
    col = (col + 1);
    goto L13;
  L16:;
    i = 0;
    goto L17;
  L17:;
    if (i <= (padN - 1)) goto L18; else goto L20;
  L18:;
#line 1028
    PixBuf_WriteByteF(&f, 0);
    goto L19;
  L19:;
    i = (i + 1);
    goto L17;
  L20:;
    goto L11;
}

static int PixBuf_SavePNG(void * pb, char *path, uint32_t path_high) {
    (void)pb;
    (void)path;
    (void)path_high;
    return gfx_pb_save_png(pb, ((void *)(path))) != 0;
}

static void * PixBuf_LoadPNG(char *path, uint32_t path_high, int32_t ncolors) {
    (void)path;
    (void)path_high;
    (void)ncolors;
    return gfx_pb_load_png(((void *)(path)), ncolors);
}

static void * PixBuf_LoadPNGPal(char *path, uint32_t path_high, void * palSrc, int32_t ncolors) {
    (void)path;
    (void)path_high;
    (void)palSrc;
    (void)ncolors;
    return gfx_pb_load_png_pal(((void *)(path)), palSrc, ncolors);
}

static int PixBuf_SaveDP2(char *path, uint32_t path_high) {
    (void)path;
    (void)path_high;
    void * f;
    void * base;
    int32_t li;
    int32_t i;
    int32_t total;
    uint32_t packed;
    void * pix;
    if (PixBuf_layerCount < 1) goto L2; else goto L1;
  L1:;
#line 1056
    base = PixBuf_layers[0];
    if (base == NULL) goto L4; else goto L3;
  L2:;
    return 0;
  L3:;
#line 1059
    m2_Lookup(&f, path, 1);
    if (FileSystem_Done) goto L5; else goto L6;
  L4:;
    return 0;
  L5:;
#line 1063
    m2_WriteChar(&f, 'D');
    m2_WriteChar(&f, 'P');
    m2_WriteChar(&f, '2');
#line 1064
    PixBuf_WriteByteF(&f, 0);
#line 1065
    PixBuf_WriteByteF(&f, 1);
#line 1066
    PixBuf_Write16LE(&f, 256);
#line 1067
    PixBuf_WriteByteF(&f, PixBuf_layerCount);
#line 1068
    PixBuf_Write32LE(&f, gfx_pb_width(base));
#line 1069
    PixBuf_Write32LE(&f, gfx_pb_height(base));
    i = 0;
    goto L7;
  L6:;
    return 0;
  L7:;
    if (i <= 255) goto L8; else goto L10;
  L8:;
#line 1073
    packed = PixBuf_PalPacked(base, i);
#line 1074
    PixBuf_WriteByteF(&f, Color_UnpackR(packed));
#line 1075
    PixBuf_WriteByteF(&f, Color_UnpackG(packed));
#line 1076
    PixBuf_WriteByteF(&f, Color_UnpackB(packed));
    goto L9;
  L9:;
    i = (i + 1);
    goto L7;
  L10:;
    li = 0;
    goto L11;
  L11:;
    if (li <= (PixBuf_layerCount - 1)) goto L12; else goto L14;
  L12:;
#line 1081
    PixBuf_WriteByteF(&f, PixBuf_layerVis[li]);
    if (PixBuf_layers[li] != NULL) goto L16; else goto L15;
  L13:;
    li = (li + 1);
    goto L11;
  L14:;
#line 1090
    m2_Close(&f);
    return 1;
  L15:;
    goto L13;
  L16:;
#line 1083
    pix = gfx_pb_pixel_ptr(PixBuf_layers[li]);
#line 1084
    total = (gfx_pb_width(PixBuf_layers[li]) * gfx_pb_height(PixBuf_layers[li]));
    i = 0;
    goto L17;
  L17:;
    if (i <= (total - 1)) goto L18; else goto L20;
  L18:;
#line 1086
    PixBuf_WriteByteF(&f, gfx_buf_get(pix, i));
    goto L19;
  L19:;
    i = (i + 1);
    goto L17;
  L20:;
    goto L15;
}

static int PixBuf_LoadDP2(char *path, uint32_t path_high) {
    (void)path;
    (void)path_high;
    void * f;
    char ch;
    int32_t ver;
    int32_t nc;
    int32_t nl;
    int32_t w;
    int32_t h;
    int32_t pr[255 + 1];
    int32_t pg[255 + 1];
    int32_t pbb[255 + 1];
    int32_t i;
    int32_t li;
    int32_t idx;
    int32_t total;
    void * base;
    void * lp;
    void * pix;
    int32_t byt;
#line 1104
    m2_Lookup(&f, path, 0);
    if (FileSystem_Done) goto L1; else goto L2;
  L1:;
#line 1108
    m2_ReadChar(&f, &ch);
    if (ch != 'D') goto L4; else goto L3;
  L2:;
    return 0;
  L3:;
#line 1109
    m2_ReadChar(&f, &ch);
    if (ch != 'P') goto L6; else goto L5;
  L4:;
#line 1108
    m2_Close(&f);
    return 0;
  L5:;
#line 1110
    m2_ReadChar(&f, &ch);
    if (ch != '2') goto L8; else goto L7;
  L6:;
#line 1109
    m2_Close(&f);
    return 0;
  L7:;
#line 1111
    PixBuf_ReadByteF(&f, &ver);
#line 1112
    PixBuf_ReadByteF(&f, &ver);
    if (ver != 1) goto L10; else goto L9;
  L8:;
#line 1110
    m2_Close(&f);
    return 0;
  L9:;
#line 1115
    PixBuf_Read16LE(&f, &nc);
#line 1116
    PixBuf_ReadByteF(&f, &nl);
#line 1117
    PixBuf_Read32LE(&f, &w);
    PixBuf_Read32LE(&f, &h);
    if (nl < 1) goto L12; else goto L16;
  L10:;
#line 1113
    m2_Close(&f);
    return 0;
  L11:;
    i = 0;
    goto L17;
  L12:;
#line 1119
    m2_Close(&f);
    return 0;
  L13:;
    if (h > 8192) goto L12; else goto L11;
  L14:;
    if (w > 8192) goto L12; else goto L13;
  L15:;
    if (h < 1) goto L12; else goto L14;
  L16:;
    if (w < 1) goto L12; else goto L15;
  L17:;
    if (i <= 255) goto L18; else goto L20;
  L18:;
#line 1124
    PixBuf_ReadByteF(&f, &pr[i]);
    PixBuf_ReadByteF(&f, &pg[i]);
    PixBuf_ReadByteF(&f, &pbb[i]);
    goto L19;
  L19:;
    i = (i + 1);
    goto L17;
  L20:;
    goto L21;
  L21:;
    if (PixBuf_layerCount > 1) goto L22; else goto L23;
  L22:;
#line 1128
    PixBuf_LayerRemove((PixBuf_layerCount - 1));
    goto L21;
  L23:;
    if (PixBuf_layers[0] != NULL) goto L25; else goto L24;
  L24:;
#line 1132
    PixBuf_layers[0] = gfx_pb_create(w, h);
    if (PixBuf_layers[0] == NULL) goto L27; else goto L26;
  L25:;
#line 1129
    gfx_pb_free(PixBuf_layers[0]);
    goto L24;
  L26:;
#line 1134
    PixBuf_layerCount = 1;
#line 1135
    base = PixBuf_layers[0];
    i = 0;
    goto L28;
  L27:;
#line 1133
    m2_Close(&f);
    return 0;
  L28:;
    if (i <= 255) goto L29; else goto L31;
  L29:;
#line 1138
    gfx_pb_set_pal(base, i, pr[i], pg[i], pbb[i]);
    goto L30;
  L30:;
    i = (i + 1);
    goto L28;
  L31:;
#line 1141
    PixBuf_ReadByteF(&f, &PixBuf_layerVis[0]);
#line 1142
    pix = gfx_pb_pixel_ptr(base);
#line 1143
    total = (w * h);
    i = 0;
    goto L32;
  L32:;
    if (i <= (total - 1)) goto L33; else goto L35;
  L33:;
#line 1145
    PixBuf_ReadByteF(&f, &byt);
#line 1146
    gfx_buf_set(pix, i, byt);
    goto L34;
  L34:;
    i = (i + 1);
    goto L32;
  L35:;
#line 1148
    gfx_pb_mark_dirty(base, 0, 0, w, h);
    li = 1;
    goto L36;
  L36:;
    if (li <= (nl - 1)) goto L37; else goto L39;
  L37:;
#line 1152
    idx = PixBuf_LayerAdd(w, h);
    if (idx < 0) goto L41; else goto L40;
  L38:;
    li = (li + 1);
    goto L36;
  L39:;
#line 1166
    PixBuf_layerActive = 0;
#line 1167
    m2_Close(&f);
    return 1;
  L40:;
#line 1154
    PixBuf_ReadByteF(&f, &PixBuf_layerVis[idx]);
#line 1155
    lp = PixBuf_layers[idx];
    if (lp != NULL) goto L43; else goto L42;
  L41:;
#line 1153
    m2_Close(&f);
    return 1;
  L42:;
    goto L38;
  L43:;
#line 1157
    PixBuf_CopyPal(base, lp);
#line 1158
    pix = gfx_pb_pixel_ptr(lp);
    i = 0;
    goto L44;
  L44:;
    if (i <= (total - 1)) goto L45; else goto L47;
  L45:;
#line 1160
    PixBuf_ReadByteF(&f, &byt);
#line 1161
    gfx_buf_set(pix, i, byt);
    goto L46;
  L46:;
    i = (i + 1);
    goto L44;
  L47:;
#line 1163
    gfx_pb_mark_dirty(lp, 0, 0, w, h);
    goto L42;
}

static void PixBuf_WriteIntF(void * *f, int32_t n) {
    (void)f;
    (void)n;
    char buf[10 + 1];
    int32_t i;
    int32_t k;
    int neg;
    if (n < 0) goto L2; else goto L3;
  L1:;
#line 1179
    i = 0;
    goto L4;
  L2:;
#line 1178
    neg = 1;
    n = (-n);
    goto L1;
  L3:;
    neg = 0;
    goto L1;
  L4:;
#line 1181
    buf[i] = ((char)((((int32_t)((unsigned char)('0'))) + m2_mod(n, 10))));
#line 1182
    n = m2_div(n, 10);
#line 1183
    (i++);
    if (n == 0) goto L5; else goto L4;
  L5:;
    if (neg) goto L7; else goto L6;
  L6:;
    k = (i - 1);
    goto L8;
  L7:;
#line 1185
    m2_WriteChar(&(*f), '-');
    goto L6;
  L8:;
    if (k >= 0) goto L9; else goto L11;
  L9:;
#line 1186
    m2_WriteChar(&(*f), buf[k]);
    goto L10;
  L10:;
    k = (k + (-1));
    goto L8;
  L11:;
    return;
}

static void PixBuf_SkipWhite(void * *f, char *ch) {
    (void)f;
    (void)ch;
    goto L1;
  L1:;
    if (FileSystem_Done) goto L4; else goto L3;
  L2:;
#line 1193
    m2_ReadChar(&(*f), &(*ch));
    goto L1;
  L3:;
    return;
  L4:;
    if ((*ch) == ' ') goto L2; else goto L7;
  L5:;
    if ((*ch) == '\r') goto L2; else goto L3;
  L6:;
    if ((*ch) == '\n') goto L2; else goto L5;
  L7:;
    if ((*ch) == '\t') goto L2; else goto L6;
}

static int PixBuf_ReadIntF(void * *f, int32_t *n) {
    (void)f;
    (void)n;
    char ch;
    int neg;
    int got;
#line 1202
    (*n) = 0;
    got = 0;
    neg = 0;
#line 1203
    m2_ReadChar(&(*f), &ch);
#line 1204
    PixBuf_SkipWhite(&(*f), &ch);
    if (FileSystem_Done) goto L1; else goto L2;
  L1:;
    if (ch == '-') goto L4; else goto L3;
  L2:;
    return 0;
  L3:;
    goto L5;
  L4:;
#line 1206
    neg = 1;
    m2_ReadChar(&(*f), &ch);
    goto L3;
  L5:;
    if (FileSystem_Done) goto L9; else goto L7;
  L6:;
#line 1208
    (*n) = (((*n) * 10) + (((int32_t)((unsigned char)(ch))) - ((int32_t)((unsigned char)('0')))));
#line 1209
    got = 1;
#line 1210
    m2_ReadChar(&(*f), &ch);
    goto L5;
  L7:;
    if (neg) goto L11; else goto L10;
  L8:;
    if (ch <= '9') goto L6; else goto L7;
  L9:;
    if (ch >= '0') goto L8; else goto L7;
  L10:;
    return got;
  L11:;
#line 1212
    (*n) = (-(*n));
    goto L10;
}

static int PixBuf_SavePal(void * pb, char *path, uint32_t path_high) {
    (void)pb;
    (void)path;
    (void)path_high;
    void * f;
    int32_t i;
    int32_t r;
    int32_t g;
    int32_t b;
    uint32_t packed;
#line 1223
    m2_Lookup(&f, path, 1);
    if (FileSystem_Done) goto L1; else goto L2;
  L1:;
    i = 0;
    goto L3;
  L2:;
    return 0;
  L3:;
    if (i <= 255) goto L4; else goto L6;
  L4:;
#line 1226
    packed = PixBuf_PalPacked(pb, i);
#line 1227
    r = Color_UnpackR(packed);
#line 1228
    g = Color_UnpackG(packed);
#line 1229
    b = Color_UnpackB(packed);
#line 1230
    PixBuf_WriteIntF(&f, r);
#line 1231
    m2_WriteChar(&f, ' ');
#line 1232
    PixBuf_WriteIntF(&f, g);
#line 1233
    m2_WriteChar(&f, ' ');
#line 1234
    PixBuf_WriteIntF(&f, b);
#line 1235
    m2_WriteChar(&f, '\n');
    goto L5;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 1237
    m2_Close(&f);
    return 1;
}

static int PixBuf_LoadPal(void * pb, char *path, uint32_t path_high) {
    (void)pb;
    (void)path;
    (void)path_high;
    void * f;
    int32_t i;
    int32_t r;
    int32_t g;
    int32_t b;
#line 1245
    m2_Lookup(&f, path, 0);
    if (FileSystem_Done) goto L1; else goto L2;
  L1:;
    i = 0;
    goto L3;
  L2:;
    return 0;
  L3:;
    if (i <= 255) goto L4; else goto L6;
  L4:;
    if (PixBuf_ReadIntF(&f, &r)) goto L7; else goto L8;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 1253
    m2_Close(&f);
    return 1;
  L7:;
    if (PixBuf_ReadIntF(&f, &g)) goto L9; else goto L10;
  L8:;
#line 1248
    m2_Close(&f);
    return i > 0;
  L9:;
    if (PixBuf_ReadIntF(&f, &b)) goto L11; else goto L12;
  L10:;
#line 1249
    m2_Close(&f);
    return i > 0;
  L11:;
#line 1251
    PixBuf_SetPal(pb, i, r, g, b);
    goto L5;
  L12:;
#line 1250
    m2_Close(&f);
    return i > 0;
}

static void PixBuf_FrameInit(void * pb) {
    (void)pb;
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (256 - 1)) goto L2; else goto L4;
  L2:;
#line 1265
    PixBuf_frames[i] = NULL;
#line 1266
    PixBuf_frameTiming[i] = 100;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 1268
    PixBuf_frames[0] = pb;
#line 1269
    PixBuf_nFrames = 1;
#line 1270
    PixBuf_currentFrame = 0;
    return;
}

static int32_t PixBuf_FrameCount(void) {
    return PixBuf_nFrames;
}

static int32_t PixBuf_FrameCurrent(void) {
    return PixBuf_currentFrame;
}

static int32_t PixBuf_FrameNew(int32_t w, int32_t h) {
    (void)w;
    (void)h;
    void * p;
    int32_t idx;
    if (PixBuf_nFrames >= 256) goto L2; else goto L1;
  L1:;
#line 1283
    p = gfx_pb_create(w, h);
    if (p == NULL) goto L4; else goto L3;
  L2:;
    return (-1);
  L3:;
    if (PixBuf_frames[0] != NULL) goto L6; else goto L5;
  L4:;
    return (-1);
  L5:;
#line 1286
    gfx_pb_clear(p, 0);
#line 1287
    idx = PixBuf_nFrames;
#line 1288
    PixBuf_frames[idx] = p;
#line 1289
    PixBuf_frameTiming[idx] = 100;
#line 1290
    (PixBuf_nFrames++);
    return idx;
  L6:;
#line 1285
    PixBuf_CopyPal(PixBuf_frames[0], p);
    goto L5;
}

static void PixBuf_FrameDelete(int32_t idx) {
    (void)idx;
    int32_t i;
    if (idx < 0) goto L2; else goto L4;
  L1:;
    if (PixBuf_frames[idx] != NULL) goto L6; else goto L5;
  L2:;
    return;
  L3:;
    if (PixBuf_nFrames <= 1) goto L2; else goto L1;
  L4:;
    if (idx >= PixBuf_nFrames) goto L2; else goto L3;
  L5:;
    i = idx;
    goto L7;
  L6:;
#line 1298
    gfx_pb_free(PixBuf_frames[idx]);
    goto L5;
  L7:;
    if (i <= (PixBuf_nFrames - 2)) goto L8; else goto L10;
  L8:;
#line 1300
    PixBuf_frames[i] = PixBuf_frames[(i + 1)];
#line 1301
    PixBuf_frameTiming[i] = PixBuf_frameTiming[(i + 1)];
    goto L9;
  L9:;
    i = (i + 1);
    goto L7;
  L10:;
#line 1303
    (PixBuf_nFrames--);
#line 1304
    PixBuf_frames[PixBuf_nFrames] = NULL;
    if (PixBuf_currentFrame >= PixBuf_nFrames) goto L12; else goto L11;
  L11:;
    return;
  L12:;
#line 1305
    PixBuf_currentFrame = (PixBuf_nFrames - 1);
    goto L11;
}

static void PixBuf_FrameSet(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 1310
    PixBuf_currentFrame = idx;
    goto L1;
  L3:;
    if (idx < PixBuf_nFrames) goto L2; else goto L1;
}

static void * PixBuf_FrameGet(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_frames[idx];
  L3:;
    return NULL;
  L4:;
    if (idx < PixBuf_nFrames) goto L2; else goto L3;
}

static void * PixBuf_FrameGetCurrent(void) {
    if (PixBuf_currentFrame >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_frames[PixBuf_currentFrame];
  L3:;
    return NULL;
  L4:;
    if (PixBuf_currentFrame < PixBuf_nFrames) goto L2; else goto L3;
}

static int32_t PixBuf_FrameTiming(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L4; else goto L3;
  L1:;
    return 0;
  L2:;
    return PixBuf_frameTiming[idx];
  L3:;
    return 100;
  L4:;
    if (idx < PixBuf_nFrames) goto L2; else goto L3;
}

static void PixBuf_FrameSetTiming(int32_t idx, int32_t ms) {
    (void)idx;
    (void)ms;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 1334
    PixBuf_frameTiming[idx] = ms;
    goto L1;
  L3:;
    if (idx < PixBuf_nFrames) goto L2; else goto L1;
}

static void * PixBuf_FrameDuplicate(int32_t idx) {
    (void)idx;
    void * src;
    void * p;
    int32_t ni;
    if (idx < 0) goto L2; else goto L4;
  L1:;
#line 1341
    src = PixBuf_frames[idx];
    if (src == NULL) goto L6; else goto L5;
  L2:;
    return NULL;
  L3:;
    if (PixBuf_nFrames >= 256) goto L2; else goto L1;
  L4:;
    if (idx >= PixBuf_nFrames) goto L2; else goto L3;
  L5:;
#line 1343
    p = gfx_pb_create(gfx_pb_width(src), gfx_pb_height(src));
    if (p == NULL) goto L8; else goto L7;
  L6:;
    return NULL;
  L7:;
#line 1345
    gfx_pb_copy_pixels(src, p);
#line 1346
    ni = PixBuf_nFrames;
#line 1347
    PixBuf_frames[ni] = p;
#line 1348
    PixBuf_frameTiming[ni] = PixBuf_frameTiming[idx];
#line 1349
    (PixBuf_nFrames++);
    return p;
  L8:;
    return NULL;
}

static void PixBuf_RenderAlpha(void * ren, void * tex, void * pb, int32_t alpha) {
    (void)ren;
    (void)tex;
    (void)pb;
    (void)alpha;
#line 1354
    gfx_pb_render_alpha(ren, tex, pb, alpha);
    return;
}

static void * PixBuf_FramesToSheet(int32_t cols) {
    (void)cols;
    int32_t rows;
    int32_t fw;
    int32_t fh;
    int32_t f;
    int32_t dx;
    int32_t dy;
    int32_t x;
    int32_t y;
    void * sheet;
    void * src;
    void * srcBuf;
    void * dstBuf;
    int32_t sw;
    int32_t sheetW;
    if (PixBuf_nFrames <= 0) goto L2; else goto L1;
  L1:;
    if (cols <= 0) goto L4; else goto L3;
  L2:;
    return NULL;
  L3:;
#line 1366
    rows = m2_div(((PixBuf_nFrames + cols) - 1), cols);
    if (PixBuf_frames[0] != NULL) goto L6; else goto L7;
  L4:;
#line 1365
    cols = PixBuf_nFrames;
    goto L3;
  L5:;
#line 1370
    sheet = gfx_pb_create((fw * cols), (fh * rows));
    if (sheet == NULL) goto L9; else goto L8;
  L6:;
#line 1368
    fw = gfx_pb_width(PixBuf_frames[0]);
    fh = gfx_pb_height(PixBuf_frames[0]);
    goto L5;
  L7:;
#line 1369
    fw = 64;
    fh = 64;
    goto L5;
  L8:;
    if (PixBuf_frames[0] != NULL) goto L11; else goto L10;
  L9:;
    return NULL;
  L10:;
#line 1373
    gfx_pb_clear(sheet, 0);
#line 1374
    sheetW = (fw * cols);
#line 1375
    dstBuf = gfx_pb_pixel_ptr(sheet);
    f = 0;
    goto L12;
  L11:;
#line 1372
    PixBuf_CopyPal(PixBuf_frames[0], sheet);
    goto L10;
  L12:;
    if (f <= (PixBuf_nFrames - 1)) goto L13; else goto L15;
  L13:;
#line 1377
    src = PixBuf_frames[f];
    if (src != NULL) goto L17; else goto L16;
  L14:;
    f = (f + 1);
    goto L12;
  L15:;
#line 1395
    gfx_pb_mark_dirty(sheet, 0, 0, (fw * cols), (fh * rows));
    return sheet;
  L16:;
    goto L14;
  L17:;
#line 1379
    srcBuf = gfx_pb_pixel_ptr(src);
#line 1380
    sw = gfx_pb_width(src);
#line 1381
    dx = (m2_mod(f, cols) * fw);
#line 1382
    dy = (m2_div(f, cols) * fh);
    y = 0;
    goto L18;
  L18:;
    if (y <= (fh - 1)) goto L19; else goto L21;
  L19:;
    if (y < gfx_pb_height(src)) goto L23; else goto L22;
  L20:;
    y = (y + 1);
    goto L18;
  L21:;
    goto L16;
  L22:;
    goto L20;
  L23:;
    x = 0;
    goto L24;
  L24:;
    if (x <= (fw - 1)) goto L25; else goto L27;
  L25:;
    if (x < sw) goto L29; else goto L28;
  L26:;
    x = (x + 1);
    goto L24;
  L27:;
    goto L22;
  L28:;
    goto L26;
  L29:;
#line 1387
    gfx_buf_set(dstBuf, (((dy + y) * sheetW) + (dx + x)), gfx_buf_get(srcBuf, ((y * sw) + x)));
    goto L28;
}

static void PixBuf_RenderHAM(void * ren, void * tex, void * pb, int32_t mode) {
    (void)ren;
    (void)tex;
    (void)pb;
    (void)mode;
#line 1404
    gfx_pb_render_ham(ren, tex, pb, mode);
    return;
}

static void PixBuf_CopperGradient(void * ren, void * tex, void * pb, int32_t startLine, int32_t endLine, int32_t c1, int32_t c2) {
    (void)ren;
    (void)tex;
    (void)pb;
    (void)startLine;
    (void)endLine;
    (void)c1;
    (void)c2;
    uint32_t packed1;
    uint32_t packed2;
    uint32_t px;
    uint32_t scr;
    int32_t r1;
    int32_t g1;
    int32_t b1;
    int32_t r2;
    int32_t g2;
    int32_t b2;
    int32_t w;
    int32_t h;
    int32_t range;
    int32_t y;
    int32_t x;
    int32_t offset;
    int32_t tVal;
    int32_t tr;
    int32_t tg;
    int32_t tb;
    int32_t pixR;
    int32_t pixG;
    int32_t pixB;
#line 1414
    w = gfx_pb_width(pb);
    h = gfx_pb_height(pb);
    if (startLine < 0) goto L2; else goto L1;
  L1:;
    if (endLine > h) goto L4; else goto L3;
  L2:;
#line 1415
    startLine = 0;
    goto L1;
  L3:;
#line 1417
    range = (endLine - startLine);
    if (range <= 0) goto L6; else goto L5;
  L4:;
#line 1416
    endLine = h;
    goto L3;
  L5:;
#line 1421
    packed1 = PixBuf_PalPacked(pb, m2_mod(c1, 256));
#line 1422
    packed2 = PixBuf_PalPacked(pb, m2_mod(c2, 256));
#line 1423
    r1 = Color_UnpackR(packed1);
    g1 = Color_UnpackG(packed1);
    b1 = Color_UnpackB(packed1);
#line 1424
    r2 = Color_UnpackR(packed2);
    g2 = Color_UnpackG(packed2);
    b2 = Color_UnpackB(packed2);
#line 1427
    gfx_pb_pal_to_screen(pb);
    y = startLine;
    goto L7;
  L6:;
    return;
  L7:;
    if (y <= (endLine - 1)) goto L8; else goto L10;
  L8:;
#line 1431
    tVal = m2_div(((y - startLine) * 255), range);
#line 1432
    tr = (r1 + m2_div(((r2 - r1) * tVal), 255));
#line 1433
    tg = (g1 + m2_div(((g2 - g1) * tVal), 255));
#line 1434
    tb = (b1 + m2_div(((b2 - b1) * tVal), 255));
    x = 0;
    goto L11;
  L9:;
    y = (y + 1);
    goto L7;
  L10:;
#line 1455
    gfx_pb_flush_tex(tex, pb);
    return;
  L11:;
    if (x <= (w - 1)) goto L12; else goto L14;
  L12:;
#line 1436
    offset = ((y * w) + x);
#line 1437
    px = ((uint32_t)(gfx_pb_rgba_get32(pb, offset)));
#line 1439
    pixR = ((int32_t)(((uint32_t)(((uint32_t)(px) >> (16))) & (uint32_t)(255))));
#line 1440
    pixG = ((int32_t)(((uint32_t)(((uint32_t)(px) >> (8))) & (uint32_t)(255))));
#line 1441
    pixB = ((int32_t)(((uint32_t)(px) & (uint32_t)(255))));
#line 1443
    pixR = m2_div(((pixR * 60) + (tr * 40)), 100);
#line 1444
    pixG = m2_div(((pixG * 60) + (tg * 40)), 100);
#line 1445
    pixB = m2_div(((pixB * 60) + (tb * 40)), 100);
#line 1447
    scr = ((uint32_t)(((uint32_t)(((uint32_t)(4278190080) | (uint32_t)(((uint32_t)(((uint32_t)(pixR))) << (16))))) | (uint32_t)(((uint32_t)(((uint32_t)(pixG))) << (8))))) | (uint32_t)(((uint32_t)(pixB))));
#line 1450
    gfx_pb_rgba_set32(pb, offset, ((int32_t)(scr)));
    goto L13;
  L13:;
    x = (x + 1);
    goto L11;
  L14:;
    goto L9;
}

static void PixBuf_WriteStr(void * *f, char *s, uint32_t s_high) {
    (void)f;
    (void)s;
    (void)s_high;
    int32_t i;
#line 1465
    i = 0;
    goto L1;
  L1:;
    if (i <= s_high) goto L4; else goto L3;
  L2:;
#line 1467
    m2_WriteChar(&(*f), s[i]);
    (i++);
    goto L1;
  L3:;
    return;
  L4:;
    if (s[i] != '\0') goto L2; else goto L3;
}

static int PixBuf_ConfigSave(char *path, uint32_t path_high, int32_t *keys, uint32_t keys_high, int32_t *vals, uint32_t vals_high, int32_t count) {
    (void)path;
    (void)path_high;
    (void)keys;
    (void)keys_high;
    (void)vals;
    (void)vals_high;
    (void)count;
    void * f;
    int32_t i;
    int32_t n;
#line 1476
    m2_Lookup(&f, path, 1);
    if (FileSystem_Done) goto L1; else goto L2;
  L1:;
#line 1478
    PixBuf_WriteStr(&f, "DPAINT_CFG 1", 12);
#line 1479
    m2_WriteChar(&f, '\n');
#line 1480
    n = count;
    if (n > 64) goto L4; else goto L3;
  L2:;
    return 0;
  L3:;
    i = 0;
    goto L5;
  L4:;
#line 1481
    n = 64;
    goto L3;
  L5:;
    if (i <= (n - 1)) goto L6; else goto L8;
  L6:;
#line 1483
    PixBuf_WriteIntF(&f, keys[i]);
#line 1484
    m2_WriteChar(&f, ' ');
#line 1485
    PixBuf_WriteIntF(&f, vals[i]);
#line 1486
    m2_WriteChar(&f, '\n');
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 1488
    m2_Close(&f);
    return 1;
}

static int32_t PixBuf_ConfigLoad(char *path, uint32_t path_high, int32_t *keys, uint32_t keys_high, int32_t *vals, uint32_t vals_high, int32_t maxCount) {
    (void)path;
    (void)path_high;
    (void)keys;
    (void)keys_high;
    (void)vals;
    (void)vals_high;
    (void)maxCount;
    void * f;
    char ch;
    int32_t cnt;
    int32_t ver;
#line 1498
    m2_Lookup(&f, path, 0);
    if (FileSystem_Done) goto L1; else goto L2;
  L1:;
#line 1501
    m2_ReadChar(&f, &ch);
    goto L3;
  L2:;
    return 0;
  L3:;
    if (FileSystem_Done) goto L7; else goto L5;
  L4:;
#line 1502
    m2_ReadChar(&f, &ch);
    goto L3;
  L5:;
#line 1504
    cnt = 0;
    goto L8;
  L6:;
    if (ch != '\r') goto L4; else goto L5;
  L7:;
    if (ch != '\n') goto L6; else goto L5;
  L8:;
    if (cnt < maxCount) goto L9; else goto L10;
  L9:;
    if (PixBuf_ReadIntF(&f, &keys[cnt])) goto L11; else goto L12;
  L10:;
#line 1510
    m2_Close(&f);
    return cnt;
  L11:;
    if (PixBuf_ReadIntF(&f, &vals[cnt])) goto L13; else goto L14;
  L12:;
#line 1506
    m2_Close(&f);
    return cnt;
  L13:;
#line 1508
    (cnt++);
    goto L8;
  L14:;
#line 1507
    m2_Close(&f);
    return cnt;
}

static void PixBuf_Log(char *path, uint32_t path_high, char *msg, uint32_t msg_high) {
    (void)path;
    (void)path_high;
    (void)msg;
    (void)msg_high;
#line 1515
    gfx_log(((void *)(path)), ((void *)(msg)));
    return;
}

static void PixBuf_init(void) {
#line 1518
    PixBuf_polyN = 0;
#line 1519
    PixBuf_layerCount = 0;
#line 1520
    PixBuf_layerActive = 0;
#line 1521
    PixBuf_nFrames = 0;
#line 1522
    PixBuf_currentFrame = 0;
    return;
}

/* Imported Module Assets */

typedef struct Assets_RegionDef Assets_RegionDef;
static const int32_t Assets_MapSize = 4096;
static const int32_t Assets_TileH = 32;
static const int32_t Assets_NumRegions = 10;
static const int32_t Assets_TileW = 16;
static const int32_t Assets_SectorSize = 32768;
static const int32_t Assets_TerrainSize = 512;
static const int32_t Assets_MaxPathLen = 127;
typedef struct Assets_RegionDef Assets_RegionDef;
struct Assets_RegionDef {
    int32_t image[3 + 1];
    int32_t terra1;
    int32_t terra2;
    int32_t sector;
    int32_t region;
    char name[31 + 1];
};

static const int32_t Assets_MaxImgs = 24;
static void Assets_IntToStr3(int32_t val, char *out, uint32_t out_high);
static void Assets_IntToStr2(int32_t val, char *out, uint32_t out_high);
static void Assets_InitRegionTable(void);
static void Assets_InitAssets(void);
static void Assets_AssetPath(char *name, uint32_t name_high, char *result, uint32_t result_high);
static void Assets_MakePath(char *prefix, uint32_t prefix_high, int32_t num, int32_t digits, char *ext, uint32_t ext_high);
static int Assets_LoadBin(char *path, uint32_t path_high, void * buf, int32_t size);
static void * Assets_LoadImgCached(int32_t num);
static void * Assets_LoadOvlCached(int32_t num);
static void Assets_SetAmigaPal(void * pb);
static void * Assets_LoadPBCached(int32_t num);
static int Assets_PreloadAll(void);
static void Assets_SwitchRegion(int32_t regionIdx);
static int Assets_LoadHUD(int32_t targetW, int32_t targetH);
static int32_t Assets_GetSectorByteForRegion(int32_t x, int32_t y, int32_t regIdx);
static int32_t Assets_GetSectorByte(int32_t x, int32_t y);
static void Assets_SetSectorByte(int32_t x, int32_t y, int32_t val);
static int32_t Assets_GetMapSector(int32_t x, int32_t y);
static void Assets_SetMapSector(int32_t x, int32_t y, int32_t val);
static int32_t Assets_GetTerrainAt(int32_t x, int32_t y);
static int32_t Assets_GetTilesBits(int32_t secByte);
static int32_t Assets_GetMapTag(int32_t secByte);
static int32_t Assets_GetMaskType(int32_t secByte);
static int Assets_IsBlocked(int32_t x, int32_t y);
static int32_t Assets_TerrainSpeedAt(int32_t x, int32_t y);
static int32_t Assets_DetectRegion(int32_t mapX, int32_t mapY);
static void Assets_CheckRegionSwitch(int32_t px, int32_t py);

int32_t Assets_xReg;
int32_t Assets_currentRegion;
int32_t Assets_yReg;
void * Assets_shadowPB;
void * Assets_hudTex;
void * Assets_dragonTex;
Assets_RegionDef Assets_regions[9 + 1];
char Assets_sectorMem[32767 + 1];
char Assets_mapMem[4095 + 1];
char Assets_terraMem[1023 + 1];
void * Assets_tileTex[3 + 1];
void * Assets_tileOverlay[3 + 1];
void * Assets_tilePB[3 + 1];
void * Assets_brotherTex[2 + 1];
void * Assets_enemyTex[4 + 1];
void * Assets_npcTex[5 + 1];
char Assets_pathBuf[127 + 1];
char Assets_numBuf[15 + 1];
char Assets_modeBuf[3 + 1];
char Assets_basePath[63 + 1];
char Assets_sect032[32767 + 1];
char Assets_sect096[32767 + 1];
char Assets_map160[4095 + 1];
char Assets_map168[4095 + 1];
char Assets_map176[4095 + 1];
char Assets_map184[4095 + 1];
char Assets_map192[4095 + 1];
char Assets_allTerr[10 + 1][511 + 1];
int32_t Assets_cachedNum[23 + 1];
void * Assets_cachedTex[23 + 1];
int32_t Assets_cachedCount;
int32_t Assets_ovlNum[23 + 1];
void * Assets_ovlTex[23 + 1];
int32_t Assets_ovlCount;
int32_t Assets_activeSect;
int32_t Assets_activeMap;
int32_t Assets_pbNum[23 + 1];
void * Assets_pbBuf[23 + 1];
int32_t Assets_pbCount;
void * Assets_palRef;
static void Assets_IntToStr3(int32_t val, char *out, uint32_t out_high) {
    (void)val;
    (void)out;
    (void)out_high;
#line 42 "src/Assets.mod"
    out[0] = ((char)((((int32_t)((unsigned char)('0'))) + m2_div(val, 100))));
#line 43
    out[1] = ((char)((((int32_t)((unsigned char)('0'))) + m2_div(m2_mod(val, 100), 10))));
#line 44
    out[2] = ((char)((((int32_t)((unsigned char)('0'))) + m2_mod(val, 10))));
#line 45
    out[3] = '\0';
    return;
}

static void Assets_IntToStr2(int32_t val, char *out, uint32_t out_high) {
    (void)val;
    (void)out;
    (void)out_high;
#line 50
    out[0] = ((char)((((int32_t)((unsigned char)('0'))) + m2_div(val, 10))));
#line 51
    out[1] = ((char)((((int32_t)((unsigned char)('0'))) + m2_mod(val, 10))));
#line 52
    out[2] = '\0';
    return;
}

static void Assets_InitRegionTable(void) {
#line 57
    Assets_regions[0].image[0] = 320;
    Assets_regions[0].image[1] = 480;
    Assets_regions[0].image[2] = 520;
    Assets_regions[0].image[3] = 560;
#line 58
    Assets_regions[0].terra1 = 0;
    Assets_regions[0].terra2 = 1;
    Assets_regions[0].sector = 32;
    Assets_regions[0].region = 160;
#line 59
    m2_Strings_Assign("Snowy Region", Assets_regions[0].name, 31);
#line 60
    Assets_regions[1].image[0] = 320;
    Assets_regions[1].image[1] = 360;
    Assets_regions[1].image[2] = 400;
    Assets_regions[1].image[3] = 440;
#line 61
    Assets_regions[1].terra1 = 2;
    Assets_regions[1].terra2 = 3;
    Assets_regions[1].sector = 32;
    Assets_regions[1].region = 160;
#line 62
    m2_Strings_Assign("Witch Woods", Assets_regions[1].name, 31);
#line 63
    Assets_regions[2].image[0] = 320;
    Assets_regions[2].image[1] = 360;
    Assets_regions[2].image[2] = 520;
    Assets_regions[2].image[3] = 560;
#line 64
    Assets_regions[2].terra1 = 2;
    Assets_regions[2].terra2 = 1;
    Assets_regions[2].sector = 32;
    Assets_regions[2].region = 168;
#line 65
    m2_Strings_Assign("Swamp Region", Assets_regions[2].name, 31);
#line 66
    Assets_regions[3].image[0] = 320;
    Assets_regions[3].image[1] = 360;
    Assets_regions[3].image[2] = 400;
    Assets_regions[3].image[3] = 440;
#line 67
    Assets_regions[3].terra1 = 2;
    Assets_regions[3].terra2 = 3;
    Assets_regions[3].sector = 32;
    Assets_regions[3].region = 168;
#line 68
    m2_Strings_Assign("Plains Rocks", Assets_regions[3].name, 31);
#line 69
    Assets_regions[4].image[0] = 320;
    Assets_regions[4].image[1] = 480;
    Assets_regions[4].image[2] = 520;
    Assets_regions[4].image[3] = 600;
#line 70
    Assets_regions[4].terra1 = 0;
    Assets_regions[4].terra2 = 4;
    Assets_regions[4].sector = 32;
    Assets_regions[4].region = 176;
#line 71
    m2_Strings_Assign("Desert Area", Assets_regions[4].name, 31);
#line 72
    Assets_regions[5].image[0] = 320;
    Assets_regions[5].image[1] = 280;
    Assets_regions[5].image[2] = 240;
    Assets_regions[5].image[3] = 200;
#line 73
    Assets_regions[5].terra1 = 5;
    Assets_regions[5].terra2 = 6;
    Assets_regions[5].sector = 32;
    Assets_regions[5].region = 176;
#line 74
    m2_Strings_Assign("Bay City Farms", Assets_regions[5].name, 31);
#line 75
    Assets_regions[6].image[0] = 320;
    Assets_regions[6].image[1] = 640;
    Assets_regions[6].image[2] = 520;
    Assets_regions[6].image[3] = 600;
#line 76
    Assets_regions[6].terra1 = 7;
    Assets_regions[6].terra2 = 4;
    Assets_regions[6].sector = 32;
    Assets_regions[6].region = 184;
#line 77
    m2_Strings_Assign("Volcanic", Assets_regions[6].name, 31);
#line 78
    Assets_regions[7].image[0] = 320;
    Assets_regions[7].image[1] = 280;
    Assets_regions[7].image[2] = 240;
    Assets_regions[7].image[3] = 200;
#line 79
    Assets_regions[7].terra1 = 5;
    Assets_regions[7].terra2 = 6;
    Assets_regions[7].sector = 32;
    Assets_regions[7].region = 184;
#line 80
    m2_Strings_Assign("Forest Wilderness", Assets_regions[7].name, 31);
#line 81
    Assets_regions[8].image[0] = 680;
    Assets_regions[8].image[1] = 720;
    Assets_regions[8].image[2] = 800;
    Assets_regions[8].image[3] = 840;
#line 82
    Assets_regions[8].terra1 = 8;
    Assets_regions[8].terra2 = 9;
    Assets_regions[8].sector = 96;
    Assets_regions[8].region = 192;
#line 83
    m2_Strings_Assign("Inside Buildings", Assets_regions[8].name, 31);
#line 84
    Assets_regions[9].image[0] = 680;
    Assets_regions[9].image[1] = 760;
    Assets_regions[9].image[2] = 800;
    Assets_regions[9].image[3] = 840;
#line 85
    Assets_regions[9].terra1 = 10;
    Assets_regions[9].terra2 = 9;
    Assets_regions[9].sector = 96;
    Assets_regions[9].region = 192;
#line 86
    m2_Strings_Assign("Dungeons Caves", Assets_regions[9].name, 31);
    return;
}

static void Assets_InitAssets(void) {
    int32_t i;
#line 92
    Assets_currentRegion = (-1);
#line 93
    Assets_hudTex = NULL;
#line 94
    Assets_xReg = 0;
    Assets_yReg = 0;
#line 95
    Assets_activeSect = 0;
    Assets_activeMap = 0;
#line 96
    Assets_cachedCount = 0;
#line 97
    Assets_ovlCount = 0;
#line 98
    Assets_shadowPB = NULL;
#line 99
    Assets_pbCount = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
#line 100
    Assets_tilePB[i] = NULL;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 101
    Assets_palRef = PixBuf_Create(1, 1);
    if (Assets_palRef != NULL) goto L6; else goto L5;
  L5:;
#line 103
    Assets_InitRegionTable();
#line 104
    m2_Strings_Assign("rb", Assets_modeBuf, 3);
    i = 0;
    goto L7;
  L6:;
#line 102
    Assets_SetAmigaPal(Assets_palRef);
    goto L5;
  L7:;
    if (i <= 3) goto L8; else goto L10;
  L8:;
#line 105
    Assets_tileTex[i] = NULL;
    goto L9;
  L9:;
    i = (i + 1);
    goto L7;
  L10:;
    i = 0;
    goto L11;
  L11:;
    if (i <= 3) goto L12; else goto L14;
  L12:;
#line 106
    Assets_tileOverlay[i] = NULL;
    goto L13;
  L13:;
    i = (i + 1);
    goto L11;
  L14:;
    i = 0;
    goto L15;
  L15:;
    if (i <= 2) goto L16; else goto L18;
  L16:;
#line 107
    Assets_brotherTex[i] = NULL;
    goto L17;
  L17:;
    i = (i + 1);
    goto L15;
  L18:;
    i = 0;
    goto L19;
  L19:;
    if (i <= 4) goto L20; else goto L22;
  L20:;
#line 108
    Assets_enemyTex[i] = NULL;
    goto L21;
  L21:;
    i = (i + 1);
    goto L19;
  L22:;
    i = 0;
    goto L23;
  L23:;
    if (i <= 4) goto L24; else goto L26;
  L24:;
#line 109
    Assets_npcTex[i] = NULL;
    goto L25;
  L25:;
    i = (i + 1);
    goto L23;
  L26:;
#line 110
    Assets_dragonTex = NULL;
    if (m2sys_file_exists(((void *)"assets/hiscreen.bmp")) == 1) goto L28; else goto L29;
  L27:;
    return;
  L28:;
#line 112
    m2_Strings_Assign("assets/", Assets_basePath, 63);
    goto L27;
  L29:;
    if (m2sys_file_exists(((void *)"../../assets/hiscreen.bmp")) == 1) goto L30; else goto L31;
  L30:;
#line 114
    m2_Strings_Assign("../../assets/", Assets_basePath, 63);
    goto L27;
  L31:;
#line 116
    m2_Strings_Assign("assets/", Assets_basePath, 63);
    goto L27;
}

static void Assets_AssetPath(char *name, uint32_t name_high, char *result, uint32_t result_high) {
    (void)name;
    (void)name_high;
    (void)result;
    (void)result_high;
#line 122
    m2_Strings_Assign(Assets_basePath, result, result_high);
#line 123
    m2_Strings_Concat(result, name, result, result_high);
    return;
}

static void Assets_MakePath(char *prefix, uint32_t prefix_high, int32_t num, int32_t digits, char *ext, uint32_t ext_high) {
    (void)prefix;
    (void)prefix_high;
    (void)num;
    (void)digits;
    (void)ext;
    (void)ext_high;
#line 129
    m2_Strings_Assign(Assets_basePath, Assets_pathBuf, 127);
#line 130
    m2_Strings_Concat(Assets_pathBuf, prefix, Assets_pathBuf, 127);
    if (digits == 3) goto L2; else goto L3;
  L1:;
#line 134
    m2_Strings_Concat(Assets_pathBuf, Assets_numBuf, Assets_pathBuf, 127);
#line 135
    m2_Strings_Concat(Assets_pathBuf, ext, Assets_pathBuf, 127);
    return;
  L2:;
#line 131
    Assets_IntToStr3(num, Assets_numBuf, 15);
    goto L1;
  L3:;
#line 132
    Assets_IntToStr2(num, Assets_numBuf, 15);
    goto L1;
}

static int Assets_LoadBin(char *path, uint32_t path_high, void * buf, int32_t size) {
    (void)path;
    (void)path_high;
    (void)buf;
    (void)size;
    int32_t fd;
    int32_t n;
#line 141
    fd = m2sys_fopen(((void *)(path)), ((void *)&(Assets_modeBuf)));
    if (fd < 0) goto L2; else goto L1;
  L1:;
#line 146
    n = m2sys_fread_bytes(fd, buf, size);
#line 147
    m2sys_fclose(fd);
    return n >= size;
  L2:;
#line 143
    m2_WriteString("  FAILED: ");
    m2_WriteString(path);
    m2_WriteLn();
    return 0;
}

static void * Assets_LoadImgCached(int32_t num) {
    (void)num;
    int32_t i;
    void * tex;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Assets_cachedCount - 1)) goto L2; else goto L4;
  L2:;
    if (Assets_cachedNum[i] == num) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 158
    Assets_MakePath("image_", 6, num, 3, ".bmp", 4);
#line 159
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
#line 160
    tex = Platform_LoadBMPTexture(Assets_pathBuf, 127);
    if (tex != NULL) goto L9; else goto L7;
  L5:;
    goto L3;
  L6:;
    return Assets_cachedTex[i];
  L7:;
    return tex;
  L8:;
#line 162
    Assets_cachedNum[Assets_cachedCount] = num;
#line 163
    Assets_cachedTex[Assets_cachedCount] = tex;
#line 164
    (Assets_cachedCount++);
    goto L7;
  L9:;
    if (Assets_cachedCount < 24) goto L8; else goto L7;
}

static void * Assets_LoadOvlCached(int32_t num) {
    (void)num;
    int32_t i;
    void * tex;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Assets_ovlCount - 1)) goto L2; else goto L4;
  L2:;
    if (Assets_ovlNum[i] == num) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 176
    Assets_MakePath("image_", 6, num, 3, ".bmp", 4);
#line 178
    tex = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 0, 0, 0);
    if (tex != NULL) goto L9; else goto L7;
  L5:;
    goto L3;
  L6:;
    return Assets_ovlTex[i];
  L7:;
    return tex;
  L8:;
#line 180
    Assets_ovlNum[Assets_ovlCount] = num;
#line 181
    Assets_ovlTex[Assets_ovlCount] = tex;
#line 182
    (Assets_ovlCount++);
    goto L7;
  L9:;
    if (Assets_ovlCount < 24) goto L8; else goto L7;
}

static void Assets_SetAmigaPal(void * pb) {
    (void)pb;
#line 195
    PixBuf_SetPal(pb, 0, 0, 0, 0);
#line 196
    PixBuf_SetPal(pb, 1, 255, 255, 255);
#line 197
    PixBuf_SetPal(pb, 2, 238, 153, 102);
#line 198
    PixBuf_SetPal(pb, 3, 187, 102, 51);
#line 199
    PixBuf_SetPal(pb, 4, 102, 51, 17);
#line 200
    PixBuf_SetPal(pb, 5, 119, 187, 255);
#line 201
    PixBuf_SetPal(pb, 6, 51, 51, 51);
#line 202
    PixBuf_SetPal(pb, 7, 221, 187, 136);
#line 203
    PixBuf_SetPal(pb, 8, 34, 34, 51);
#line 204
    PixBuf_SetPal(pb, 9, 68, 68, 85);
#line 205
    PixBuf_SetPal(pb, 10, 136, 136, 153);
#line 206
    PixBuf_SetPal(pb, 11, 187, 187, 204);
#line 207
    PixBuf_SetPal(pb, 12, 85, 34, 17);
#line 208
    PixBuf_SetPal(pb, 13, 153, 68, 17);
#line 209
    PixBuf_SetPal(pb, 14, 255, 136, 34);
#line 210
    PixBuf_SetPal(pb, 15, 255, 204, 119);
#line 211
    PixBuf_SetPal(pb, 16, 0, 68, 0);
#line 212
    PixBuf_SetPal(pb, 17, 0, 119, 0);
#line 213
    PixBuf_SetPal(pb, 18, 0, 187, 0);
#line 214
    PixBuf_SetPal(pb, 19, 102, 255, 102);
#line 215
    PixBuf_SetPal(pb, 20, 0, 0, 85);
#line 216
    PixBuf_SetPal(pb, 21, 0, 0, 153);
#line 217
    PixBuf_SetPal(pb, 22, 0, 0, 221);
#line 218
    PixBuf_SetPal(pb, 23, 51, 119, 255);
#line 219
    PixBuf_SetPal(pb, 24, 204, 0, 0);
#line 220
    PixBuf_SetPal(pb, 25, 255, 85, 0);
#line 221
    PixBuf_SetPal(pb, 26, 255, 170, 0);
#line 222
    PixBuf_SetPal(pb, 27, 255, 255, 102);
#line 223
    PixBuf_SetPal(pb, 28, 238, 187, 102);
#line 224
    PixBuf_SetPal(pb, 29, 238, 170, 85);
#line 225
    PixBuf_SetPal(pb, 30, 0, 0, 255);
#line 226
    PixBuf_SetPal(pb, 31, 187, 221, 255);
    return;
}

static void * Assets_LoadPBCached(int32_t num) {
    (void)num;
    int32_t i;
    void * pb;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Assets_pbCount - 1)) goto L2; else goto L4;
  L2:;
    if (Assets_pbNum[i] == num) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 236
    Assets_MakePath("image_", 6, num, 3, ".png", 4);
    if (Assets_palRef != NULL) goto L8; else goto L9;
  L5:;
    goto L3;
  L6:;
    return Assets_pbBuf[i];
  L7:;
    if (pb != NULL) goto L12; else goto L10;
  L8:;
#line 238
    pb = PixBuf_LoadPNGPal(Assets_pathBuf, 127, Assets_palRef, 32);
    goto L7;
  L9:;
#line 240
    pb = PixBuf_LoadPNG(Assets_pathBuf, 127, 32);
    goto L7;
  L10:;
    return pb;
  L11:;
#line 243
    Assets_pbNum[Assets_pbCount] = num;
#line 244
    Assets_pbBuf[Assets_pbCount] = pb;
#line 245
    (Assets_pbCount++);
    goto L10;
  L12:;
    if (Assets_pbCount < 24) goto L11; else goto L10;
}

static int Assets_PreloadAll(void) {
    int32_t i;
    int32_t j;
#line 253
    m2_WriteString("Preloading all game assets...");
    m2_WriteLn();
#line 255
    Assets_MakePath("sector_", 7, 32, 3, ".bin", 4);
#line 256
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_sect032)), 32768)) goto L1; else goto L2;
  L1:;
#line 258
    Assets_MakePath("sector_", 7, 96, 3, ".bin", 4);
#line 259
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_sect096)), 32768)) goto L3; else goto L4;
  L2:;
    return 0;
  L3:;
#line 262
    Assets_MakePath("map_", 4, 160, 3, ".bin", 4);
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_map160)), 4096)) goto L5; else goto L6;
  L4:;
    return 0;
  L5:;
#line 264
    Assets_MakePath("map_", 4, 168, 3, ".bin", 4);
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_map168)), 4096)) goto L7; else goto L8;
  L6:;
    return 0;
  L7:;
#line 266
    Assets_MakePath("map_", 4, 176, 3, ".bin", 4);
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_map176)), 4096)) goto L9; else goto L10;
  L8:;
    return 0;
  L9:;
#line 268
    Assets_MakePath("map_", 4, 184, 3, ".bin", 4);
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_map184)), 4096)) goto L11; else goto L12;
  L10:;
    return 0;
  L11:;
#line 270
    Assets_MakePath("map_", 4, 192, 3, ".bin", 4);
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_map192)), 4096)) goto L13; else goto L14;
  L12:;
    return 0;
  L13:;
    i = 0;
    goto L15;
  L14:;
    return 0;
  L15:;
    if (i <= 10) goto L16; else goto L18;
  L16:;
#line 274
    Assets_MakePath("terrain_", 8, i, 2, ".bin", 4);
    m2_WriteString("  ");
    m2_WriteString(Assets_pathBuf);
    m2_WriteLn();
    if (Assets_LoadBin(Assets_pathBuf, 127, ((void *)&(Assets_allTerr[i])), 512)) goto L19; else goto L20;
  L17:;
    i = (i + 1);
    goto L15;
  L18:;
    i = 0;
    goto L21;
  L19:;
    goto L17;
  L20:;
    return 0;
  L21:;
    if (i <= (10 - 1)) goto L22; else goto L24;
  L22:;
    j = 0;
    goto L25;
  L23:;
    i = (i + 1);
    goto L21;
  L24:;
#line 289
    Assets_AssetPath("shadow_mem.png", 14, Assets_pathBuf, 127);
#line 290
    Assets_shadowPB = PixBuf_LoadPNG(Assets_pathBuf, 127, 256);
    if (Assets_shadowPB == NULL) goto L34; else goto L35;
  L25:;
    if (j <= 3) goto L26; else goto L28;
  L26:;
    if (Assets_LoadImgCached(Assets_regions[i].image[j]) == NULL) goto L30; else goto L29;
  L27:;
    j = (j + 1);
    goto L25;
  L28:;
    goto L23;
  L29:;
    if (Assets_LoadOvlCached(Assets_regions[i].image[j]) == NULL) goto L32; else goto L31;
  L30:;
    return 0;
  L31:;
    goto L27;
  L32:;
#line 283
    m2_WriteString("overlay load failed");
    m2_WriteLn();
    goto L31;
  L33:;
#line 298
    Assets_AssetPath("julian.bmp", 10, Assets_pathBuf, 127);
#line 299
    Assets_brotherTex[0] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 300
    Assets_AssetPath("phillip.bmp", 11, Assets_pathBuf, 127);
#line 301
    Assets_brotherTex[1] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 302
    Assets_AssetPath("kevin.bmp", 9, Assets_pathBuf, 127);
#line 303
    Assets_brotherTex[2] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
    if (Assets_brotherTex[0] == NULL) goto L37; else goto L38;
  L34:;
#line 292
    m2_WriteString("*** Shadow mask failed ***");
    m2_WriteLn();
    goto L33;
  L35:;
#line 294
    m2_WriteString("Shadow mask loaded");
    m2_WriteLn();
    goto L33;
  L36:;
#line 311
    Assets_AssetPath("shape_6_Ogre_16x32_x64.bmp", 26, Assets_pathBuf, 127);
#line 312
    Assets_enemyTex[0] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 313
    Assets_AssetPath("shape_7_Ghost_16x32_x64.bmp", 27, Assets_pathBuf, 127);
#line 314
    Assets_enemyTex[1] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 315
    Assets_AssetPath("shape_8_DKnight-Spiders_16x32_x64.bmp", 37, Assets_pathBuf, 127);
#line 316
    Assets_enemyTex[2] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 317
    Assets_AssetPath("shape_9_Necro-Farmer-Loraii_16x32_x64.bmp", 41, Assets_pathBuf, 127);
#line 318
    Assets_enemyTex[3] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 319
    Assets_AssetPath("shape_12_Snake-Salamander_16x32_x64.bmp", 39, Assets_pathBuf, 127);
#line 320
    Assets_enemyTex[4] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
    if (Assets_enemyTex[0] != NULL) goto L40; else goto L39;
  L37:;
#line 305
    m2_WriteString("*** Brother sprites failed ***");
    m2_WriteLn();
    goto L36;
  L38:;
#line 307
    m2_WriteString("Brother sprites loaded");
    m2_WriteLn();
    goto L36;
  L39:;
#line 326
    Assets_AssetPath("shape_13_Wizard-Priest_16x32_x8.bmp", 35, Assets_pathBuf, 127);
#line 327
    Assets_npcTex[0] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 328
    Assets_AssetPath("shape_14_Royal-Set_16x32_x8.bmp", 31, Assets_pathBuf, 127);
#line 329
    Assets_npcTex[1] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 330
    Assets_AssetPath("shape_15_Bartender_16x32_x8.bmp", 31, Assets_pathBuf, 127);
#line 331
    Assets_npcTex[2] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 332
    Assets_AssetPath("shape_16_Witch_16x32_x8.bmp", 27, Assets_pathBuf, 127);
#line 333
    Assets_npcTex[3] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 334
    Assets_AssetPath("shape_17_Ranger-Beggar_16x32_x8.bmp", 35, Assets_pathBuf, 127);
#line 335
    Assets_npcTex[4] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
#line 336
    Assets_AssetPath("scroll_priest_16x32.bmp", 23, Assets_pathBuf, 127);
#line 337
    Assets_npcTex[5] = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
    if (Assets_npcTex[0] != NULL) goto L42; else goto L41;
  L40:;
#line 322
    m2_WriteString("Enemy sprites loaded");
    m2_WriteLn();
    goto L39;
  L41:;
#line 343
    Assets_AssetPath("shape_10_Dragon_48x40_x5.bmp", 28, Assets_pathBuf, 127);
#line 344
    Assets_dragonTex = Platform_LoadBMPKeyedTexture(Assets_pathBuf, 127, 255, 0, 255);
    if (Assets_dragonTex != NULL) goto L44; else goto L43;
  L42:;
#line 339
    m2_WriteString("NPC sprites loaded");
    m2_WriteLn();
    goto L41;
  L43:;
#line 349
    m2_WriteString("Done: ");
    m2_WriteInt(Assets_cachedCount, 1);
#line 350
    m2_WriteString(" textures cached");
    m2_WriteLn();
    return 1;
  L44:;
#line 346
    m2_WriteString("Dragon sprite loaded");
    m2_WriteLn();
    goto L43;
}

static void Assets_SwitchRegion(int32_t regionIdx) {
    (void)regionIdx;
    int32_t i;
    if (regionIdx < 0) goto L2; else goto L3;
  L1:;
    if (regionIdx == Assets_currentRegion) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (regionIdx >= 10) goto L2; else goto L1;
  L4:;
    if (Assets_regions[regionIdx].sector == 96) goto L7; else goto L8;
  L5:;
    return;
  L6:;
    if ((Assets_regions[regionIdx].region == 160)) goto L11;
    if ((Assets_regions[regionIdx].region == 168)) goto L12;
    if ((Assets_regions[regionIdx].region == 176)) goto L13;
    if ((Assets_regions[regionIdx].region == 184)) goto L14;
    if ((Assets_regions[regionIdx].region == 192)) goto L15;
    goto L10;
  L7:;
#line 362
    Assets_activeSect = 1;
    goto L6;
  L8:;
#line 363
    Assets_activeSect = 0;
    goto L6;
  L9:;
    i = 0;
    goto L16;
  L10:;
#line 374
    Assets_activeMap = 0;
    goto L9;
  L11:;
#line 368
    Assets_activeMap = 0;
    goto L9;
  L12:;
#line 369
    Assets_activeMap = 1;
    goto L9;
  L13:;
#line 370
    Assets_activeMap = 2;
    goto L9;
  L14:;
#line 371
    Assets_activeMap = 3;
    goto L9;
  L15:;
#line 372
    Assets_activeMap = 4;
    goto L9;
  L16:;
    if (i <= (512 - 1)) goto L17; else goto L19;
  L17:;
#line 379
    Assets_terraMem[i] = Assets_allTerr[Assets_regions[regionIdx].terra1][i];
#line 380
    Assets_terraMem[(512 + i)] = Assets_allTerr[Assets_regions[regionIdx].terra2][i];
    goto L18;
  L18:;
    i = (i + 1);
    goto L16;
  L19:;
    i = 0;
    goto L20;
  L20:;
    if (i <= 3) goto L21; else goto L23;
  L21:;
#line 385
    Assets_tileTex[i] = Assets_LoadImgCached(Assets_regions[regionIdx].image[i]);
#line 386
    Assets_tileOverlay[i] = Assets_LoadOvlCached(Assets_regions[regionIdx].image[i]);
#line 387
    Assets_tilePB[i] = Assets_LoadPBCached(Assets_regions[regionIdx].image[i]);
    goto L22;
  L22:;
    i = (i + 1);
    goto L20;
  L23:;
#line 390
    Assets_currentRegion = regionIdx;
    if (regionIdx < 8) goto L25; else goto L26;
  L24:;
    if (Assets_activeSect == 1) goto L28; else goto L29;
  L25:;
#line 392
    Assets_xReg = (m2_mod(regionIdx, 2) * 64);
#line 393
    Assets_yReg = (m2_div(regionIdx, 2) * 32);
    goto L24;
  L26:;
#line 395
    Assets_xReg = 0;
#line 396
    Assets_yReg = (m2_div(regionIdx, 2) * 32);
    goto L24;
  L27:;
    if ((Assets_activeMap == 0)) goto L32;
    if ((Assets_activeMap == 1)) goto L33;
    if ((Assets_activeMap == 2)) goto L34;
    if ((Assets_activeMap == 3)) goto L35;
    if ((Assets_activeMap == 4)) goto L36;
    goto L31;
  L28:;
#line 401
    memcpy(Assets_sectorMem, Assets_sect096, sizeof(Assets_sectorMem));
    goto L27;
  L29:;
#line 403
    memcpy(Assets_sectorMem, Assets_sect032, sizeof(Assets_sectorMem));
    goto L27;
  L30:;
    return;
  L31:;
    goto L30;
  L32:;
#line 406
    memcpy(Assets_mapMem, Assets_map160, sizeof(Assets_mapMem));
    goto L30;
  L33:;
#line 407
    memcpy(Assets_mapMem, Assets_map168, sizeof(Assets_mapMem));
    goto L30;
  L34:;
#line 408
    memcpy(Assets_mapMem, Assets_map176, sizeof(Assets_mapMem));
    goto L30;
  L35:;
#line 409
    memcpy(Assets_mapMem, Assets_map184, sizeof(Assets_mapMem));
    goto L30;
  L36:;
#line 410
    memcpy(Assets_mapMem, Assets_map192, sizeof(Assets_mapMem));
    goto L30;
}

static int Assets_LoadHUD(int32_t targetW, int32_t targetH) {
    (void)targetW;
    (void)targetH;
#line 417
    m2_Strings_Assign(Assets_basePath, Assets_pathBuf, 127);
#line 418
    m2_Strings_Concat(Assets_pathBuf, "hiscreen.bmp", Assets_pathBuf, 127);
#line 421
    Assets_hudTex = Platform_LoadBMPScaled(Assets_pathBuf, 127, targetW, targetH);
    return Assets_hudTex != NULL;
}

static int32_t Assets_GetSectorByteForRegion(int32_t x, int32_t y, int32_t regIdx) {
    (void)x;
    (void)y;
    (void)regIdx;
    int32_t imx;
    int32_t imy;
    int32_t secx;
    int32_t secy;
    int32_t secNum;
    int32_t offset;
    int32_t rxr;
    int32_t ryr;
    if (regIdx < 0) goto L2; else goto L3;
  L1:;
#line 430
    imx = m2_div(x, 16);
#line 431
    imy = m2_div(y, 32);
    if (regIdx < 8) goto L5; else goto L6;
  L2:;
    return 0;
  L3:;
    if (regIdx >= 10) goto L2; else goto L1;
  L4:;
#line 442
    secx = (m2_div(imx, 16) - rxr);
    if (secx < 0) goto L8; else goto L9;
  L5:;
#line 435
    rxr = (m2_mod(regIdx, 2) * 64);
#line 436
    ryr = (m2_div(regIdx, 2) * 32);
    goto L4;
  L6:;
#line 438
    rxr = 0;
#line 439
    ryr = (m2_div(regIdx, 2) * 32);
    goto L4;
  L7:;
#line 450
    secy = (m2_div(imy, 8) - ryr);
    if (secy < 0) goto L14; else goto L13;
  L8:;
    if (((uint32_t)(((uint32_t)(secx))) & (uint32_t)(32)) != 0) goto L11; else goto L12;
  L9:;
    if (secx >= 64) goto L8; else goto L7;
  L10:;
    goto L7;
  L11:;
#line 445
    secx = 0;
    goto L10;
  L12:;
#line 447
    secx = 63;
    goto L10;
  L13:;
    if (secy >= 32) goto L16; else goto L15;
  L14:;
#line 451
    secy = 0;
    goto L13;
  L15:;
#line 454
    offset = (((secy * 128) + secx) + rxr);
    if (offset < 0) goto L18; else goto L19;
  L16:;
#line 452
    secy = 31;
    goto L15;
  L17:;
    if ((Assets_regions[regIdx].region == 160)) goto L22;
    if ((Assets_regions[regIdx].region == 168)) goto L23;
    if ((Assets_regions[regIdx].region == 176)) goto L24;
    if ((Assets_regions[regIdx].region == 184)) goto L25;
    if ((Assets_regions[regIdx].region == 192)) goto L26;
    goto L21;
  L18:;
    return 0;
  L19:;
    if (offset >= 4096) goto L18; else goto L17;
  L20:;
#line 465
    offset = (((secNum * 128) + (m2_mod(imy, 8) * 16)) + m2_mod(imx, 16));
    if (offset < 0) goto L28; else goto L29;
  L21:;
#line 463
    secNum = 0;
    goto L20;
  L22:;
#line 457
    secNum = ((int32_t)((unsigned char)(Assets_map160[offset])));
    goto L20;
  L23:;
#line 458
    secNum = ((int32_t)((unsigned char)(Assets_map168[offset])));
    goto L20;
  L24:;
#line 459
    secNum = ((int32_t)((unsigned char)(Assets_map176[offset])));
    goto L20;
  L25:;
#line 460
    secNum = ((int32_t)((unsigned char)(Assets_map184[offset])));
    goto L20;
  L26:;
#line 461
    secNum = ((int32_t)((unsigned char)(Assets_map192[offset])));
    goto L20;
  L27:;
    if (Assets_regions[regIdx].sector == 96) goto L31; else goto L32;
  L28:;
    return 0;
  L29:;
    if (offset >= 32768) goto L28; else goto L27;
  L30:;
    return 0;
  L31:;
    return ((int32_t)((unsigned char)(Assets_sect096[offset])));
  L32:;
    return ((int32_t)((unsigned char)(Assets_sect032[offset])));
}

static int32_t Assets_GetSectorByte(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    int32_t imx;
    int32_t imy;
    int32_t secx;
    int32_t secy;
    int32_t secNum;
    int32_t offset;
#line 477
    imx = m2_div(x, 16);
#line 478
    imy = m2_div(y, 32);
#line 484
    secx = (m2_div(imx, 16) - Assets_xReg);
    if (secx < 0) goto L2; else goto L3;
  L1:;
#line 495
    secy = (m2_div(imy, 8) - Assets_yReg);
    if (secy < 0) goto L8; else goto L7;
  L2:;
    if (((uint32_t)(((uint32_t)(secx))) & (uint32_t)(32)) != 0) goto L5; else goto L6;
  L3:;
    if (secx >= 64) goto L2; else goto L1;
  L4:;
    goto L1;
  L5:;
#line 489
    secx = 0;
    goto L4;
  L6:;
#line 491
    secx = 63;
    goto L4;
  L7:;
    if (secy >= 32) goto L10; else goto L9;
  L8:;
#line 496
    secy = 0;
    goto L7;
  L9:;
#line 499
    offset = (((secy * 128) + secx) + Assets_xReg);
    if (offset < 0) goto L12; else goto L13;
  L10:;
#line 497
    secy = 31;
    goto L9;
  L11:;
    if ((Assets_activeMap == 0)) goto L16;
    if ((Assets_activeMap == 1)) goto L17;
    if ((Assets_activeMap == 2)) goto L18;
    if ((Assets_activeMap == 3)) goto L19;
    if ((Assets_activeMap == 4)) goto L20;
    goto L15;
  L12:;
    return 0;
  L13:;
    if (offset >= 4096) goto L12; else goto L11;
  L14:;
#line 511
    offset = (((secNum * 128) + (m2_mod(imy, 8) * 16)) + m2_mod(imx, 16));
    if (offset < 0) goto L22; else goto L23;
  L15:;
#line 509
    secNum = 0;
    goto L14;
  L16:;
#line 503
    secNum = ((int32_t)((unsigned char)(Assets_map160[offset])));
    goto L14;
  L17:;
#line 504
    secNum = ((int32_t)((unsigned char)(Assets_map168[offset])));
    goto L14;
  L18:;
#line 505
    secNum = ((int32_t)((unsigned char)(Assets_map176[offset])));
    goto L14;
  L19:;
#line 506
    secNum = ((int32_t)((unsigned char)(Assets_map184[offset])));
    goto L14;
  L20:;
#line 507
    secNum = ((int32_t)((unsigned char)(Assets_map192[offset])));
    goto L14;
  L21:;
    if (Assets_activeSect == 1) goto L25; else goto L26;
  L22:;
    return 0;
  L23:;
    if (offset >= 32768) goto L22; else goto L21;
  L24:;
    return 0;
  L25:;
    return ((int32_t)((unsigned char)(Assets_sect096[offset])));
  L26:;
    return ((int32_t)((unsigned char)(Assets_sect032[offset])));
}

static void Assets_SetSectorByte(int32_t x, int32_t y, int32_t val) {
    (void)x;
    (void)y;
    (void)val;
    int32_t imx;
    int32_t imy;
    int32_t secx;
    int32_t secy;
    int32_t secNum;
    int32_t offset;
#line 524
    imx = m2_div(x, 16);
#line 525
    imy = m2_div(y, 32);
#line 526
    secx = (m2_div(imx, 16) - Assets_xReg);
    if (secx < 0) goto L2; else goto L3;
  L1:;
#line 532
    secy = (m2_div(imy, 8) - Assets_yReg);
    if (secy < 0) goto L8; else goto L7;
  L2:;
    if (((uint32_t)(((uint32_t)(secx))) & (uint32_t)(32)) != 0) goto L5; else goto L6;
  L3:;
    if (secx >= 64) goto L2; else goto L1;
  L4:;
    goto L1;
  L5:;
#line 528
    secx = 0;
    goto L4;
  L6:;
#line 529
    secx = 63;
    goto L4;
  L7:;
    if (secy >= 32) goto L10; else goto L9;
  L8:;
#line 533
    secy = 0;
    goto L7;
  L9:;
#line 535
    offset = (((secy * 128) + secx) + Assets_xReg);
    if (offset < 0) goto L12; else goto L13;
  L10:;
#line 534
    secy = 31;
    goto L9;
  L11:;
    if ((Assets_activeMap == 0)) goto L16;
    if ((Assets_activeMap == 1)) goto L17;
    if ((Assets_activeMap == 2)) goto L18;
    if ((Assets_activeMap == 3)) goto L19;
    if ((Assets_activeMap == 4)) goto L20;
    goto L15;
  L12:;
    return;
  L13:;
    if (offset >= 4096) goto L12; else goto L11;
  L14:;
#line 545
    offset = (((secNum * 128) + (m2_mod(imy, 8) * 16)) + m2_mod(imx, 16));
    if (offset < 0) goto L22; else goto L23;
  L15:;
    return;
  L16:;
#line 538
    secNum = ((int32_t)((unsigned char)(Assets_map160[offset])));
    goto L14;
  L17:;
#line 539
    secNum = ((int32_t)((unsigned char)(Assets_map168[offset])));
    goto L14;
  L18:;
#line 540
    secNum = ((int32_t)((unsigned char)(Assets_map176[offset])));
    goto L14;
  L19:;
#line 541
    secNum = ((int32_t)((unsigned char)(Assets_map184[offset])));
    goto L14;
  L20:;
#line 542
    secNum = ((int32_t)((unsigned char)(Assets_map192[offset])));
    goto L14;
  L21:;
    if (Assets_activeSect == 1) goto L25; else goto L26;
  L22:;
    return;
  L23:;
    if (offset >= 32768) goto L22; else goto L21;
  L24:;
    return;
  L25:;
#line 548
    Assets_sect096[offset] = ((char)(val));
    goto L24;
  L26:;
#line 550
    Assets_sect032[offset] = ((char)(val));
    goto L24;
}

static int32_t Assets_GetMapSector(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    int32_t imx;
    int32_t imy;
    int32_t secx;
    int32_t secy;
    int32_t offset;
    int32_t secNum;
#line 557
    imx = m2_div(x, 16);
#line 558
    imy = m2_div(y, 32);
#line 559
    secx = (m2_div(imx, 16) - Assets_xReg);
    if (secx < 0) goto L2; else goto L3;
  L1:;
#line 567
    secy = (m2_div(imy, 8) - Assets_yReg);
    if (secy < 0) goto L8; else goto L7;
  L2:;
    if (((uint32_t)(((uint32_t)(secx))) & (uint32_t)(32)) != 0) goto L5; else goto L6;
  L3:;
    if (secx >= 64) goto L2; else goto L1;
  L4:;
    goto L1;
  L5:;
#line 562
    secx = 0;
    goto L4;
  L6:;
#line 564
    secx = 63;
    goto L4;
  L7:;
    if (secy >= 32) goto L10; else goto L9;
  L8:;
#line 568
    secy = 0;
    goto L7;
  L9:;
#line 570
    offset = (((secy * 128) + secx) + Assets_xReg);
    if (offset < 0) goto L12; else goto L13;
  L10:;
#line 569
    secy = 31;
    goto L9;
  L11:;
    if ((Assets_activeMap == 0)) goto L16;
    if ((Assets_activeMap == 1)) goto L17;
    if ((Assets_activeMap == 2)) goto L18;
    if ((Assets_activeMap == 3)) goto L19;
    if ((Assets_activeMap == 4)) goto L20;
    goto L15;
  L12:;
    return 0;
  L13:;
    if (offset >= 4096) goto L12; else goto L11;
  L14:;
    return secNum;
  L15:;
#line 579
    secNum = 0;
    goto L14;
  L16:;
#line 573
    secNum = ((int32_t)((unsigned char)(Assets_map160[offset])));
    goto L14;
  L17:;
#line 574
    secNum = ((int32_t)((unsigned char)(Assets_map168[offset])));
    goto L14;
  L18:;
#line 575
    secNum = ((int32_t)((unsigned char)(Assets_map176[offset])));
    goto L14;
  L19:;
#line 576
    secNum = ((int32_t)((unsigned char)(Assets_map184[offset])));
    goto L14;
  L20:;
#line 577
    secNum = ((int32_t)((unsigned char)(Assets_map192[offset])));
    goto L14;
}

static void Assets_SetMapSector(int32_t x, int32_t y, int32_t val) {
    (void)x;
    (void)y;
    (void)val;
    int32_t imx;
    int32_t imy;
    int32_t secx;
    int32_t secy;
    int32_t offset;
#line 587
    imx = m2_div(x, 16);
#line 588
    imy = m2_div(y, 32);
#line 589
    secx = (m2_div(imx, 16) - Assets_xReg);
    if (secx < 0) goto L2; else goto L3;
  L1:;
#line 595
    secy = (m2_div(imy, 8) - Assets_yReg);
    if (secy < 0) goto L8; else goto L7;
  L2:;
    if (((uint32_t)(((uint32_t)(secx))) & (uint32_t)(32)) != 0) goto L5; else goto L6;
  L3:;
    if (secx >= 64) goto L2; else goto L1;
  L4:;
    goto L1;
  L5:;
#line 591
    secx = 0;
    goto L4;
  L6:;
#line 592
    secx = 63;
    goto L4;
  L7:;
    if (secy >= 32) goto L10; else goto L9;
  L8:;
#line 596
    secy = 0;
    goto L7;
  L9:;
#line 598
    offset = (((secy * 128) + secx) + Assets_xReg);
    if (offset < 0) goto L12; else goto L13;
  L10:;
#line 597
    secy = 31;
    goto L9;
  L11:;
    if ((Assets_activeMap == 0)) goto L16;
    if ((Assets_activeMap == 1)) goto L17;
    if ((Assets_activeMap == 2)) goto L18;
    if ((Assets_activeMap == 3)) goto L19;
    if ((Assets_activeMap == 4)) goto L20;
    goto L15;
  L12:;
    return;
  L13:;
    if (offset >= 4096) goto L12; else goto L11;
  L14:;
    return;
  L15:;
    goto L14;
  L16:;
#line 601
    Assets_map160[offset] = ((char)(val));
    goto L14;
  L17:;
#line 602
    Assets_map168[offset] = ((char)(val));
    goto L14;
  L18:;
#line 603
    Assets_map176[offset] = ((char)(val));
    goto L14;
  L19:;
#line 604
    Assets_map184[offset] = ((char)(val));
    goto L14;
  L20:;
#line 605
    Assets_map192[offset] = ((char)(val));
    goto L14;
}

static int32_t Assets_GetTerrainAt(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    int32_t secByte;
    int32_t cm;
    int32_t tilesMask;
    int32_t subBit;
#line 613
    secByte = Assets_GetSectorByte(x, y);
#line 614
    cm = (secByte * 4);
    if ((cm + 2) < 0) goto L2; else goto L3;
  L1:;
#line 619
    subBit = 128;
    if (m2_mod(x, 16) >= 8) goto L5; else goto L4;
  L2:;
    return 0;
  L3:;
    if ((cm + 2) >= 1024) goto L2; else goto L1;
  L4:;
    if (m2_mod(y, 32) >= 16) goto L7; else goto L6;
  L5:;
#line 621
    subBit = m2_div(subBit, 16);
    goto L4;
  L6:;
    if (m2_mod(m2_mod(y, 32), 16) >= 8) goto L9; else goto L8;
  L7:;
#line 624
    subBit = m2_div(subBit, 4);
    goto L6;
  L8:;
#line 632
    tilesMask = ((int32_t)((unsigned char)(Assets_terraMem[(cm + 2)])));
    if (m2_mod(m2_div(tilesMask, subBit), 2) == 0) goto L11; else goto L10;
  L9:;
#line 627
    subBit = m2_div(subBit, 2);
    goto L8;
  L10:;
    return m2_mod(m2_div(((int32_t)((unsigned char)(Assets_terraMem[(cm + 1)]))), 16), 16);
  L11:;
    return 0;
}

static int32_t Assets_GetTilesBits(int32_t secByte) {
    (void)secByte;
    int32_t cm;
#line 644
    cm = (secByte * 4);
    if ((cm + 2) < 0) goto L2; else goto L3;
  L1:;
    return ((int32_t)((unsigned char)(Assets_terraMem[(cm + 2)])));
  L2:;
    return 0;
  L3:;
    if ((cm + 2) >= 1024) goto L2; else goto L1;
}

static int32_t Assets_GetMapTag(int32_t secByte) {
    (void)secByte;
    int32_t cm;
#line 652
    cm = (secByte * 4);
    if (cm < 0) goto L2; else goto L3;
  L1:;
    return ((int32_t)((unsigned char)(Assets_terraMem[cm])));
  L2:;
    return 0;
  L3:;
    if (cm >= 1024) goto L2; else goto L1;
}

static int32_t Assets_GetMaskType(int32_t secByte) {
    (void)secByte;
    int32_t cm;
#line 660
    cm = (secByte * 4);
    if ((cm + 1) < 0) goto L2; else goto L3;
  L1:;
    return m2_mod(((int32_t)((unsigned char)(Assets_terraMem[(cm + 1)]))), 16);
  L2:;
    return 0;
  L3:;
    if ((cm + 1) >= 1024) goto L2; else goto L1;
}

static int Assets_IsBlocked(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    int32_t t;
#line 669
    t = Assets_GetTerrainAt(x, y);
    if (t == 15) goto L2; else goto L1;
  L1:;
    return (t == 1 || t >= 10);
  L2:;
    return 0;
}

static int32_t Assets_TerrainSpeedAt(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    int32_t t;
#line 680
    t = Assets_GetTerrainAt(x, y);
    if ((t == 0)) goto L3;
    if ((t == 1)) goto L4;
    if ((t == 2)) goto L5;
    if ((t == 3)) goto L6;
    if ((t == 4)) goto L7;
    if ((t == 5)) goto L8;
    if ((t == 10)) goto L9;
    if ((t == 15)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 2;
  L3:;
    return 2;
  L4:;
    return 0;
  L5:;
    return 4;
  L6:;
    return 2;
  L7:;
    return 2;
  L8:;
    return 1;
  L9:;
    return 1;
  L10:;
    return 1;
}

static int32_t Assets_DetectRegion(int32_t mapX, int32_t mapY) {
    (void)mapX;
    (void)mapY;
    int32_t xs;
    int32_t ys;
    int32_t xr;
    int32_t yr;
#line 702
    xs = m2_div((mapX + 151), 256);
#line 703
    ys = m2_div((mapY + 64), 256);
#line 704
    xr = m2_mod(m2_div(xs, 64), 2);
#line 705
    yr = m2_mod(m2_div(ys, 32), 8);
    return (xr + (yr * 2));
}

static void Assets_CheckRegionSwitch(int32_t px, int32_t py) {
    (void)px;
    (void)py;
    int32_t newReg;
    if (Assets_currentRegion >= 8) goto L2; else goto L1;
  L1:;
#line 713
    newReg = Assets_DetectRegion(px, py);
    if (newReg != Assets_currentRegion) goto L6; else goto L3;
  L2:;
    return;
  L3:;
    return;
  L4:;
#line 715
    Assets_SwitchRegion(newReg);
    goto L3;
  L5:;
    if (newReg < 8) goto L4; else goto L3;
  L6:;
    if (newReg >= 0) goto L5; else goto L3;
}

/* Imported Module BinaryIO */

static const int32_t BinaryIO_MaxFiles = 32;
static const int32_t BinaryIO_SeekSet = 0;
static const int32_t BinaryIO_SeekEnd = 2;
static void BinaryIO_EnsureInit(void);
static int32_t BinaryIO_AllocSlot(void);
static void BinaryIO_OpenRead(char *name, uint32_t name_high, uint32_t *fh);
static void BinaryIO_OpenWrite(char *name, uint32_t name_high, uint32_t *fh);
static void BinaryIO_Close(uint32_t fh);
static void BinaryIO_ReadByte(uint32_t fh, uint32_t *b);
static void BinaryIO_WriteByte(uint32_t fh, uint32_t b);
static void BinaryIO_ReadBytes(uint32_t fh, char *buf, uint32_t buf_high, uint32_t n, uint32_t *actual);
static void BinaryIO_WriteBytes(uint32_t fh, char *buf, uint32_t buf_high, uint32_t n);
static void BinaryIO_FileSize(uint32_t fh, uint32_t *size);
static void BinaryIO_Seek(uint32_t fh, uint32_t pos);
static void BinaryIO_Tell(uint32_t fh, uint32_t *pos);
static int BinaryIO_IsEOF(uint32_t fh);

int BinaryIO_Done;
int BinaryIO_Done;
void * BinaryIO_files[31 + 1];
int BinaryIO_initialized;
static void BinaryIO_EnsureInit(void) {
    uint32_t i;
    if (BinaryIO_initialized) goto L1; else goto L2;
  L1:;
    return;
  L2:;
    i = 0;
    goto L3;
  L3:;
    if (i <= (32 - 1)) goto L4; else goto L6;
  L4:;
#line 20 "/Users/aoesterer/.mx/lib/m2stdlib/src/BinaryIO.mod"
    BinaryIO_files[i] = NULL;
    goto L5;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 22
    BinaryIO_initialized = 1;
    goto L1;
}

static int32_t BinaryIO_AllocSlot(void) {
    uint32_t i;
#line 29
    BinaryIO_EnsureInit();
    i = 0;
    goto L1;
  L1:;
    if (i <= (32 - 1)) goto L2; else goto L4;
  L2:;
    if (BinaryIO_files[i] == NULL) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return (-1);
  L5:;
    goto L3;
  L6:;
    return ((int32_t)(i));
}

static void BinaryIO_OpenRead(char *name, uint32_t name_high, uint32_t *fh) {
    (void)name;
    (void)name_high;
    (void)fh;
    int32_t slot;
    char rb[2 + 1];
#line 39
    slot = BinaryIO_AllocSlot();
    if (slot < 0) goto L2; else goto L1;
  L1:;
#line 43
    rb[0] = 'r';
    rb[1] = 'b';
    rb[2] = ((char)(0));
#line 44
    BinaryIO_files[slot] = fopen(((void *)(name)), ((void *)&(rb)));
    if (BinaryIO_files[slot] != NULL) goto L4; else goto L5;
  L2:;
#line 41
    BinaryIO_Done = 0;
    (*fh) = 0;
    return;
  L3:;
    return;
  L4:;
#line 46
    (*fh) = ((uint32_t)((slot + 1)));
#line 47
    BinaryIO_Done = 1;
    goto L3;
  L5:;
#line 49
    (*fh) = 0;
#line 50
    BinaryIO_Done = 0;
    goto L3;
}

static void BinaryIO_OpenWrite(char *name, uint32_t name_high, uint32_t *fh) {
    (void)name;
    (void)name_high;
    (void)fh;
    int32_t slot;
    char wb[2 + 1];
#line 57
    slot = BinaryIO_AllocSlot();
    if (slot < 0) goto L2; else goto L1;
  L1:;
#line 61
    wb[0] = 'w';
    wb[1] = 'b';
    wb[2] = ((char)(0));
#line 62
    BinaryIO_files[slot] = fopen(((void *)(name)), ((void *)&(wb)));
    if (BinaryIO_files[slot] != NULL) goto L4; else goto L5;
  L2:;
#line 59
    BinaryIO_Done = 0;
    (*fh) = 0;
    return;
  L3:;
    return;
  L4:;
#line 64
    (*fh) = ((uint32_t)((slot + 1)));
#line 65
    BinaryIO_Done = 1;
    goto L3;
  L5:;
#line 67
    (*fh) = 0;
#line 68
    BinaryIO_Done = 0;
    goto L3;
}

static void BinaryIO_Close(uint32_t fh) {
    (void)fh;
#line 74
    BinaryIO_EnsureInit();
    if (fh >= 1) goto L4; else goto L1;
  L1:;
    return;
  L2:;
#line 76
    fclose(BinaryIO_files[(fh - 1)]);
#line 77
    BinaryIO_files[(fh - 1)] = NULL;
    goto L1;
  L3:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L1;
  L4:;
    if (fh <= 32) goto L3; else goto L1;
}

static void BinaryIO_ReadByte(uint32_t fh, uint32_t *b) {
    (void)fh;
    (void)b;
    int32_t c;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 85
    c = fgetc(BinaryIO_files[(fh - 1)]);
    if (c == (-1)) goto L7; else goto L8;
  L3:;
#line 92
    (*b) = 0;
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
  L6:;
    goto L1;
  L7:;
#line 87
    (*b) = 0;
    BinaryIO_Done = 0;
    goto L6;
  L8:;
#line 89
    (*b) = ((uint32_t)(c));
    BinaryIO_Done = 1;
    goto L6;
}

static void BinaryIO_WriteByte(uint32_t fh, uint32_t b) {
    (void)fh;
    (void)b;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 99
    fputc(((int32_t)(b)), BinaryIO_files[(fh - 1)]);
#line 100
    BinaryIO_Done = 1;
    goto L1;
  L3:;
#line 102
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
}

static void BinaryIO_ReadBytes(uint32_t fh, char *buf, uint32_t buf_high, uint32_t n, uint32_t *actual) {
    (void)fh;
    (void)buf;
    (void)buf_high;
    (void)n;
    (void)actual;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 109
    (*actual) = fread(((void *)(buf)), 1, n, BinaryIO_files[(fh - 1)]);
#line 110
    BinaryIO_Done = (*actual) > 0;
    goto L1;
  L3:;
#line 112
    (*actual) = 0;
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
}

static void BinaryIO_WriteBytes(uint32_t fh, char *buf, uint32_t buf_high, uint32_t n) {
    (void)fh;
    (void)buf;
    (void)buf_high;
    (void)n;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 119
    fwrite(((void *)(buf)), 1, n, BinaryIO_files[(fh - 1)]);
#line 120
    BinaryIO_Done = 1;
    goto L1;
  L3:;
#line 122
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
}

static void BinaryIO_FileSize(uint32_t fh, uint32_t *size) {
    (void)fh;
    (void)size;
    int32_t cur;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 130
    cur = ftell(BinaryIO_files[(fh - 1)]);
#line 131
    fseek(BinaryIO_files[(fh - 1)], 0, 2);
#line 132
    (*size) = ((uint32_t)(ftell(BinaryIO_files[(fh - 1)])));
#line 133
    fseek(BinaryIO_files[(fh - 1)], cur, 0);
#line 134
    BinaryIO_Done = 1;
    goto L1;
  L3:;
#line 136
    (*size) = 0;
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
}

static void BinaryIO_Seek(uint32_t fh, uint32_t pos) {
    (void)fh;
    (void)pos;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 143
    fseek(BinaryIO_files[(fh - 1)], ((int32_t)(pos)), 0);
#line 144
    BinaryIO_Done = 1;
    goto L1;
  L3:;
#line 146
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
}

static void BinaryIO_Tell(uint32_t fh, uint32_t *pos) {
    (void)fh;
    (void)pos;
    if (fh >= 1) goto L5; else goto L3;
  L1:;
    return;
  L2:;
#line 153
    (*pos) = ((uint32_t)(ftell(BinaryIO_files[(fh - 1)])));
#line 154
    BinaryIO_Done = 1;
    goto L1;
  L3:;
#line 156
    (*pos) = 0;
    BinaryIO_Done = 0;
    goto L1;
  L4:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L3;
  L5:;
    if (fh <= 32) goto L4; else goto L3;
}

static int BinaryIO_IsEOF(uint32_t fh) {
    (void)fh;
    if (fh >= 1) goto L4; else goto L1;
  L1:;
    return 1;
  L2:;
    return feof(BinaryIO_files[(fh - 1)]) != 0;
  L3:;
    if (BinaryIO_files[(fh - 1)] != NULL) goto L2; else goto L1;
  L4:;
    if (fh <= 32) goto L3; else goto L1;
}

static void BinaryIO_init(void) {
#line 169
    BinaryIO_Done = 1;
#line 170
    BinaryIO_initialized = 0;
    return;
}

/* Imported Module Storage */

static void Storage_ALLOCATE(void * *p, uint32_t size);
static void Storage_DEALLOCATE(void * *p, uint32_t size);

static void Storage_ALLOCATE(void * *p, uint32_t size) {
    (void)p;
    (void)size;
#line 7 "/Users/aoesterer/.mx/lib/m2stdlib/src/Storage.mod"
    (*p) = malloc(size);
    return;
}

static void Storage_DEALLOCATE(void * *p, uint32_t size) {
    (void)p;
    (void)size;
#line 12
    free((*p));
#line 13
    (*p) = NULL;
    return;
}

/* Imported Module SFX */

static const int32_t SFX_SfxSwordClang = 1;
static const int32_t SFX_SfxPlayerHit = 0;
static const int32_t SFX_SfxEnemyHit = 3;
static const int32_t SFX_SfxArrowHit = 2;
static const int32_t SFX_SfxBowFire = 4;
static const int32_t SFX_SfxWandFire = 5;
static const int32_t SFX_NumSamples = 6;
static const int32_t SFX_SampleRate = 22050;
static const int32_t SFX_MaxSampleBytes = 4096;
static int SFX_LoadSample(int32_t idx, char *name, uint32_t name_high);
static int SFX_InitSFX(void);
static void SFX_PlayEffect(int32_t num);
static void SFX_ShutdownSFX(void);

int32_t SFX_sfxDev;
void * SFX_samples[5 + 1];
int32_t SFX_sampleLen[5 + 1];
int SFX_loaded;
static int SFX_LoadSample(int32_t idx, char *name, uint32_t name_high) {
    (void)idx;
    (void)name;
    (void)name_high;
    char p[127 + 1];
    uint32_t fd;
    void * buf;
    uint32_t n;
#line 32 "src/SFX.mod"
    Assets_AssetPath(name, name_high, p, 127);
#line 35
    m2_BinaryIO_OpenRead(p, &fd);
    if (BinaryIO_Done) goto L1; else goto L2;
  L1:;
#line 40
    m2_ALLOCATE((void **)&buf, 4096);
#line 41
    m2_BinaryIO_ReadBytes(fd, buf, 4096, &n);
#line 42
    m2_BinaryIO_Close(fd);
    if (n <= 44) goto L4; else goto L3;
  L2:;
#line 37
    m2_WriteString("SFX: cannot open ");
    m2_WriteString(name);
    m2_WriteLn();
    return 0;
  L3:;
#line 48
    SFX_samples[idx] = buf;
#line 49
    SFX_sampleLen[idx] = ((int32_t)(n));
    return 1;
  L4:;
#line 44
    m2_DEALLOCATE((void **)&buf, 4096);
    return 0;
}

static int SFX_InitSFX(void) {
    int32_t i;
#line 56
    SFX_loaded = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= (6 - 1)) goto L2; else goto L4;
  L2:;
#line 58
    SFX_samples[i] = NULL;
#line 59
    SFX_sampleLen[i] = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 62
    SFX_sfxDev = Playback_OpenDevice(22050, 1, Playback_FormatS16, 512);
    if (SFX_sfxDev == 0) goto L6; else goto L5;
  L5:;
#line 67
    Playback_ResumeDevice(SFX_sfxDev);
#line 69
    SFX_LoadSample(0, "fta_sample_0.wav", 16);
#line 70
    SFX_LoadSample(1, "fta_sample_1.wav", 16);
#line 71
    SFX_LoadSample(2, "fta_sample_2.wav", 16);
#line 72
    SFX_LoadSample(3, "fta_sample_3.wav", 16);
#line 73
    SFX_LoadSample(4, "fta_sample_4.wav", 16);
#line 74
    SFX_LoadSample(5, "fta_sample_5.wav", 16);
#line 76
    SFX_loaded = 1;
#line 77
    m2_WriteString("SFX: loaded ");
    m2_WriteInt(6, 1);
#line 78
    m2_WriteString(" samples");
    m2_WriteLn();
    return 1;
  L6:;
#line 64
    m2_WriteString("SFX: cannot open audio device");
    m2_WriteLn();
    return 0;
}

static void SFX_PlayEffect(int32_t num) {
    (void)num;
    int ok;
    if (SFX_loaded) goto L1; else goto L2;
  L1:;
    if (num < 0) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    if (SFX_samples[num] == NULL) goto L7; else goto L6;
  L4:;
    return;
  L5:;
    if (num >= 6) goto L4; else goto L3;
  L6:;
    if (SFX_sampleLen[num] <= 44) goto L9; else goto L8;
  L7:;
    return;
  L8:;
#line 91
    Playback_ClearQueued(SFX_sfxDev);
#line 92
    ok = Playback_QueueBytes(SFX_sfxDev, SFX_samples[num], ((uint32_t)(SFX_sampleLen[num])));
    return;
  L9:;
    return;
}

static void SFX_ShutdownSFX(void) {
    int32_t i;
    if (SFX_sfxDev != 0) goto L2; else goto L1;
  L1:;
    i = 0;
    goto L3;
  L2:;
#line 99
    Playback_CloseDevice(SFX_sfxDev);
#line 100
    SFX_sfxDev = 0;
    goto L1;
  L3:;
    if (i <= (6 - 1)) goto L4; else goto L6;
  L4:;
    if (SFX_samples[i] != NULL) goto L8; else goto L7;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 108
    SFX_loaded = 0;
    return;
  L7:;
    goto L5;
  L8:;
#line 104
    m2_DEALLOCATE((void **)&SFX_samples[i], 4096);
#line 105
    SFX_samples[i] = NULL;
    goto L7;
}

/* Imported Module Actor */

typedef struct Actor_Actor Actor_Actor;
static const int32_t Actor_StShoot1 = 24;
static const int32_t Actor_TacEvade = 6;
static const int32_t Actor_StFrozen = 20;
static const int32_t Actor_TypeCarrier = 5;
static const int32_t Actor_GoalAttack2 = 2;
static const int32_t Actor_TacShootFrust = 10;
static const int32_t Actor_TacPursue = 1;
static const int32_t Actor_GoalArcher2 = 4;
static const int32_t Actor_GoalConfused = 10;
static const int32_t Actor_TypeObject = 1;
static const int32_t Actor_TacFrust = 9;
static const int32_t Actor_TacFollow = 2;
static const int32_t Actor_GoalFlee = 5;
static const int32_t Actor_TypeSetfig = 4;
static const int32_t Actor_GoalArcher1 = 3;
static const int32_t Actor_StShoot3 = 25;
static const int32_t Actor_GoalAttack1 = 1;
static const int32_t Actor_TacShoot = 8;
static const int32_t Actor_MaxActors = 48;
static const int32_t Actor_TypeDragon = 6;
static const int32_t Actor_TypeEnemy = 2;
static const int32_t Actor_GoalDeath = 7;
static const int32_t Actor_StFlying = 21;
static const int32_t Actor_TacRandom = 4;
static const int32_t Actor_StSleep = 23;
static const int32_t Actor_TacBumble = 3;
static const int32_t Actor_StWalking = 12;
static const int32_t Actor_GoalStand = 6;
static const int32_t Actor_TypeRaft = 3;
static const int32_t Actor_TypePlayer = 0;
static const int32_t Actor_TacBackup = 5;
static const int32_t Actor_GoalUser = 0;
static const int32_t Actor_TacEggSeek = 11;
static const int32_t Actor_GoalFollow = 9;
static const int32_t Actor_GoalWait = 8;
static const int32_t Actor_StStill = 13;
static const int32_t Actor_StFighting = 0;
static const int32_t Actor_StSink = 16;
static const int32_t Actor_StFall = 22;
static const int32_t Actor_StDead = 15;
static const int32_t Actor_StDying = 14;
typedef struct Actor_Actor Actor_Actor;
struct Actor_Actor {
    int32_t absX;
    int32_t absY;
    int32_t relX;
    int32_t relY;
    int32_t actorType;
    int32_t race;
    int32_t index;
    int visible;
    int looted;
    int32_t weapon;
    int32_t environ;
    int32_t goal;
    int32_t tactic;
    int32_t state;
    int32_t facing;
    int32_t vitality;
    int32_t velX;
    int32_t velY;
};

static void Actor_InitActor(Actor_Actor *a);
static void Actor_InitAll(void);

int32_t Actor_actorCount;
Actor_Actor Actor_actors[47 + 1];
static void Actor_InitActor(Actor_Actor *a) {
    (void)a;
#line 5 "src/Actor.mod"
    (*a).absX = 0;
    (*a).absY = 0;
#line 6
    (*a).relX = 0;
    (*a).relY = 0;
#line 7
    (*a).actorType = 0;
#line 8
    (*a).race = 0;
#line 9
    (*a).index = 0;
#line 10
    (*a).visible = 0;
#line 11
    (*a).looted = 0;
#line 12
    (*a).weapon = 0;
#line 13
    (*a).environ = 0;
#line 14
    (*a).goal = 0;
#line 15
    (*a).tactic = 0;
#line 16
    (*a).state = 13;
#line 17
    (*a).facing = 4;
#line 18
    (*a).vitality = 100;
#line 19
    (*a).velX = 0;
#line 20
    (*a).velY = 0;
    return;
}

static void Actor_InitAll(void) {
    int32_t i;
#line 26
    Actor_actorCount = 1;
    i = 0;
    goto L1;
  L1:;
    if (i <= (48 - 1)) goto L2; else goto L4;
  L2:;
#line 28
    Actor_InitActor(&Actor_actors[i]);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

/* Imported Module World */

typedef struct World_Tile World_Tile;
static const int32_t World_MaxCoord = 65536;
static const int32_t World_TerrDoor = 6;
static const int32_t World_TerrWater = 1;
static const int32_t World_CoordMask = 65535;
static const int32_t World_TerrPath = 4;
static const int32_t World_TileSize = 16;
static const int32_t World_TerrFloor = 10;
static const int32_t World_WorldW = 64;
static const int32_t World_TerrGrass = 0;
static const int32_t World_TerrWall = 5;
static const int32_t World_WorldH = 64;
static const int32_t World_TerrSand = 7;
static const int32_t World_TerrMountain = 3;
static const int32_t World_TerrBridge = 9;
static const int32_t World_TerrForest = 2;
static const int32_t World_TerrSwamp = 8;
typedef struct World_Tile World_Tile;
struct World_Tile {
    int32_t terrain;
};

static void World_InitWorld(void);
static void World_BuildRoom(int32_t rx, int32_t ry, int32_t rw, int32_t rh);
static int World_IsPassable(int32_t terrain);
static int32_t World_TerrainSpeed(int32_t terrain);
static int32_t World_GetTerrain(int32_t x, int32_t y);
static int32_t World_WorldToTile(int32_t wx);
static void World_UpdateCamera(int32_t playerX, int32_t playerY);

int32_t World_camY;
int32_t World_camX;
World_Tile World_tiles[63 + 1][63 + 1];
static void World_InitWorld(void) {
    int32_t x;
    int32_t y;
#line 8 "src/World.mod"
    World_camX = 0;
#line 9
    World_camY = 0;
    x = 0;
    goto L1;
  L1:;
    if (x <= (64 - 1)) goto L2; else goto L4;
  L2:;
    y = 0;
    goto L5;
  L3:;
    x = (x + 1);
    goto L1;
  L4:;
    x = 0;
    goto L9;
  L5:;
    if (y <= (64 - 1)) goto L6; else goto L8;
  L6:;
#line 14
    World_tiles[x][y].terrain = 0;
    goto L7;
  L7:;
    y = (y + 1);
    goto L5;
  L8:;
    goto L3;
  L9:;
    if (x <= (64 - 1)) goto L10; else goto L12;
  L10:;
#line 20
    World_tiles[x][0].terrain = 3;
#line 21
    World_tiles[x][(64 - 1)].terrain = 3;
    goto L11;
  L11:;
    x = (x + 1);
    goto L9;
  L12:;
    y = 0;
    goto L13;
  L13:;
    if (y <= (64 - 1)) goto L14; else goto L16;
  L14:;
#line 24
    World_tiles[0][y].terrain = 3;
#line 25
    World_tiles[(64 - 1)][y].terrain = 3;
    goto L15;
  L15:;
    y = (y + 1);
    goto L13;
  L16:;
    x = 5;
    goto L17;
  L17:;
    if (x <= 15) goto L18; else goto L20;
  L18:;
    y = 5;
    goto L21;
  L19:;
    x = (x + 1);
    goto L17;
  L20:;
    x = 4;
    goto L25;
  L21:;
    if (y <= 12) goto L22; else goto L24;
  L22:;
#line 31
    World_tiles[x][y].terrain = 1;
    goto L23;
  L23:;
    y = (y + 1);
    goto L21;
  L24:;
    goto L19;
  L25:;
    if (x <= 16) goto L26; else goto L28;
  L26:;
#line 36
    World_tiles[x][4].terrain = 7;
#line 37
    World_tiles[x][13].terrain = 7;
    goto L27;
  L27:;
    x = (x + 1);
    goto L25;
  L28:;
    y = 4;
    goto L29;
  L29:;
    if (y <= 13) goto L30; else goto L32;
  L30:;
#line 40
    World_tiles[4][y].terrain = 7;
#line 41
    World_tiles[16][y].terrain = 7;
    goto L31;
  L31:;
    y = (y + 1);
    goto L29;
  L32:;
    x = 9;
    goto L33;
  L33:;
    if (x <= 11) goto L34; else goto L36;
  L34:;
#line 45
    World_tiles[x][8].terrain = 9;
#line 46
    World_tiles[x][9].terrain = 9;
    goto L35;
  L35:;
    x = (x + 1);
    goto L33;
  L36:;
    x = 35;
    goto L37;
  L37:;
    if (x <= 50) goto L38; else goto L40;
  L38:;
    y = 4;
    goto L41;
  L39:;
    x = (x + 1);
    goto L37;
  L40:;
    x = 40;
    goto L45;
  L41:;
    if (y <= 15) goto L42; else goto L44;
  L42:;
#line 52
    World_tiles[x][y].terrain = 2;
    goto L43;
  L43:;
    y = (y + 1);
    goto L41;
  L44:;
    goto L39;
  L45:;
    if (x <= 44) goto L46; else goto L48;
  L46:;
    y = 8;
    goto L49;
  L47:;
    x = (x + 1);
    goto L45;
  L48:;
    x = 35;
    goto L53;
  L49:;
    if (y <= 11) goto L50; else goto L52;
  L50:;
#line 58
    World_tiles[x][y].terrain = 0;
    goto L51;
  L51:;
    y = (y + 1);
    goto L49;
  L52:;
    goto L47;
  L53:;
    if (x <= 50) goto L54; else goto L56;
  L54:;
#line 63
    World_tiles[x][10].terrain = 4;
    goto L55;
  L55:;
    x = (x + 1);
    goto L53;
  L56:;
    x = 2;
    goto L57;
  L57:;
    if (x <= (64 - 3)) goto L58; else goto L60;
  L58:;
#line 68
    World_tiles[x][30].terrain = 4;
#line 69
    World_tiles[x][31].terrain = 4;
    goto L59;
  L59:;
    x = (x + 1);
    goto L57;
  L60:;
    y = 2;
    goto L61;
  L61:;
    if (y <= (64 - 3)) goto L62; else goto L64;
  L62:;
#line 74
    World_tiles[30][y].terrain = 4;
#line 75
    World_tiles[31][y].terrain = 4;
    goto L63;
  L63:;
    y = (y + 1);
    goto L61;
  L64:;
    x = 26;
    goto L65;
  L65:;
    if (x <= 35) goto L66; else goto L68;
  L66:;
    y = 26;
    goto L69;
  L67:;
    x = (x + 1);
    goto L65;
  L68:;
#line 87
    World_BuildRoom(22, 22, 6, 5);
#line 88
    World_tiles[24][26].terrain = 6;
#line 91
    World_BuildRoom(36, 22, 6, 5);
#line 92
    World_tiles[38][26].terrain = 6;
#line 95
    World_BuildRoom(22, 36, 5, 5);
#line 96
    World_tiles[24][36].terrain = 6;
#line 99
    World_BuildRoom(37, 36, 8, 7);
#line 100
    World_tiles[40][36].terrain = 6;
    x = 38;
    goto L73;
  L69:;
    if (y <= 35) goto L70; else goto L72;
  L70:;
#line 82
    World_tiles[x][y].terrain = 4;
    goto L71;
  L71:;
    y = (y + 1);
    goto L69;
  L72:;
    goto L67;
  L73:;
    if (x <= 44) goto L74; else goto L76;
  L74:;
    y = 37;
    goto L77;
  L75:;
    x = (x + 1);
    goto L73;
  L76:;
    x = 5;
    goto L81;
  L77:;
    if (y <= 42) goto L78; else goto L80;
  L78:;
#line 104
    World_tiles[x][y].terrain = 10;
    goto L79;
  L79:;
    y = (y + 1);
    goto L77;
  L80:;
    goto L75;
  L81:;
    if (x <= 18) goto L82; else goto L84;
  L82:;
    y = 42;
    goto L85;
  L83:;
    x = (x + 1);
    goto L81;
  L84:;
    x = 8;
    goto L89;
  L85:;
    if (y <= 55) goto L86; else goto L88;
  L86:;
#line 111
    World_tiles[x][y].terrain = 8;
    goto L87;
  L87:;
    y = (y + 1);
    goto L85;
  L88:;
    goto L83;
  L89:;
    if (x <= 10) goto L90; else goto L92;
  L90:;
    y = 46;
    goto L93;
  L91:;
    x = (x + 1);
    goto L89;
  L92:;
    x = 14;
    goto L97;
  L93:;
    if (y <= 48) goto L94; else goto L96;
  L94:;
#line 117
    World_tiles[x][y].terrain = 1;
    goto L95;
  L95:;
    y = (y + 1);
    goto L93;
  L96:;
    goto L91;
  L97:;
    if (x <= 16) goto L98; else goto L100;
  L98:;
    y = 50;
    goto L101;
  L99:;
    x = (x + 1);
    goto L97;
  L100:;
    x = 45;
    goto L105;
  L101:;
    if (y <= 52) goto L102; else goto L104;
  L102:;
#line 122
    World_tiles[x][y].terrain = 1;
    goto L103;
  L103:;
    y = (y + 1);
    goto L101;
  L104:;
    goto L99;
  L105:;
    if (x <= 58) goto L106; else goto L108;
  L106:;
    y = 45;
    goto L109;
  L107:;
    x = (x + 1);
    goto L105;
  L108:;
    y = 48;
    goto L113;
  L109:;
    if (y <= 55) goto L110; else goto L112;
  L110:;
#line 129
    World_tiles[x][y].terrain = 3;
    goto L111;
  L111:;
    y = (y + 1);
    goto L109;
  L112:;
    goto L107;
  L113:;
    if (y <= 52) goto L114; else goto L116;
  L114:;
#line 134
    World_tiles[50][y].terrain = 4;
#line 135
    World_tiles[51][y].terrain = 4;
    goto L115;
  L115:;
    y = (y + 1);
    goto L113;
  L116:;
#line 139
    World_tiles[20][15].terrain = 2;
#line 140
    World_tiles[21][16].terrain = 2;
#line 141
    World_tiles[19][17].terrain = 2;
#line 142
    World_tiles[25][12].terrain = 2;
#line 143
    World_tiles[26][13].terrain = 2;
#line 144
    World_tiles[50][30].terrain = 2;
#line 145
    World_tiles[51][29].terrain = 2;
#line 146
    World_tiles[52][30].terrain = 2;
#line 147
    World_tiles[15][35].terrain = 2;
#line 148
    World_tiles[16][36].terrain = 2;
    return;
}

static void World_BuildRoom(int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    (void)rx;
    (void)ry;
    (void)rw;
    (void)rh;
    int32_t x;
    int32_t y;
    x = rx;
    goto L1;
  L1:;
    if (x <= ((rx + rw) - 1)) goto L2; else goto L4;
  L2:;
#line 156
    World_tiles[x][ry].terrain = 5;
#line 157
    World_tiles[x][((ry + rh) - 1)].terrain = 5;
    goto L3;
  L3:;
    x = (x + 1);
    goto L1;
  L4:;
    y = ry;
    goto L5;
  L5:;
    if (y <= ((ry + rh) - 1)) goto L6; else goto L8;
  L6:;
#line 160
    World_tiles[rx][y].terrain = 5;
#line 161
    World_tiles[((rx + rw) - 1)][y].terrain = 5;
    goto L7;
  L7:;
    y = (y + 1);
    goto L5;
  L8:;
    x = (rx + 1);
    goto L9;
  L9:;
    if (x <= ((rx + rw) - 2)) goto L10; else goto L12;
  L10:;
    y = (ry + 1);
    goto L13;
  L11:;
    x = (x + 1);
    goto L9;
  L12:;
    return;
  L13:;
    if (y <= ((ry + rh) - 2)) goto L14; else goto L16;
  L14:;
#line 166
    World_tiles[x][y].terrain = 10;
    goto L15;
  L15:;
    y = (y + 1);
    goto L13;
  L16:;
    goto L11;
}

static int World_IsPassable(int32_t terrain) {
    (void)terrain;
    return (((((((terrain == 0 || terrain == 4) || terrain == 6) || terrain == 7) || terrain == 9) || terrain == 10) || terrain == 8) || terrain == 2);
}

static int32_t World_TerrainSpeed(int32_t terrain) {
    (void)terrain;
    if ((terrain == 8)) goto L3;
    if ((terrain == 2)) goto L4;
    if ((terrain == 7)) goto L5;
    if ((terrain == 4)) goto L6;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 2;
  L3:;
    return 1;
  L4:;
    return 1;
  L5:;
    return 1;
  L6:;
    return 3;
}

static int32_t World_GetTerrain(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    int32_t tx;
    int32_t ty;
#line 194
    tx = m2_div(x, 16);
#line 195
    ty = m2_div(y, 16);
    if (tx < 0) goto L2; else goto L5;
  L1:;
    return World_tiles[tx][ty].terrain;
  L2:;
    return 3;
  L3:;
    if (ty >= 64) goto L2; else goto L1;
  L4:;
    if (ty < 0) goto L2; else goto L3;
  L5:;
    if (tx >= 64) goto L2; else goto L4;
}

static int32_t World_WorldToTile(int32_t wx) {
    (void)wx;
    return m2_div(wx, 16);
}

static void World_UpdateCamera(int32_t playerX, int32_t playerY) {
    (void)playerX;
    (void)playerY;
    int32_t halfW;
    int32_t halfH;
#line 210
    halfW = m2_div(320, 2);
#line 211
    halfH = m2_div(143, 2);
#line 212
    World_camX = (playerX - halfW);
#line 213
    World_camY = (playerY - halfH);
    if (World_camX < 0) goto L2; else goto L1;
  L1:;
    if (World_camY < 0) goto L4; else goto L3;
  L2:;
#line 214
    World_camX = 0;
    goto L1;
  L3:;
    if (World_camX > (65536 - 320)) goto L6; else goto L5;
  L4:;
#line 215
    World_camY = 0;
    goto L3;
  L5:;
    if (World_camY > (65536 - 143)) goto L8; else goto L7;
  L6:;
#line 218
    World_camX = (65536 - 320);
    goto L5;
  L7:;
    return;
  L8:;
#line 221
    World_camY = (65536 - 143);
    goto L7;
}

/* Imported Module Brothers */

typedef struct Brothers_BrotherData Brothers_BrotherData;
static const int32_t Brothers_StSunStone = 7;
static const int32_t Brothers_StLasso = 5;
static const int32_t Brothers_StHarvestScroll = 43;
static const int32_t Brothers_Julian = 0;
static const int32_t Brothers_StNightshade = 35;
static const int32_t Brothers_NumBrothers = 3;
static const int32_t Brothers_StWrit = 28;
static const int32_t Brothers_Philip = 1;
static const int32_t Brothers_StMandrake = 31;
static const int32_t Brothers_StWolfsbane = 32;
static const int32_t Brothers_StYarrow = 34;
static const int32_t Brothers_StFearScroll = 40;
static const int32_t Brothers_StSanctuaryScroll = 42;
static const int32_t Brothers_StShell = 6;
static const int32_t Brothers_StStatue = 25;
static const int32_t Brothers_StWardScroll = 37;
static const int32_t Brothers_StFireScroll = 39;
static const int32_t Brothers_StShard = 30;
static const int32_t Brothers_StBone = 29;
static const int32_t Brothers_StTalisman = 22;
static const int32_t Brothers_Kevin = 2;
static const int32_t Brothers_LastStuff = 44;
static const int32_t Brothers_StLightScroll = 41;
static const int32_t Brothers_StMugwort = 33;
static const int32_t Brothers_StBloodroot = 36;
static const int32_t Brothers_StFreezeScroll = 38;
static const int32_t Brothers_StHealScroll = 44;
typedef struct Brothers_BrotherData Brothers_BrotherData;
struct Brothers_BrotherData {
    char name[15 + 1];
    int32_t vitality;
    int32_t weapon;
    int32_t brave;
    int32_t luck;
    int32_t kind;
    int32_t wealth;
    int32_t weaponInv[5 + 1];
    int32_t stuff[44 + 1];
    int32_t startX;
    int32_t startY;
    int alive;
};

static void Brothers_ClearInventory(Brothers_BrotherData *b);
static int Brothers_HasStuff(int32_t idx);
static int Brothers_HasWeapon(int32_t idx);
static void Brothers_GiveStuff(int32_t idx);
static void Brothers_AddStuffN(int32_t idx, int32_t n);
static void Brothers_SetStuff(int32_t idx, int32_t val);
static void Brothers_AddWealth(int32_t amount);
static void Brothers_IncBrave(void);
static void Brothers_DecLuck(int32_t amount);
static void Brothers_DecKind(int32_t amount);
static void Brothers_IncKind(void);
static void Brothers_InitBrothers(void);
static void Brothers_SaveBrotherState(void);
static void Brothers_RestoreBrotherState(void);
static int Brothers_SwitchToNext(void);
static void Brothers_ActiveName(char *name, uint32_t name_high);

int32_t Brothers_activeBrother;
Brothers_BrotherData Brothers_brothers[2 + 1];
static void Brothers_ClearInventory(Brothers_BrotherData *b) {
    (void)b;
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= 5) goto L2; else goto L4;
  L2:;
#line 10 "src/Brothers.mod"
    (*b).weaponInv[i] = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    i = 0;
    goto L5;
  L5:;
    if (i <= 44) goto L6; else goto L8;
  L6:;
#line 11
    (*b).stuff[i] = 0;
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
    return;
}

static int Brothers_HasStuff(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return 0;
  L2:;
    return Brothers_brothers[Brothers_activeBrother].stuff[idx] > 0;
  L3:;
    if (idx <= 44) goto L2; else goto L1;
}

static int Brothers_HasWeapon(int32_t idx) {
    (void)idx;
    if (idx >= 1) goto L3; else goto L1;
  L1:;
    return 0;
  L2:;
    return Brothers_brothers[Brothers_activeBrother].stuff[(idx - 1)] > 0;
  L3:;
    if (idx <= 5) goto L2; else goto L1;
}

static void Brothers_GiveStuff(int32_t idx) {
    (void)idx;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 34
    (Brothers_brothers[Brothers_activeBrother].stuff[idx]++);
    goto L1;
  L3:;
    if (idx <= 44) goto L2; else goto L1;
}

static void Brothers_AddStuffN(int32_t idx, int32_t n) {
    (void)idx;
    (void)n;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 41
    (Brothers_brothers[Brothers_activeBrother].stuff[idx] += n);
    goto L1;
  L3:;
    if (idx <= 44) goto L2; else goto L1;
}

static void Brothers_SetStuff(int32_t idx, int32_t val) {
    (void)idx;
    (void)val;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 48
    Brothers_brothers[Brothers_activeBrother].stuff[idx] = val;
    goto L1;
  L3:;
    if (idx <= 44) goto L2; else goto L1;
}

static void Brothers_AddWealth(int32_t amount) {
    (void)amount;
#line 54
    (Brothers_brothers[Brothers_activeBrother].wealth += amount);
    return;
}

static void Brothers_IncBrave(void) {
#line 59
    (Brothers_brothers[Brothers_activeBrother].brave++);
    return;
}

static void Brothers_DecLuck(int32_t amount) {
    (void)amount;
#line 64
    (Brothers_brothers[Brothers_activeBrother].luck -= amount);
    if (Brothers_brothers[Brothers_activeBrother].luck < 0) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 66
    Brothers_brothers[Brothers_activeBrother].luck = 0;
    goto L1;
}

static void Brothers_DecKind(int32_t amount) {
    (void)amount;
#line 72
    (Brothers_brothers[Brothers_activeBrother].kind -= amount);
    if (Brothers_brothers[Brothers_activeBrother].kind < 0) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 74
    Brothers_brothers[Brothers_activeBrother].kind = 0;
    goto L1;
}

static void Brothers_IncKind(void) {
#line 80
    (Brothers_brothers[Brothers_activeBrother].kind++);
    return;
}

static void Brothers_InitBrothers(void) {
#line 85
    Brothers_activeBrother = 0;
#line 87
    m2_Strings_Assign("Julian", Brothers_brothers[0].name, 15);
#line 88
    Brothers_brothers[0].vitality = 200;
#line 89
    Brothers_brothers[0].weapon = 3;
#line 90
    Brothers_brothers[0].brave = 150;
#line 91
    Brothers_brothers[0].luck = 20;
#line 92
    Brothers_brothers[0].kind = 15;
#line 93
    Brothers_brothers[0].wealth = 200;
#line 94
    Brothers_ClearInventory(&Brothers_brothers[0]);
#line 95
    Brothers_brothers[0].stuff[0] = 1;
#line 96
    Brothers_brothers[0].stuff[2] = 1;
#line 97
    Brothers_brothers[0].stuff[11] = 6;
#line 98
    Brothers_brothers[0].stuff[13] = 4;
#line 99
    Brothers_brothers[0].startX = 19036;
#line 100
    Brothers_brothers[0].startY = 15755;
#line 101
    Brothers_brothers[0].alive = 1;
#line 103
    m2_Strings_Assign("Philip", Brothers_brothers[1].name, 15);
#line 104
    Brothers_brothers[1].vitality = 20;
#line 105
    Brothers_brothers[1].weapon = 0;
#line 106
    Brothers_brothers[1].brave = 20;
#line 107
    Brothers_brothers[1].luck = 35;
#line 108
    Brothers_brothers[1].kind = 15;
#line 109
    Brothers_brothers[1].wealth = 15;
#line 110
    Brothers_ClearInventory(&Brothers_brothers[1]);
#line 111
    Brothers_brothers[1].startX = 19036;
#line 112
    Brothers_brothers[1].startY = 15755;
#line 113
    Brothers_brothers[1].alive = 1;
#line 115
    m2_Strings_Assign("Kevin", Brothers_brothers[2].name, 15);
#line 116
    Brothers_brothers[2].vitality = 18;
#line 117
    Brothers_brothers[2].weapon = 0;
#line 118
    Brothers_brothers[2].brave = 15;
#line 119
    Brothers_brothers[2].luck = 20;
#line 120
    Brothers_ClearInventory(&Brothers_brothers[2]);
#line 121
    Brothers_brothers[2].kind = 35;
#line 122
    Brothers_brothers[2].wealth = 10;
#line 123
    Brothers_brothers[2].startX = 19036;
#line 124
    Brothers_brothers[2].startY = 15755;
#line 125
    Brothers_brothers[2].alive = 1;
    return;
}

static void Brothers_SaveBrotherState(void) {
#line 130
    Brothers_brothers[Brothers_activeBrother].vitality = Actor_actors[0].vitality;
#line 131
    Brothers_brothers[Brothers_activeBrother].weapon = Actor_actors[0].weapon;
    return;
}

static void Brothers_RestoreBrotherState(void) {
#line 136
    Actor_actors[0].absX = Brothers_brothers[Brothers_activeBrother].startX;
#line 137
    Actor_actors[0].absY = Brothers_brothers[Brothers_activeBrother].startY;
#line 138
    Actor_actors[0].vitality = Brothers_brothers[Brothers_activeBrother].vitality;
#line 139
    Actor_actors[0].weapon = Brothers_brothers[Brothers_activeBrother].weapon;
#line 140
    Actor_actors[0].state = 13;
#line 141
    Actor_actors[0].facing = 4;
    return;
}

static int Brothers_SwitchToNext(void) {
    int32_t i;
    int32_t next;
#line 147
    Brothers_brothers[Brothers_activeBrother].alive = 0;
    i = 1;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
#line 151
    next = m2_mod((Brothers_activeBrother + i), 3);
    if (Brothers_brothers[next].alive) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
#line 153
    Brothers_activeBrother = next;
#line 155
    Brothers_ClearInventory(&Brothers_brothers[next]);
#line 156
    Brothers_brothers[next].stuff[0] = 1;
#line 157
    Brothers_brothers[next].weapon = 1;
#line 158
    Brothers_RestoreBrotherState();
    return 1;
}

static void Brothers_ActiveName(char *name, uint32_t name_high) {
    (void)name;
    (void)name_high;
#line 168
    m2_Strings_Assign(Brothers_brothers[Brothers_activeBrother].name, name, name_high);
    return;
}

/* Imported Module HudLog */

static const int32_t HudLog_NumLines = 4;
static const int32_t HudLog_MaxLineLen = 40;
static void HudLog_InitHudLog(void);
static void HudLog_ScrollUp(void);
static void HudLog_InsertLine(char *s, uint32_t s_high);
static void HudLog_AddLogLine(char *msg, uint32_t msg_high);
static void HudLog_AppendToLine(char *msg, uint32_t msg_high);
static void HudLog_SetStats(int32_t b, int32_t l, int32_t k, int32_t w, int32_t v);
static void HudLog_GetLine(int32_t row, char *buf, uint32_t buf_high);
static int32_t HudLog_GetStatBrv(void);
static int32_t HudLog_GetStatLck(void);
static int32_t HudLog_GetStatKnd(void);
static int32_t HudLog_GetStatWlth(void);
static int32_t HudLog_GetStatVit(void);

int HudLog_logDirty;
int HudLog_statDirty;
char HudLog_lines[3 + 1][39 + 1];
int32_t HudLog_brv;
int32_t HudLog_lck;
int32_t HudLog_knd;
int32_t HudLog_wlth;
int32_t HudLog_vit;
static void HudLog_InitHudLog(void) {
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (4 - 1)) goto L2; else goto L4;
  L2:;
#line 13 "src/HudLog.mod"
    HudLog_lines[i][0] = '\0';
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 15
    HudLog_brv = 0;
    HudLog_lck = 0;
    HudLog_knd = 0;
    HudLog_wlth = 0;
    HudLog_vit = 0;
#line 16
    HudLog_logDirty = 1;
#line 17
    HudLog_statDirty = 1;
    return;
}

static void HudLog_ScrollUp(void) {
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (4 - 2)) goto L2; else goto L4;
  L2:;
#line 24
    m2_Strings_Assign(HudLog_lines[(i + 1)], HudLog_lines[i], 39);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 26
    HudLog_lines[(4 - 1)][0] = '\0';
    return;
}

static void HudLog_InsertLine(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
#line 31
    HudLog_ScrollUp();
#line 32
    m2_Strings_Assign(s, HudLog_lines[(4 - 1)], 39);
#line 33
    HudLog_logDirty = 1;
    return;
}

static void HudLog_AddLogLine(char *msg, uint32_t msg_high) {
    (void)msg;
    (void)msg_high;
    static const int32_t WrapCol = 37;
    int32_t src;
    char buf[39 + 1];
    int32_t bi;
    int32_t lastSpace;
    char ch;
    char tail[39 + 1];
    int32_t ti;
    int32_t i;
#line 48
    src = 0;
#line 49
    bi = 0;
#line 50
    lastSpace = (-1);
    goto L1;
  L1:;
    if (src <= msg_high) goto L4; else goto L3;
  L2:;
#line 53
    ch = msg[src];
#line 54
    (src++);
    if (ch == ' ') goto L6; else goto L5;
  L3:;
    if (bi > 0) goto L17; else goto L16;
  L4:;
    if (msg[src] != '\0') goto L2; else goto L3;
  L5:;
#line 60
    buf[bi] = ch;
#line 61
    (bi++);
    if (bi >= 37) goto L8; else goto L7;
  L6:;
#line 57
    lastSpace = bi;
    goto L5;
  L7:;
    goto L1;
  L8:;
    if (lastSpace > 0) goto L10; else goto L11;
  L9:;
    goto L7;
  L10:;
#line 67
    buf[lastSpace] = '\0';
#line 68
    HudLog_InsertLine(buf, 39);
#line 70
    ti = 0;
    i = (lastSpace + 1);
    goto L12;
  L11:;
#line 79
    buf[bi] = '\0';
#line 80
    HudLog_InsertLine(buf, 39);
#line 81
    bi = 0;
#line 82
    lastSpace = (-1);
    goto L9;
  L12:;
    if (i <= (bi - 1)) goto L13; else goto L15;
  L13:;
#line 72
    buf[ti] = buf[i];
#line 73
    (ti++);
    goto L14;
  L14:;
    i = (i + 1);
    goto L12;
  L15:;
#line 75
    bi = ti;
#line 76
    lastSpace = (-1);
    goto L9;
  L16:;
    return;
  L17:;
#line 89
    buf[bi] = '\0';
#line 90
    HudLog_InsertLine(buf, 39);
    goto L16;
}

static void HudLog_AppendToLine(char *msg, uint32_t msg_high) {
    (void)msg;
    (void)msg_high;
    int32_t len;
#line 97
    len = m2_Strings_Length(HudLog_lines[(4 - 1)]);
    if ((len + m2_Strings_Length(msg)) < 40) goto L2; else goto L1;
  L1:;
#line 101
    HudLog_logDirty = 1;
    return;
  L2:;
#line 99
    m2_Strings_Concat(HudLog_lines[(4 - 1)], msg, HudLog_lines[(4 - 1)], 39);
    goto L1;
}

static void HudLog_SetStats(int32_t b, int32_t l, int32_t k, int32_t w, int32_t v) {
    (void)b;
    (void)l;
    (void)k;
    (void)w;
    (void)v;
    if (b != HudLog_brv) goto L2; else goto L6;
  L1:;
    return;
  L2:;
#line 108
    HudLog_brv = b;
    HudLog_lck = l;
    HudLog_knd = k;
    HudLog_wlth = w;
    HudLog_vit = v;
#line 109
    HudLog_statDirty = 1;
    goto L1;
  L3:;
    if (v != HudLog_vit) goto L2; else goto L1;
  L4:;
    if (w != HudLog_wlth) goto L2; else goto L3;
  L5:;
    if (k != HudLog_knd) goto L2; else goto L4;
  L6:;
    if (l != HudLog_lck) goto L2; else goto L5;
}

static void HudLog_GetLine(int32_t row, char *buf, uint32_t buf_high) {
    (void)row;
    (void)buf;
    (void)buf_high;
    if (row >= 0) goto L4; else goto L3;
  L1:;
    return;
  L2:;
#line 116
    m2_Strings_Assign(HudLog_lines[row], buf, buf_high);
    goto L1;
  L3:;
#line 118
    buf[0] = '\0';
    goto L1;
  L4:;
    if (row < 4) goto L2; else goto L3;
}

static int32_t HudLog_GetStatBrv(void) {
    return HudLog_brv;
}

static int32_t HudLog_GetStatLck(void) {
    return HudLog_lck;
}

static int32_t HudLog_GetStatKnd(void) {
    return HudLog_knd;
}

static int32_t HudLog_GetStatWlth(void) {
    return HudLog_wlth;
}

static int32_t HudLog_GetStatVit(void) {
    return HudLog_vit;
}

/* Imported Module Doors */

typedef struct Doors_DoorRec Doors_DoorRec;
typedef struct Doors_OpenEntry Doors_OpenEntry;
typedef struct Doors_UnlockedDoor Doors_UnlockedDoor;
typedef struct Doors_SavedTile Doors_SavedTile;
static const int32_t Doors_DoorCount = 86;
typedef struct Doors_DoorRec Doors_DoorRec;
struct Doors_DoorRec {
    int32_t xc1;
    int32_t yc1;
    int32_t xc2;
    int32_t yc2;
    int32_t dtype;
    int32_t secs;
};

typedef struct Doors_OpenEntry Doors_OpenEntry;
struct Doors_OpenEntry {
    int32_t doorId;
    int32_t mapId;
    int32_t new1;
    int32_t new2;
    int32_t above;
    int32_t keyType;
};

typedef struct Doors_UnlockedDoor Doors_UnlockedDoor;
struct Doors_UnlockedDoor {
    int32_t px;
    int32_t py;
    int32_t region;
};

typedef struct Doors_SavedTile Doors_SavedTile;
struct Doors_SavedTile {
    int32_t px;
    int32_t py;
    int32_t oldVal;
};

static const int32_t Doors_CAVE = 18;
static const int32_t Doors_STAIR = 15;
static const int32_t Doors_OpenCount = 17;
static const int32_t Doors_MaxUnlockedDoors = 128;
static void Doors_SetDoor(int32_t i, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t dt, int32_t sc);
static void Doors_InitDoors(void);
static int Doors_CheckDoor(int32_t heroX, int32_t heroY, int32_t regionNum, int32_t *newX, int32_t *newY, int32_t *newRegion);
static int Doors_IsUnlocked(int32_t px, int32_t py, int32_t region);
static void Doors_RememberUnlocked(int32_t px, int32_t py, int32_t region);
static int32_t Doors_GetUnlockedCount(void);
static void Doors_GetUnlockedDoor(int32_t i, int32_t *px, int32_t *py, int32_t *region);
static void Doors_ClearUnlockedDoors(void);
static void Doors_AddUnlockedDoor(int32_t px, int32_t py, int32_t region);
static void Doors_InitOpenList(void);
static void Doors_SaveAndSet(int32_t px, int32_t py, int32_t newVal);
static void Doors_RestoreDoorTiles(void);
static int Doors_OpenDoorTile(int32_t heroX, int32_t heroY);
static void Doors_CheckCloseDoors(int32_t heroX, int32_t heroY);
static int Doors_UseKeyOnDoor(int32_t heroX, int32_t heroY, int32_t keyType);

Doors_DoorRec Doors_doors[85 + 1];
Doors_OpenEntry Doors_openList[16 + 1];
int Doors_bumped;
Doors_UnlockedDoor Doors_unlockedDoors[127 + 1];
int32_t Doors_unlockedCount;
Doors_SavedTile Doors_savedTiles[7 + 1];
int32_t Doors_savedCount;
static void Doors_InitOpenList_O(int32_t i, int32_t did, int32_t mid, int32_t n1, int32_t n2, int32_t ab, int32_t kt) {
#line 315 "src/Doors.mod"
    Doors_openList[i].doorId = did;
#line 316
    Doors_openList[i].mapId = mid;
#line 317
    Doors_openList[i].new1 = n1;
#line 318
    Doors_openList[i].new2 = n2;
#line 319
    Doors_openList[i].above = ab;
#line 320
    Doors_openList[i].keyType = kt;
    return;
}

static void Doors_SetDoor(int32_t i, int32_t x1, int32_t m2_y1, int32_t x2, int32_t y2, int32_t dt, int32_t sc) {
    (void)i;
    (void)x1;
    (void)m2_y1;
    (void)x2;
    (void)y2;
    (void)dt;
    (void)sc;
#line 27
    Doors_doors[i].xc1 = x1;
    Doors_doors[i].yc1 = m2_y1;
#line 28
    Doors_doors[i].xc2 = x2;
    Doors_doors[i].yc2 = y2;
#line 29
    Doors_doors[i].dtype = dt;
    Doors_doors[i].secs = sc;
    return;
}

static void Doors_InitDoors(void) {
#line 34
    Doors_SetDoor(0, 4464, 20576, 10352, 35680, 1, 1);
#line 35
    Doors_SetDoor(1, 4464, 20576, 10352, 35680, 1, 1);
#line 36
    Doors_SetDoor(2, 4464, 20576, 10352, 35680, 1, 1);
#line 37
    Doors_SetDoor(3, 4464, 20576, 10352, 35680, 1, 1);
#line 38
    Doors_SetDoor(4, 5008, 7008, 6528, 35936, 18, 2);
#line 39
    Doors_SetDoor(5, 6000, 27296, 8816, 38560, 9, 1);
#line 40
    Doors_SetDoor(6, 6512, 25248, 8048, 38560, 9, 1);
#line 41
    Doors_SetDoor(7, 6816, 19360, 5024, 38304, 17, 1);
#line 42
    Doors_SetDoor(8, 6816, 19552, 5024, 38752, 17, 1);
#line 43
    Doors_SetDoor(9, 6944, 19296, 5920, 38240, 17, 1);
#line 44
    Doors_SetDoor(10, 7040, 19328, 5504, 38272, 17, 1);
#line 45
    Doors_SetDoor(11, 7040, 19520, 5504, 38720, 17, 1);
#line 46
    Doors_SetDoor(12, 7792, 15200, 10368, 40032, 3, 1);
#line 47
    Doors_SetDoor(13, 9344, 13216, 11904, 36256, 1, 1);
#line 48
    Doors_SetDoor(14, 10592, 34656, 11008, 37568, 15, 1);
#line 49
    Doors_SetDoor(15, 11008, 37568, 10592, 34688, 15, 2);
#line 50
    Doors_SetDoor(16, 11264, 29024, 10992, 37728, 9, 1);
#line 51
    Doors_SetDoor(17, 12144, 11872, 12672, 39520, 3, 1);
#line 52
    Doors_SetDoor(18, 12144, 25504, 7280, 38560, 9, 1);
#line 53
    Doors_SetDoor(19, 12672, 14528, 10112, 39104, 1, 1);
#line 54
    Doors_SetDoor(20, 13424, 19296, 1136, 36576, 15, 2);
#line 55
    Doors_SetDoor(21, 15840, 7104, 12000, 37824, 7, 1);
#line 56
    Doors_SetDoor(22, 15872, 7104, 12032, 37824, 7, 1);
#line 57
    Doors_SetDoor(23, 17008, 9568, 11904, 39520, 3, 1);
#line 58
    Doors_SetDoor(24, 17024, 15296, 10624, 39104, 1, 1);
#line 59
    Doors_SetDoor(25, 17888, 21376, 9680, 38528, 10, 1);
#line 60
    Doors_SetDoor(26, 18304, 12224, 9600, 39104, 1, 1);
#line 61
    Doors_SetDoor(27, 18528, 26176, 7264, 39488, 18, 1);
#line 62
    Doors_SetDoor(28, 18576, 26272, 7312, 39584, 11, 1);
#line 63
    Doors_SetDoor(29, 18784, 23360, 8800, 39488, 18, 1);
#line 64
    Doors_SetDoor(30, 18832, 23456, 8848, 39584, 11, 1);
#line 65
    Doors_SetDoor(31, 18848, 15552, 2976, 33472, 2, 1);
#line 66
    Doors_SetDoor(32, 18896, 15808, 3024, 33984, 2, 1);
#line 67
    Doors_SetDoor(33, 18896, 15872, 3024, 34048, 2, 1);
#line 68
    Doors_SetDoor(34, 18960, 15488, 3344, 33408, 1, 1);
#line 69
    Doors_SetDoor(35, 18960, 15680, 3856, 33600, 1, 1);
#line 70
    Doors_SetDoor(36, 18992, 15808, 3632, 34240, 1, 1);
#line 71
    Doors_SetDoor(37, 19040, 16000, 4192, 34176, 1, 1);
#line 72
    Doors_SetDoor(38, 19056, 15488, 4976, 33408, 1, 1);
#line 73
    Doors_SetDoor(39, 19072, 15680, 4496, 33600, 1, 1);
#line 74
    Doors_SetDoor(40, 19568, 12896, 9600, 40032, 3, 1);
#line 75
    Doors_SetDoor(41, 19808, 21568, 8032, 40000, 18, 1);
#line 76
    Doors_SetDoor(42, 19856, 17280, 12416, 36224, 13, 1);
#line 77
    Doors_SetDoor(43, 19856, 21664, 8080, 40096, 11, 1);
#line 78
    Doors_SetDoor(44, 19936, 27520, 10704, 38528, 10, 1);
#line 79
    Doors_SetDoor(45, 21344, 22592, 8800, 38976, 18, 1);
#line 80
    Doors_SetDoor(46, 21392, 22688, 8848, 39072, 11, 1);
#line 81
    Doors_SetDoor(47, 21600, 17728, 7264, 38976, 18, 1);
#line 82
    Doors_SetDoor(48, 21616, 25728, 11392, 36224, 3, 1);
#line 83
    Doors_SetDoor(49, 21648, 17824, 7312, 39072, 11, 1);
#line 84
    Doors_SetDoor(50, 22000, 21216, 5856, 33760, 10, 1);
#line 85
    Doors_SetDoor(51, 22208, 21440, 7104, 33984, 13, 1);
#line 86
    Doors_SetDoor(52, 22208, 21568, 6592, 34112, 13, 1);
#line 87
    Doors_SetDoor(53, 22256, 20896, 6640, 33440, 13, 1);
#line 88
    Doors_SetDoor(54, 22272, 21056, 7664, 33600, 14, 1);
#line 89
    Doors_SetDoor(55, 22288, 21568, 7184, 34368, 13, 1);
#line 90
    Doors_SetDoor(56, 22320, 21248, 6736, 33792, 13, 1);
#line 91
    Doors_SetDoor(57, 22320, 21376, 7216, 33920, 14, 1);
#line 92
    Doors_SetDoor(58, 22352, 20896, 7264, 33440, 13, 1);
#line 93
    Doors_SetDoor(59, 22352, 21088, 8272, 33632, 13, 1);
#line 94
    Doors_SetDoor(60, 22368, 21440, 8288, 33984, 13, 1);
#line 95
    Doors_SetDoor(61, 22368, 21568, 7776, 34112, 13, 1);
#line 96
    Doors_SetDoor(62, 22624, 23872, 7264, 39488, 18, 1);
#line 97
    Doors_SetDoor(63, 22672, 23968, 7312, 40096, 11, 1);
#line 98
    Doors_SetDoor(64, 22720, 11872, 2752, 34912, 18, 2);
#line 99
    Doors_SetDoor(65, 22880, 28480, 8800, 39488, 18, 1);
#line 100
    Doors_SetDoor(66, 22928, 28576, 8848, 40096, 11, 1);
#line 101
    Doors_SetDoor(67, 22944, 26464, 10912, 35680, 15, 1);
#line 102
    Doors_SetDoor(68, 23008, 22656, 10192, 38528, 10, 1);
#line 103
    Doors_SetDoor(69, 24176, 6752, 9600, 39520, 3, 1);
#line 104
    Doors_SetDoor(70, 24256, 10592, 4544, 35680, 18, 2);
#line 105
    Doors_SetDoor(71, 24672, 29248, 6496, 40000, 18, 1);
#line 106
    Doors_SetDoor(72, 24720, 29344, 6544, 40096, 11, 1);
#line 107
    Doors_SetDoor(73, 24816, 12992, 9712, 35776, 3, 1);
#line 108
    Doors_SetDoor(74, 25792, 6240, 960, 34400, 18, 2);
#line 109
    Doors_SetDoor(75, 25952, 23872, 8032, 39488, 18, 1);
#line 110
    Doors_SetDoor(76, 26000, 23968, 8080, 39072, 11, 1);
#line 111
    Doors_SetDoor(77, 26048, 6688, 1200, 34880, 9, 2);
#line 112
    Doors_SetDoor(78, 26224, 10848, 11136, 39520, 3, 1);
#line 113
    Doors_SetDoor(79, 26624, 7008, 10992, 36960, 9, 1);
#line 114
    Doors_SetDoor(80, 27472, 17280, 10320, 36224, 13, 1);
#line 115
    Doors_SetDoor(81, 27616, 31872, 11216, 38528, 10, 1);
#line 116
    Doors_SetDoor(82, 27760, 11872, 10368, 39520, 3, 1);
#line 117
    Doors_SetDoor(83, 28000, 26688, 8032, 39488, 18, 1);
#line 118
    Doors_SetDoor(84, 28048, 26784, 8080, 39584, 11, 1);
#line 119
    Doors_SetDoor(85, 28384, 21120, 12752, 38528, 10, 1);
    return;
}

static int Doors_CheckDoor(int32_t heroX, int32_t heroY, int32_t regionNum, int32_t *newX, int32_t *newY, int32_t *newRegion) {
    (void)heroX;
    (void)heroY;
    (void)regionNum;
    (void)newX;
    (void)newY;
    (void)newRegion;
    int32_t i;
    int32_t k;
    int32_t j;
    int32_t xtest;
    int32_t ytest;
    int32_t dt;
    Doors_DoorRec d;
#line 127
    xtest = (m2_div(heroX, 16) * 16);
#line 128
    ytest = (m2_div(heroY, 32) * 32);
    if (regionNum < 8) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
#line 132
    i = 0;
#line 133
    k = (86 - 1);
    goto L4;
  L3:;
    j = 0;
    goto L36;
  L4:;
    if (k >= i) goto L5; else goto L6;
  L5:;
#line 135
    j = m2_div((k + i), 2);
#line 136
    d = Doors_doors[j];
    if (d.xc1 > xtest) goto L8; else goto L9;
  L6:;
    goto L1;
  L7:;
    if (i >= 86) goto L34; else goto L35;
  L8:;
#line 138
    k = (j - 1);
    goto L7;
  L9:;
    if ((d.xc1 + 16) < xtest) goto L10; else goto L11;
  L10:;
#line 140
    i = (j + 1);
    goto L7;
  L11:;
    if (d.xc1 < xtest) goto L14; else goto L13;
  L12:;
#line 142
    i = (j + 1);
    goto L7;
  L13:;
    if (d.yc1 > ytest) goto L15; else goto L16;
  L14:;
    if (m2_mod(d.dtype, 2) == 0) goto L12; else goto L13;
  L15:;
#line 144
    k = (j - 1);
    goto L7;
  L16:;
    if (d.yc1 < ytest) goto L17; else goto L18;
  L17:;
#line 146
    i = (j + 1);
    goto L7;
  L18:;
#line 148
    dt = d.dtype;
    if (m2_mod(dt, 2) == 1) goto L20; else goto L21;
  L19:;
    if (dt == 18) goto L26; else goto L27;
  L20:;
    if (((uint32_t)(((uint32_t)(heroY))) & (uint32_t)(16)) != 0) goto L23; else goto L22;
  L21:;
    if (m2_mod(heroX, 16) > 6) goto L24; else goto L19;
  L22:;
    goto L19;
  L23:;
    return 0;
  L24:;
    return 0;
  L25:;
    if (d.secs == 1) goto L31; else goto L32;
  L26:;
#line 161
    (*newX) = (d.xc2 + 24);
#line 162
    (*newY) = (d.yc2 + 16);
    goto L25;
  L27:;
    if (m2_mod(dt, 2) == 1) goto L28; else goto L29;
  L28:;
#line 164
    (*newX) = (d.xc2 + 16);
#line 165
    (*newY) = d.yc2;
    goto L25;
  L29:;
#line 167
    (*newX) = (d.xc2 - 1);
#line 168
    (*newY) = (d.yc2 + 16);
    goto L25;
  L30:;
#line 175
    m2_WriteString("Door: entering region ");
#line 176
    m2_WriteInt((*newRegion), 1);
#line 177
    m2_WriteString(" via door ");
    m2_WriteInt(j, 1);
#line 178
    m2_WriteString(" at ");
    m2_WriteInt(d.xc1, 1);
#line 179
    m2_WriteString(",");
    m2_WriteInt(d.yc1, 1);
#line 180
    m2_WriteString(" -> ");
    m2_WriteInt((*newX), 1);
#line 181
    m2_WriteString(",");
    m2_WriteInt((*newY), 1);
    m2_WriteLn();
    return 1;
  L31:;
#line 171
    (*newRegion) = 8;
    goto L30;
  L32:;
#line 173
    (*newRegion) = 9;
    goto L30;
  L33:;
    goto L4;
  L34:;
    return 0;
  L35:;
    if (k < 0) goto L34; else goto L33;
  L36:;
    if (j <= (86 - 1)) goto L37; else goto L39;
  L37:;
#line 194
    d = Doors_doors[j];
    if (d.yc2 == ytest) goto L42; else goto L40;
  L38:;
    j = (j + 1);
    goto L36;
  L39:;
    goto L1;
  L40:;
    goto L38;
  L41:;
#line 198
    dt = d.dtype;
    if (m2_mod(dt, 2) == 1) goto L46; else goto L47;
  L42:;
    if (d.xc2 == xtest) goto L41; else goto L43;
  L43:;
    if (d.xc2 == (xtest - 16)) goto L44; else goto L40;
  L44:;
    if (m2_mod(d.dtype, 2) == 1) goto L41; else goto L40;
  L45:;
    goto L40;
  L46:;
    if (((uint32_t)(((uint32_t)(heroY))) & (uint32_t)(16)) == 0) goto L49; else goto L50;
  L47:;
    if (m2_mod(heroX, 16) < 2) goto L55; else goto L56;
  L48:;
    goto L45;
  L49:;
    goto L48;
  L50:;
    if (dt == 18) goto L52; else goto L53;
  L51:;
#line 209
    (*newRegion) = (-1);
    return 1;
  L52:;
#line 205
    (*newX) = (d.xc1 - 4);
    (*newY) = (d.yc1 + 16);
    goto L51;
  L53:;
#line 207
    (*newX) = (d.xc1 + 16);
    (*newY) = (d.yc1 + 34);
    goto L51;
  L54:;
    goto L45;
  L55:;
    goto L54;
  L56:;
    if (dt == 18) goto L58; else goto L59;
  L57:;
#line 221
    (*newRegion) = (-1);
    return 1;
  L58:;
#line 217
    (*newX) = (d.xc1 - 4);
    (*newY) = (d.yc1 + 16);
    goto L57;
  L59:;
#line 219
    (*newX) = (d.xc1 + 20);
    (*newY) = (d.yc1 + 16);
    goto L57;
}

static int Doors_IsUnlocked(int32_t px, int32_t py, int32_t region) {
    (void)px;
    (void)py;
    (void)region;
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Doors_unlockedCount - 1)) goto L2; else goto L4;
  L2:;
    if (Doors_unlockedDoors[i].px == px) goto L8; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
    return 1;
  L7:;
    if (Doors_unlockedDoors[i].region == region) goto L6; else goto L5;
  L8:;
    if (Doors_unlockedDoors[i].py == py) goto L7; else goto L5;
}

static void Doors_RememberUnlocked(int32_t px, int32_t py, int32_t region) {
    (void)px;
    (void)py;
    (void)region;
    if (Doors_IsUnlocked(px, py, region)) goto L2; else goto L1;
  L1:;
    if (Doors_unlockedCount >= 128) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 279
    Doors_unlockedDoors[Doors_unlockedCount].px = px;
#line 280
    Doors_unlockedDoors[Doors_unlockedCount].py = py;
#line 281
    Doors_unlockedDoors[Doors_unlockedCount].region = region;
#line 282
    (Doors_unlockedCount++);
    return;
  L4:;
    return;
}

static int32_t Doors_GetUnlockedCount(void) {
    return Doors_unlockedCount;
}

static void Doors_GetUnlockedDoor(int32_t i, int32_t *px, int32_t *py, int32_t *region) {
    (void)i;
    (void)px;
    (void)py;
    (void)region;
    if (i < 0) goto L2; else goto L3;
  L1:;
#line 296
    (*px) = Doors_unlockedDoors[i].px;
#line 297
    (*py) = Doors_unlockedDoors[i].py;
#line 298
    (*region) = Doors_unlockedDoors[i].region;
    return;
  L2:;
#line 293
    (*px) = 0;
    (*py) = 0;
    (*region) = (-1);
    return;
  L3:;
    if (i >= Doors_unlockedCount) goto L2; else goto L1;
}

static void Doors_ClearUnlockedDoors(void) {
#line 303
    Doors_unlockedCount = 0;
    return;
}

static void Doors_AddUnlockedDoor(int32_t px, int32_t py, int32_t region) {
    (void)px;
    (void)py;
    (void)region;
    if (region < 0) goto L2; else goto L3;
  L1:;
#line 309
    Doors_RememberUnlocked(px, py, region);
    return;
  L2:;
    return;
  L3:;
    if (region > 9) goto L2; else goto L1;
}

static void Doors_InitOpenList(void) {
#line 324
    Doors_InitOpenList_O(0, 64, 360, 123, 124, 2, 2);
#line 325
    Doors_InitOpenList_O(1, 120, 360, 125, 126, 2, 0);
#line 326
    Doors_InitOpenList_O(2, 122, 360, 127, 0, 0, 0);
#line 327
    Doors_InitOpenList_O(3, 64, 280, 124, 125, 2, 5);
#line 328
    Doors_InitOpenList_O(4, 77, 280, 126, 0, 0, 5);
#line 329
    Doors_InitOpenList_O(5, 82, 480, 84, 85, 2, 3);
#line 330
    Doors_InitOpenList_O(6, 64, 480, 105, 106, 2, 2);
#line 331
    Doors_InitOpenList_O(7, 128, 240, 154, 155, 1, 6);
#line 332
    Doors_InitOpenList_O(8, 39, 680, 41, 42, 2, 1);
#line 333
    Doors_InitOpenList_O(9, 25, 680, 27, 26, 3, 1);
#line 334
    Doors_InitOpenList_O(10, 114, 760, 116, 117, 1, 4);
#line 335
    Doors_InitOpenList_O(11, 118, 760, 116, 117, 1, 5);
#line 336
    Doors_InitOpenList_O(12, 136, 800, 133, 134, 135, 1);
#line 337
    Doors_InitOpenList_O(13, 187, 800, 76, 77, 2, 0);
#line 338
    Doors_InitOpenList_O(14, 73, 720, 75, 0, 0, 0);
#line 339
    Doors_InitOpenList_O(15, 165, 800, 85, 86, 4, 2);
#line 340
    Doors_InitOpenList_O(16, 210, 840, 208, 209, 2, 0);
    return;
}

static void Doors_SaveAndSet(int32_t px, int32_t py, int32_t newVal) {
    (void)px;
    (void)py;
    (void)newVal;
    if (Doors_savedCount < 8) goto L2; else goto L1;
  L1:;
#line 361
    Assets_SetSectorByte(px, py, newVal);
    return;
  L2:;
#line 356
    Doors_savedTiles[Doors_savedCount].px = px;
#line 357
    Doors_savedTiles[Doors_savedCount].py = py;
#line 358
    Doors_savedTiles[Doors_savedCount].oldVal = Assets_GetSectorByte(px, py);
#line 359
    (Doors_savedCount++);
    goto L1;
}

static void Doors_RestoreDoorTiles(void) {
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Doors_savedCount - 1)) goto L2; else goto L4;
  L2:;
#line 368
    Assets_SetSectorByte(Doors_savedTiles[i].px, Doors_savedTiles[i].py, Doors_savedTiles[i].oldVal);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 370
    Doors_savedCount = 0;
#line 371
    Doors_bumped = 0;
    return;
}

static int Doors_OpenDoorTile(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t x;
    int32_t y;
    int32_t secId;
    int32_t regId;
    int32_t j;
    int32_t k;
    int32_t l;
#line 379
    x = heroX;
#line 380
    y = heroY;
    if (Assets_GetTerrainAt(x, y) != 15) goto L2; else goto L1;
  L1:;
    if (Assets_GetTerrainAt((x - 16), y) == 15) goto L8; else goto L7;
  L2:;
#line 382
    x = (heroX + 4);
    if (Assets_GetTerrainAt(x, y) != 15) goto L4; else goto L3;
  L3:;
    goto L1;
  L4:;
#line 384
    x = (heroX - 4);
    if (Assets_GetTerrainAt(x, y) != 15) goto L6; else goto L5;
  L5:;
    goto L3;
  L6:;
    return 0;
  L7:;
    if (Assets_GetTerrainAt((x - 16), y) == 15) goto L10; else goto L9;
  L8:;
#line 393
    (x -= 16);
    goto L7;
  L9:;
    if (Assets_GetTerrainAt(x, (y + 32)) == 15) goto L12; else goto L11;
  L10:;
#line 394
    (x -= 16);
    goto L9;
  L11:;
#line 398
    x = m2_div(x, 16);
#line 399
    y = m2_div(y, 32);
#line 402
    secId = Assets_GetSectorByte((x * 16), (y * 32));
    if (Assets_currentRegion >= 0) goto L16; else goto L15;
  L12:;
#line 395
    (y += 32);
    goto L11;
  L13:;
    j = 0;
    goto L17;
  L14:;
#line 406
    regId = Assets_regions[Assets_currentRegion].image[(secId / 64)];
    goto L13;
  L15:;
    return 0;
  L16:;
    if (Assets_currentRegion <= 9) goto L14; else goto L15;
  L17:;
    if (j <= (17 - 1)) goto L18; else goto L20;
  L18:;
    if (Doors_openList[j].mapId == regId) goto L23; else goto L21;
  L19:;
    j = (j + 1);
    goto L17;
  L20:;
    return 0;
  L21:;
    goto L19;
  L22:;
#line 414
    k = Doors_openList[j].keyType;
    if (k > 0) goto L27; else goto L24;
  L23:;
    if (Doors_openList[j].doorId == secId) goto L22; else goto L21;
  L24:;
#line 427
    Doors_savedCount = 0;
#line 428
    Doors_SaveAndSet((x * 16), (y * 32), Doors_openList[j].new1);
#line 429
    k = Doors_openList[j].new2;
    if (k > 0) goto L31; else goto L30;
  L25:;
    if (Doors_bumped) goto L28; else goto L29;
  L26:;
    if (Doors_IsUnlocked((x * 16), (y * 32), Assets_currentRegion)) goto L24; else goto L25;
  L27:;
    if (Platform_cheatKeys) goto L24; else goto L26;
  L28:;
    return 0;
  L29:;
#line 421
    HudLog_AddLogLine("It's locked.", 12);
#line 422
    Doors_bumped = 1;
    goto L28;
  L30:;
    return 1;
  L31:;
#line 431
    l = Doors_openList[j].above;
    if (l == 1) goto L33; else goto L34;
  L32:;
    goto L30;
  L33:;
#line 433
    Doors_SaveAndSet((x * 16), ((y - 1) * 32), k);
    goto L32;
  L34:;
    if (l == 3) goto L35; else goto L36;
  L35:;
#line 435
    Doors_SaveAndSet(((x - 1) * 16), (y * 32), k);
    goto L32;
  L36:;
    if (l == 4) goto L37; else goto L38;
  L37:;
#line 437
    Doors_SaveAndSet((x * 16), ((y - 1) * 32), 87);
#line 438
    Doors_SaveAndSet(((x + 1) * 16), (y * 32), 86);
#line 439
    Doors_SaveAndSet(((x + 1) * 16), ((y - 1) * 32), 88);
    goto L32;
  L38:;
#line 441
    Doors_SaveAndSet(((x + 1) * 16), (y * 32), k);
    if (l != 2) goto L40; else goto L39;
  L39:;
    goto L32;
  L40:;
#line 443
    Doors_SaveAndSet(((x + 2) * 16), (y * 32), Doors_openList[j].above);
    goto L39;
}

static void Doors_CheckCloseDoors(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t dx;
    int32_t dy;
    if (Doors_savedCount == 0) goto L2; else goto L1;
  L1:;
#line 457
    dx = (heroX - Doors_savedTiles[0].px);
#line 458
    dy = (heroY - Doors_savedTiles[0].py);
    if (dx < 0) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (dy < 0) goto L6; else goto L5;
  L4:;
#line 459
    dx = (-dx);
    goto L3;
  L5:;
    if (dx > 64) goto L8; else goto L9;
  L6:;
#line 460
    dy = (-dy);
    goto L5;
  L7:;
    return;
  L8:;
#line 462
    Doors_RestoreDoorTiles();
    goto L7;
  L9:;
    if (dy > 64) goto L8; else goto L7;
}

static int Doors_UseKeyOnDoor(int32_t heroX, int32_t heroY, int32_t keyType) {
    (void)heroX;
    (void)heroY;
    (void)keyType;
    static const int32_t Step = 16;
    int32_t dir;
    int32_t tx;
    int32_t ty;
    int32_t x;
    int32_t y;
    int32_t secId;
    int32_t regId;
    int32_t j;
    int32_t k;
    int32_t l;
    int32_t xd[8 + 1];
    int32_t yd[8 + 1];
#line 477
    xd[0] = 0;
    yd[0] = 0;
#line 478
    xd[1] = 0;
    yd[1] = (-16);
#line 479
    xd[2] = 16;
    yd[2] = (-16);
#line 480
    xd[3] = 16;
    yd[3] = 0;
#line 481
    xd[4] = 16;
    yd[4] = 16;
#line 482
    xd[5] = 0;
    yd[5] = 16;
#line 483
    xd[6] = (-16);
    yd[6] = 16;
#line 484
    xd[7] = (-16);
    yd[7] = 0;
#line 485
    xd[8] = (-16);
    yd[8] = (-16);
    dir = 0;
    goto L1;
  L1:;
    if (dir <= 8) goto L2; else goto L4;
  L2:;
#line 488
    tx = (heroX + xd[dir]);
#line 489
    ty = (heroY + yd[dir]);
    if (Assets_GetTerrainAt(tx, ty) == 15) goto L6; else goto L5;
  L3:;
    dir = (dir + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
#line 491
    x = tx;
    y = ty;
    if (Assets_GetTerrainAt((x - 16), y) == 15) goto L8; else goto L7;
  L7:;
    if (Assets_GetTerrainAt((x - 16), y) == 15) goto L10; else goto L9;
  L8:;
#line 493
    (x -= 16);
    goto L7;
  L9:;
    if (Assets_GetTerrainAt(x, (y + 32)) == 15) goto L12; else goto L11;
  L10:;
#line 494
    (x -= 16);
    goto L9;
  L11:;
#line 496
    x = m2_div(x, 16);
    y = m2_div(y, 32);
#line 497
    secId = Assets_GetSectorByte((x * 16), (y * 32));
    if (Assets_currentRegion >= 0) goto L15; else goto L13;
  L12:;
#line 495
    (y += 32);
    goto L11;
  L13:;
    goto L5;
  L14:;
#line 499
    regId = Assets_regions[Assets_currentRegion].image[(secId / 64)];
    j = 0;
    goto L16;
  L15:;
    if (Assets_currentRegion <= 9) goto L14; else goto L13;
  L16:;
    if (j <= (17 - 1)) goto L17; else goto L19;
  L17:;
    if (Doors_openList[j].mapId == regId) goto L23; else goto L20;
  L18:;
    j = (j + 1);
    goto L16;
  L19:;
    goto L13;
  L20:;
    goto L18;
  L21:;
#line 505
    Doors_RememberUnlocked((x * 16), (y * 32), Assets_currentRegion);
#line 506
    Assets_SetSectorByte((x * 16), (y * 32), Doors_openList[j].new1);
#line 507
    k = Doors_openList[j].new2;
    if (k > 0) goto L25; else goto L24;
  L22:;
    if (Doors_openList[j].keyType == keyType) goto L21; else goto L20;
  L23:;
    if (Doors_openList[j].doorId == secId) goto L22; else goto L20;
  L24:;
#line 525
    HudLog_AddLogLine("It opened.", 10);
#line 526
    Doors_bumped = 0;
    return 1;
  L25:;
#line 509
    l = Doors_openList[j].above;
    if (l == 1) goto L27; else goto L28;
  L26:;
    goto L24;
  L27:;
#line 511
    Assets_SetSectorByte((x * 16), ((y - 1) * 32), k);
    goto L26;
  L28:;
    if (l == 3) goto L29; else goto L30;
  L29:;
#line 513
    Assets_SetSectorByte(((x - 1) * 16), (y * 32), k);
    goto L26;
  L30:;
    if (l == 4) goto L31; else goto L32;
  L31:;
#line 515
    Assets_SetSectorByte((x * 16), ((y - 1) * 32), 87);
#line 516
    Assets_SetSectorByte(((x + 1) * 16), (y * 32), 86);
#line 517
    Assets_SetSectorByte(((x + 1) * 16), ((y - 1) * 32), 88);
    goto L26;
  L32:;
#line 519
    Assets_SetSectorByte(((x + 1) * 16), (y * 32), k);
    if (l != 2) goto L34; else goto L33;
  L33:;
    goto L26;
  L34:;
#line 521
    Assets_SetSectorByte(((x + 2) * 16), (y * 32), Doors_openList[j].above);
    goto L33;
}

static void Doors_init(void) {
#line 537
    Doors_savedCount = 0;
#line 538
    Doors_unlockedCount = 0;
#line 539
    Doors_bumped = 0;
#line 540
    Doors_InitOpenList();
    return;
}

/* Imported Module WorldObj */

typedef struct WorldObj_WorldObject WorldObj_WorldObject;
static const int32_t WorldObj_ObjCOrb = 21;
static const int32_t WorldObj_ObjRedKey = 242;
static const int32_t WorldObj_ObjNightshade = 160;
static const int32_t WorldObj_MaxWorldObjs = 1600;
static const int32_t WorldObj_ObjBloodroot = 161;
static const int32_t WorldObj_ObjFireScroll = 164;
static const int32_t WorldObj_ObjScrap = 20;
static const int32_t WorldObj_ObjChest = 15;
static const int32_t WorldObj_ObjWardScroll = 162;
static const int32_t WorldObj_ObjGoldKey = 25;
static const int32_t WorldObj_ObjBook = 150;
static const int32_t WorldObj_ObjFootstool = 31;
static const int32_t WorldObj_ObjStatue = 149;
static const int32_t WorldObj_ObjWolfsbane = 157;
static const int32_t WorldObj_ObjJSkull = 24;
static const int32_t WorldObj_ObjSacks = 16;
static const int32_t WorldObj_ObjYarrow = 159;
static const int32_t WorldObj_ObjUrn = 14;
static const int32_t WorldObj_ObjMugwort = 158;
static const int32_t WorldObj_ObjBStone = 18;
static const int32_t WorldObj_ObjVial = 22;
static const int32_t WorldObj_ObjSanctuaryScroll = 167;
static const int32_t WorldObj_ObjGRing = 17;
static const int32_t WorldObj_ObjLightScroll = 166;
static const int32_t WorldObj_ObjWhiteKey = 154;
static const int32_t WorldObj_ObjFreezeScroll = 163;
static const int32_t WorldObj_ObjMeal = 146;
static const int32_t WorldObj_ObjBTotem = 23;
static const int32_t WorldObj_MaxRegionObjs = 80;
static const int32_t WorldObj_ObjFearScroll = 165;
static const int32_t WorldObj_ObjRose = 147;
static const int32_t WorldObj_ObjMoney = 13;
static const int32_t WorldObj_ObjBlueKey = 114;
static const int32_t WorldObj_ObjFruit = 148;
static const int32_t WorldObj_ObjGreenKey = 153;
static const int32_t WorldObj_ObjHarvestScroll = 168;
static const int32_t WorldObj_ObjTurtle = 102;
static const int32_t WorldObj_ObjHealScroll = 169;
static const int32_t WorldObj_ObjMWand = 145;
static const int32_t WorldObj_ObjMandrake = 156;
static const int32_t WorldObj_ObjShell = 151;
static const int32_t WorldObj_ObjGreyKey = 26;
static const int32_t WorldObj_ObjGJewel = 19;
static const int32_t WorldObj_ObjQuiver = 11;
typedef struct WorldObj_WorldObject WorldObj_WorldObject;
struct WorldObj_WorldObject {
    int32_t x;
    int32_t y;
    int32_t objId;
    int32_t status;
    int32_t region;
};

static const int32_t WorldObj_ObjSprW = 16;
static const int32_t WorldObj_ObjSprH = 16;
static const int32_t WorldObj_ObjSheetH = 2000;
static const int32_t WorldObj_ApplesPerOutdoorRegion = 125;
static int32_t WorldObj_S(int32_t v);
static void WorldObj_AddObj(int32_t x, int32_t y, int32_t id, int32_t m2_stat, int32_t reg);
static void WorldObj_InitWorldObjects(void);
static void WorldObj_LoadObjectSprites(void);
static int32_t WorldObj_BitRand(int32_t mask);
static int32_t WorldObj_RandTreasureId(void);
static int WorldObj_NearTree(int32_t x, int32_t y);
static void WorldObj_AddRegionApples(int32_t region);
static void WorldObj_DistributeRegion(int32_t region);
static int WorldObj_IsRegionDistributed(int32_t region);
static void WorldObj_SetRegionDistributed(int32_t region, int value);
static void WorldObj_LeaveItem(int32_t x, int32_t y, int32_t id);
static void WorldObj_DrawWorldObjects(void);
static int32_t WorldObj_CheckObjectPickup(int32_t heroX, int32_t heroY);

int32_t WorldObj_objCount;
int WorldObj_revealHidden;
void * WorldObj_objTex;
WorldObj_WorldObject WorldObj_objects[1599 + 1];
int32_t WorldObj_rng;
int WorldObj_distributed[9 + 1];
static int32_t WorldObj_S(int32_t v) {
    (void)v;
    return (v * 3);
}

static void WorldObj_AddObj(int32_t x, int32_t y, int32_t id, int32_t m2_stat, int32_t reg) {
    (void)x;
    (void)y;
    (void)id;
    (void)m2_stat;
    (void)reg;
    if (WorldObj_objCount >= 1600) goto L2; else goto L1;
  L1:;
#line 22 "src/WorldObj.mod"
    WorldObj_objects[WorldObj_objCount].x = x;
#line 23
    WorldObj_objects[WorldObj_objCount].y = y;
#line 24
    WorldObj_objects[WorldObj_objCount].objId = id;
#line 25
    WorldObj_objects[WorldObj_objCount].status = m2_stat;
#line 26
    WorldObj_objects[WorldObj_objCount].region = reg;
#line 27
    (WorldObj_objCount++);
    return;
  L2:;
    return;
}

static void WorldObj_InitWorldObjects(void) {
    int32_t r;
#line 33
    WorldObj_objCount = 0;
#line 34
    WorldObj_objTex = NULL;
#line 35
    WorldObj_rng = 31337;
    r = 0;
    goto L1;
  L1:;
    if (r <= 9) goto L2; else goto L4;
  L2:;
#line 36
    WorldObj_distributed[r] = 0;
    goto L3;
  L3:;
    r = (r + 1);
    goto L1;
  L4:;
#line 39
    WorldObj_AddObj(19316, 15747, 11, 0, (-1));
#line 40
    WorldObj_AddObj(18196, 15735, 11, 0, (-1));
#line 41
    WorldObj_AddObj(12439, 36202, 10, 3, (-1));
#line 42
    WorldObj_AddObj(11092, 38526, 149, 1, (-1));
#line 43
    WorldObj_AddObj(25737, 10662, 149, 1, (-1));
#line 44
    WorldObj_AddObj(2910, 39023, 149, 1, (-1));
#line 45
    WorldObj_AddObj(12025, 37639, 149, 0, (-1));
#line 46
    WorldObj_AddObj(6700, 33766, 149, 0, (-1));
#line 49
    WorldObj_AddObj(3340, 6735, 12, 3, 0);
#line 50
    WorldObj_AddObj(9678, 7035, 12, 3, 0);
#line 51
    WorldObj_AddObj(4981, 6306, 12, 3, 0);
#line 54
    WorldObj_AddObj(23087, 5667, 102, 1, 1);
#line 57
    WorldObj_AddObj(13668, 15000, 0, 3, 2);
#line 58
    WorldObj_AddObj(10627, 13154, 0, 3, 2);
#line 59
    WorldObj_AddObj(4981, 10056, 12, 3, 2);
#line 60
    WorldObj_AddObj(13950, 11087, 16, 1, 2);
#line 61
    WorldObj_AddObj(10344, 36171, 151, 1, 2);
#line 64
    WorldObj_AddObj(19298, 16128, 15, 1, 3);
#line 65
    WorldObj_AddObj(18310, 15969, 13, 3, 3);
#line 66
    WorldObj_AddObj(20033, 14401, 0, 3, 3);
#line 67
    WorldObj_AddObj(19386, 15750, 16, 3, 3);
#line 68
    WorldObj_AddObj(19358, 15750, 17, 3, 3);
#line 69
    WorldObj_AddObj(19414, 15750, 18, 3, 3);
#line 70
    WorldObj_AddObj(24794, 13102, 13, 3, 3);
#line 71
    WorldObj_AddObj(21626, 15446, 18, 1, 3);
#line 72
    WorldObj_AddObj(21616, 15456, 13, 1, 3);
#line 73
    WorldObj_AddObj(21636, 15456, 17, 1, 3);
#line 74
    WorldObj_AddObj(20117, 14222, 19, 1, 3);
#line 75
    WorldObj_AddObj(24185, 9840, 16, 1, 3);
#line 76
    WorldObj_AddObj(25769, 10617, 13, 1, 3);
#line 77
    WorldObj_AddObj(25678, 10703, 18, 1, 3);
#line 78
    WorldObj_AddObj(17177, 10599, 20, 1, 3);
#line 79
    WorldObj_AddObj(19026, 15750, 148, 1, 3);
#line 80
    WorldObj_AddObj(19031, 15750, 148, 1, 3);
#line 81
    WorldObj_AddObj(19036, 15750, 148, 1, 3);
#line 82
    WorldObj_AddObj(19041, 15750, 148, 1, 3);
#line 83
    WorldObj_AddObj(19046, 15750, 148, 1, 3);
#line 84
    WorldObj_AddObj(19026, 15755, 148, 1, 3);
#line 85
    WorldObj_AddObj(19031, 15755, 148, 1, 3);
#line 86
    WorldObj_AddObj(19036, 15755, 148, 1, 3);
#line 87
    WorldObj_AddObj(19041, 15755, 148, 1, 3);
#line 88
    WorldObj_AddObj(19046, 15755, 148, 1, 3);
#line 90
    WorldObj_AddObj(18972, 15712, 162, 1, 3);
#line 91
    WorldObj_AddObj(18988, 15712, 163, 1, 3);
#line 92
    WorldObj_AddObj(19004, 15712, 164, 1, 3);
#line 93
    WorldObj_AddObj(19020, 15712, 165, 1, 3);
#line 94
    WorldObj_AddObj(19036, 15712, 166, 1, 3);
#line 95
    WorldObj_AddObj(19052, 15712, 167, 1, 3);
#line 96
    WorldObj_AddObj(19068, 15712, 168, 1, 3);
#line 97
    WorldObj_AddObj(19084, 15712, 169, 1, 3);
#line 98
    WorldObj_AddObj(18972, 15740, 156, 1, 3);
#line 99
    WorldObj_AddObj(18988, 15740, 156, 1, 3);
#line 100
    WorldObj_AddObj(19004, 15740, 156, 1, 3);
#line 101
    WorldObj_AddObj(19020, 15740, 156, 1, 3);
#line 102
    WorldObj_AddObj(19036, 15740, 157, 1, 3);
#line 103
    WorldObj_AddObj(19052, 15740, 157, 1, 3);
#line 104
    WorldObj_AddObj(19068, 15740, 157, 1, 3);
#line 105
    WorldObj_AddObj(19084, 15740, 157, 1, 3);
#line 106
    WorldObj_AddObj(18972, 15768, 158, 1, 3);
#line 107
    WorldObj_AddObj(18988, 15768, 158, 1, 3);
#line 108
    WorldObj_AddObj(19004, 15768, 158, 1, 3);
#line 109
    WorldObj_AddObj(19020, 15768, 158, 1, 3);
#line 110
    WorldObj_AddObj(19036, 15768, 159, 1, 3);
#line 111
    WorldObj_AddObj(19052, 15768, 159, 1, 3);
#line 112
    WorldObj_AddObj(19068, 15768, 159, 1, 3);
#line 113
    WorldObj_AddObj(19084, 15768, 159, 1, 3);
#line 114
    WorldObj_AddObj(18972, 15796, 160, 1, 3);
#line 115
    WorldObj_AddObj(18988, 15796, 160, 1, 3);
#line 116
    WorldObj_AddObj(19004, 15796, 160, 1, 3);
#line 117
    WorldObj_AddObj(19020, 15796, 160, 1, 3);
#line 118
    WorldObj_AddObj(19036, 15796, 161, 1, 3);
#line 119
    WorldObj_AddObj(19052, 15796, 161, 1, 3);
#line 120
    WorldObj_AddObj(19068, 15796, 161, 1, 3);
#line 121
    WorldObj_AddObj(19084, 15796, 161, 1, 3);
#line 123
    WorldObj_AddObj(21480, 15360, 14, 3, 3);
#line 124
    WorldObj_AddObj(21528, 15360, 14, 3, 3);
#line 125
    WorldObj_AddObj(21504, 15336, 14, 3, 3);
#line 126
    WorldObj_AddObj(21504, 15384, 14, 3, 3);
#line 127
    WorldObj_AddObj(21487, 15343, 14, 3, 3);
#line 128
    WorldObj_AddObj(21521, 15377, 14, 3, 3);
#line 129
    WorldObj_AddObj(21504, 15360, 15, 3, 3);
#line 132
    WorldObj_AddObj(6817, 19693, 13, 3, 4);
#line 135
    WorldObj_AddObj(22184, 21156, 13, 3, 5);
#line 136
    WorldObj_AddObj(18734, 17595, 17, 1, 5);
#line 137
    WorldObj_AddObj(21294, 22648, 15, 1, 5);
#line 138
    WorldObj_AddObj(22956, 19955, 0, 3, 5);
#line 139
    WorldObj_AddObj(28342, 22613, 0, 3, 5);
#line 142
    WorldObj_AddObj(24794, 13102, 13, 3, 6);
#line 145
    WorldObj_AddObj(23297, 5797, 102, 1, 7);
#line 149
    WorldObj_AddObj(6700, 33756, 1, 3, 8);
#line 150
    WorldObj_AddObj(5491, 33780, 5, 3, 8);
#line 151
    WorldObj_AddObj(5592, 33764, 6, 3, 8);
#line 152
    WorldObj_AddObj(5514, 33668, 2, 3, 8);
#line 153
    WorldObj_AddObj(5574, 33668, 2, 3, 8);
#line 154
    WorldObj_AddObj(8878, 38995, 0, 3, 8);
#line 155
    WorldObj_AddObj(7776, 34084, 0, 3, 8);
#line 156
    WorldObj_AddObj(5514, 33881, 3, 3, 8);
#line 157
    WorldObj_AddObj(5574, 33881, 3, 3, 8);
#line 158
    WorldObj_AddObj(10853, 35656, 4, 3, 8);
#line 159
    WorldObj_AddObj(12037, 37614, 7, 3, 8);
#line 160
    WorldObj_AddObj(11013, 36804, 9, 3, 8);
#line 161
    WorldObj_AddObj(9631, 38953, 8, 3, 8);
#line 162
    WorldObj_AddObj(10191, 38953, 8, 3, 8);
#line 163
    WorldObj_AddObj(10649, 38953, 8, 3, 8);
#line 164
    WorldObj_AddObj(2966, 33964, 8, 3, 8);
#line 166
    WorldObj_AddObj(9532, 40002, 31, 1, 8);
#line 167
    WorldObj_AddObj(6747, 33751, 31, 1, 8);
#line 168
    WorldObj_AddObj(11855, 36206, 31, 1, 8);
#line 169
    WorldObj_AddObj(10427, 39977, 31, 1, 8);
#line 171
    WorldObj_AddObj(11410, 36169, 155, 1, 8);
#line 172
    WorldObj_AddObj(9550, 39964, 23, 1, 8);
#line 173
    WorldObj_AddObj(9552, 39964, 23, 1, 8);
#line 174
    WorldObj_AddObj(9682, 39964, 23, 1, 8);
#line 175
    WorldObj_AddObj(9684, 39964, 23, 1, 8);
#line 176
    WorldObj_AddObj(9532, 40119, 23, 1, 8);
#line 177
    WorldObj_AddObj(9575, 39459, 14, 1, 8);
#line 178
    WorldObj_AddObj(9590, 39459, 14, 1, 8);
#line 179
    WorldObj_AddObj(9605, 39459, 14, 1, 8);
#line 180
    WorldObj_AddObj(9680, 39453, 22, 1, 8);
#line 181
    WorldObj_AddObj(9682, 39453, 22, 1, 8);
#line 182
    WorldObj_AddObj(9784, 39453, 22, 1, 8);
#line 183
    WorldObj_AddObj(9668, 39554, 15, 1, 8);
#line 184
    WorldObj_AddObj(11090, 39462, 13, 1, 8);
#line 185
    WorldObj_AddObj(11108, 39458, 23, 1, 8);
#line 186
    WorldObj_AddObj(11118, 39459, 23, 1, 8);
#line 187
    WorldObj_AddObj(11128, 39459, 23, 1, 8);
#line 188
    WorldObj_AddObj(11138, 39458, 23, 1, 8);
#line 189
    WorldObj_AddObj(11148, 39459, 23, 1, 8);
#line 190
    WorldObj_AddObj(11158, 39459, 23, 1, 8);
#line 191
    WorldObj_AddObj(11909, 36198, 15, 1, 8);
#line 192
    WorldObj_AddObj(11918, 36246, 23, 1, 8);
#line 193
    WorldObj_AddObj(11928, 36246, 23, 1, 8);
#line 194
    WorldObj_AddObj(11938, 36246, 23, 1, 8);
#line 195
    WorldObj_AddObj(12212, 38481, 15, 1, 8);
#line 196
    WorldObj_AddObj(11652, 38481, 242, 1, 8);
#line 197
    WorldObj_AddObj(10323, 40071, 14, 1, 8);
#line 198
    WorldObj_AddObj(10059, 38472, 16, 1, 8);
#line 199
    WorldObj_AddObj(10344, 36171, 151, 1, 8);
#line 200
    WorldObj_AddObj(11936, 36207, 20, 1, 8);
#line 201
    WorldObj_AddObj(9674, 35687, 14, 1, 8);
#line 203
    WorldObj_AddObj(5473, 38699, 147, 1, 8);
#line 204
    WorldObj_AddObj(7185, 34342, 148, 1, 8);
#line 205
    WorldObj_AddObj(7190, 34342, 148, 1, 8);
#line 206
    WorldObj_AddObj(7195, 34342, 148, 1, 8);
#line 207
    WorldObj_AddObj(7185, 34347, 148, 1, 8);
#line 208
    WorldObj_AddObj(7190, 34347, 148, 1, 8);
#line 209
    WorldObj_AddObj(7195, 34347, 148, 1, 8);
#line 210
    WorldObj_AddObj(6593, 34085, 148, 1, 8);
#line 211
    WorldObj_AddObj(6598, 34085, 148, 1, 8);
#line 212
    WorldObj_AddObj(6593, 34090, 148, 1, 8);
#line 213
    WorldObj_AddObj(6598, 34090, 148, 1, 8);
#line 216
    WorldObj_AddObj(3872, 33546, 25, 5, 8);
#line 217
    WorldObj_AddObj(3887, 33510, 23, 5, 8);
#line 218
    WorldObj_AddObj(4495, 33510, 22, 5, 8);
#line 219
    WorldObj_AddObj(3327, 33383, 24, 5, 8);
#line 220
    WorldObj_AddObj(4221, 34119, 11, 5, 8);
#line 221
    WorldObj_AddObj(7610, 33604, 22, 5, 8);
#line 222
    WorldObj_AddObj(7616, 33522, 13, 5, 8);
#line 223
    WorldObj_AddObj(9570, 35768, 18, 5, 8);
#line 224
    WorldObj_AddObj(9668, 35769, 11, 5, 8);
#line 225
    WorldObj_AddObj(9553, 38951, 17, 5, 8);
#line 226
    WorldObj_AddObj(10062, 39005, 24, 5, 8);
#line 227
    WorldObj_AddObj(10577, 38951, 22, 5, 8);
#line 228
    WorldObj_AddObj(11062, 39514, 13, 5, 8);
#line 229
    WorldObj_AddObj(8845, 39494, 154, 5, 8);
#line 230
    WorldObj_AddObj(6542, 39494, 19, 5, 8);
#line 231
    WorldObj_AddObj(7313, 38992, 242, 5, 8);
#line 234
    WorldObj_AddObj(7540, 38528, 145, 1, 9);
#line 235
    WorldObj_AddObj(9624, 36559, 145, 1, 9);
#line 236
    WorldObj_AddObj(9624, 37459, 145, 1, 9);
#line 237
    WorldObj_AddObj(8337, 36719, 145, 1, 9);
#line 238
    WorldObj_AddObj(8154, 34890, 15, 1, 9);
#line 239
    WorldObj_AddObj(7826, 35741, 15, 1, 9);
#line 240
    WorldObj_AddObj(3460, 37260, 0, 3, 9);
#line 241
    WorldObj_AddObj(8485, 35725, 13, 1, 9);
#line 242
    WorldObj_AddObj(3723, 39340, 138, 1, 9);
#line 244
    m2_WriteString("World: ");
    m2_WriteInt(WorldObj_objCount, 1);
#line 245
    m2_WriteString(" objects placed");
    m2_WriteLn();
    return;
}

static void WorldObj_LoadObjectSprites(void) {
    char p[127 + 1];
#line 251
    Assets_AssetPath("objects.bmp", 11, p, 127);
#line 252
    WorldObj_objTex = Platform_LoadBMPKeyedTexture(p, 127, 255, 0, 255);
    if (WorldObj_objTex == NULL) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 254
    m2_WriteString("World: object sprites failed");
    m2_WriteLn();
    goto L1;
  L3:;
#line 256
    m2_WriteString("World: object sprites loaded");
    m2_WriteLn();
    goto L1;
}

static int32_t WorldObj_BitRand(int32_t mask) {
    (void)mask;
#line 275
    WorldObj_rng = ((WorldObj_rng * 1103515245) + 12345);
    if (WorldObj_rng < 0) goto L2; else goto L1;
  L1:;
    return ((uint32_t)(((uint32_t)(m2_div(WorldObj_rng, 65536)))) & (uint32_t)(((uint32_t)(mask))));
  L2:;
#line 276
    WorldObj_rng = (-WorldObj_rng);
    goto L1;
}

static int32_t WorldObj_RandTreasureId(void) {
    int32_t r;
#line 283
    r = WorldObj_BitRand(15);
    if (r == 0) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return 16;
  L3:;
    if (r == 1) goto L4; else goto L5;
  L4:;
    return 16;
  L5:;
    if (r == 2) goto L6; else goto L7;
  L6:;
    return 16;
  L7:;
    if (r == 3) goto L8; else goto L9;
  L8:;
    return 16;
  L9:;
    if (r == 4) goto L10; else goto L11;
  L10:;
    return 15;
  L11:;
    if (r == 5) goto L12; else goto L13;
  L12:;
    return 13;
  L13:;
    if (r == 6) goto L14; else goto L15;
  L14:;
    return 25;
  L15:;
    if (r == 7) goto L16; else goto L17;
  L16:;
    return 11;
  L17:;
    if (r == 8) goto L18; else goto L19;
  L18:;
    return 26;
  L19:;
    if (r == 9) goto L20; else goto L21;
  L20:;
    return 26;
  L21:;
    if (r == 10) goto L22; else goto L23;
  L22:;
    return 26;
  L23:;
    if (r == 11) goto L24; else goto L25;
  L24:;
    return 242;
  L25:;
    if (r == 12) goto L26; else goto L27;
  L26:;
    return 23;
  L27:;
    if (r == 13) goto L28; else goto L29;
  L28:;
    return 22;
  L29:;
    if (r == 14) goto L30; else goto L31;
  L30:;
    return 154;
  L31:;
    return 15;
}

static int WorldObj_NearTree(int32_t x, int32_t y) {
    (void)x;
    (void)y;
    return (((Assets_GetTerrainAt((x - 16), y) == 15 || Assets_GetTerrainAt((x + 16), y) == 15) || Assets_GetTerrainAt(x, (y - 16)) == 15) || Assets_GetTerrainAt(x, (y + 16)) == 15);
}

static void WorldObj_AddRegionApples(int32_t region) {
    (void)region;
    int32_t i;
    int32_t tries;
    int32_t x;
    int32_t y;
    i = 1;
    goto L1;
  L1:;
    if (i <= 125) goto L2; else goto L4;
  L2:;
#line 318
    tries = 0;
    goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
#line 320
    x = (WorldObj_BitRand(16383) + (((uint32_t)(((uint32_t)(region))) & (uint32_t)(1)) * 16384));
#line 321
    y = (WorldObj_BitRand(8191) + (((uint32_t)(((uint32_t)(region))) & (uint32_t)(6)) * 4096));
#line 322
    (tries++);
    if (Assets_GetTerrainAt(x, y) == 0) goto L8; else goto L7;
  L6:;
    goto L9;
  L7:;
    if (tries >= 64) goto L6; else goto L5;
  L8:;
    if (WorldObj_NearTree(x, y)) goto L6; else goto L7;
  L9:;
    if (Assets_GetTerrainAt(x, y) != 0) goto L10; else goto L11;
  L10:;
#line 328
    x = (WorldObj_BitRand(16383) + (((uint32_t)(((uint32_t)(region))) & (uint32_t)(1)) * 16384));
#line 329
    y = (WorldObj_BitRand(8191) + (((uint32_t)(((uint32_t)(region))) & (uint32_t)(6)) * 4096));
    goto L9;
  L11:;
#line 331
    WorldObj_AddObj(x, y, 148, 1, region);
    goto L3;
}

static void WorldObj_DistributeRegion(int32_t region) {
    (void)region;
    int32_t i;
    int32_t x;
    int32_t y;
    int32_t terrain;
    if (region < 0) goto L2; else goto L3;
  L1:;
    if (WorldObj_distributed[region]) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (region > 9) goto L2; else goto L1;
  L4:;
    if (region >= 8) goto L7; else goto L6;
  L5:;
    return;
  L6:;
#line 343
    WorldObj_distributed[region] = 1;
#line 344
    m2_WriteString("World: distributing region ");
    m2_WriteInt(region, 1);
    m2_WriteLn();
    i = 0;
    goto L8;
  L7:;
#line 341
    WorldObj_distributed[region] = 1;
    return;
  L8:;
    if (i <= 9) goto L9; else goto L11;
  L9:;
    goto L12;
  L10:;
    i = (i + 1);
    goto L8;
  L11:;
#line 357
    WorldObj_AddRegionApples(region);
    return;
  L12:;
#line 351
    x = (WorldObj_BitRand(16383) + (((uint32_t)(((uint32_t)(region))) & (uint32_t)(1)) * 16384));
#line 352
    y = (WorldObj_BitRand(8191) + (((uint32_t)(((uint32_t)(region))) & (uint32_t)(6)) * 4096));
#line 353
    terrain = Assets_GetTerrainAt(x, y);
    if (terrain == 0) goto L13; else goto L12;
  L13:;
#line 355
    WorldObj_AddObj(x, y, WorldObj_RandTreasureId(), 1, region);
    goto L10;
}

static int WorldObj_IsRegionDistributed(int32_t region) {
    (void)region;
    if (region < 0) goto L2; else goto L3;
  L1:;
    return WorldObj_distributed[region];
  L2:;
    return 0;
  L3:;
    if (region > 9) goto L2; else goto L1;
}

static void WorldObj_SetRegionDistributed(int32_t region, int value) {
    (void)region;
    (void)value;
    if (region < 0) goto L2; else goto L3;
  L1:;
#line 369
    WorldObj_distributed[region] = value;
    return;
  L2:;
    return;
  L3:;
    if (region > 9) goto L2; else goto L1;
}

static void WorldObj_LeaveItem(int32_t x, int32_t y, int32_t id) {
    (void)x;
    (void)y;
    (void)id;
#line 377
    WorldObj_AddObj(x, (y + 10), id, 1, Assets_currentRegion);
    return;
}

static void WorldObj_DrawWorldObjects(void) {
    int32_t i;
    int32_t sx;
    int32_t sy;
    int32_t sprY;
    int32_t ht;
    int32_t id;
    if (WorldObj_objTex == NULL) goto L2; else goto L1;
  L1:;
#line 385
    Canvas_SetClip(Platform_ren, 0, 0, WorldObj_S(320), WorldObj_S(143));
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= (WorldObj_objCount - 1)) goto L4; else goto L6;
  L4:;
    if (WorldObj_objects[i].status == 1) goto L9; else goto L10;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 430
    Canvas_ClearClip(Platform_ren);
    return;
  L7:;
    goto L5;
  L8:;
#line 392
    sx = ((WorldObj_objects[i].x - World_camX) * 3);
#line 393
    sy = ((WorldObj_objects[i].y - World_camY) * 3);
    if (sx > (-WorldObj_S(20))) goto L17; else goto L13;
  L9:;
    if (WorldObj_objects[i].region == Assets_currentRegion) goto L8; else goto L12;
  L10:;
    if (WorldObj_revealHidden) goto L11; else goto L7;
  L11:;
    if (WorldObj_objects[i].status == 5) goto L9; else goto L7;
  L12:;
    if (WorldObj_objects[i].region == (-1)) goto L8; else goto L7;
  L13:;
    goto L7;
  L14:;
#line 399
    id = WorldObj_objects[i].objId;
    if (id >= 156) goto L21; else goto L20;
  L15:;
    if (sy < (WorldObj_S(143) + 20)) goto L14; else goto L13;
  L16:;
    if (sy > (-WorldObj_S(20))) goto L15; else goto L13;
  L17:;
    if (sx < (WorldObj_S(320) + 20)) goto L16; else goto L13;
  L18:;
#line 412
    ht = 16;
    if (id == 27) goto L28; else goto L33;
  L19:;
#line 401
    sprY = (((116 + id) - 156) * 16);
    goto L18;
  L20:;
    if (id >= 162) goto L24; else goto L23;
  L21:;
    if (id <= 161) goto L19; else goto L20;
  L22:;
#line 403
    sprY = ((122 + m2_mod((id - 162), 3)) * 16);
    goto L18;
  L23:;
    if (((uint32_t)(((uint32_t)(id))) & (uint32_t)(128)) != 0) goto L25; else goto L26;
  L24:;
    if (id <= 169) goto L22; else goto L23;
  L25:;
#line 405
    sprY = ((((int32_t)(((uint32_t)(((uint32_t)(id))) & (uint32_t)(127)))) * 16) + 8);
    goto L18;
  L26:;
#line 407
    sprY = (id * 16);
    goto L18;
  L27:;
    if ((sprY + ht) <= 2000) goto L39; else goto L38;
  L28:;
#line 418
    ht = 8;
    goto L27;
  L29:;
    if (((uint32_t)(((uint32_t)(id))) & (uint32_t)(128)) != 0) goto L36; else goto L27;
  L30:;
    if (id > 16) goto L35; else goto L29;
  L31:;
    if (id == 26) goto L28; else goto L30;
  L32:;
    if (id == 25) goto L28; else goto L31;
  L33:;
    if (id >= 8) goto L34; else goto L32;
  L34:;
    if (id <= 12) goto L28; else goto L32;
  L35:;
    if (id < 24) goto L28; else goto L29;
  L36:;
    if (id < 156) goto L28; else goto L37;
  L37:;
    if (id > 169) goto L28; else goto L27;
  L38:;
    goto L13;
  L39:;
#line 421
    Platform_DrawTexRegion(WorldObj_objTex, 0, sprY, 16, ht, (sx - WorldObj_S(8)), (sy - WorldObj_S(m2_div(ht, 2))), WorldObj_S(16), WorldObj_S(ht));
    goto L38;
}

static int32_t WorldObj_CheckObjectPickup(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t i;
    int32_t dx;
    int32_t dy;
    int32_t id;
    i = 0;
    goto L1;
  L1:;
    if (i <= (WorldObj_objCount - 1)) goto L2; else goto L4;
  L2:;
    if (WorldObj_objects[i].status == 1) goto L7; else goto L8;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return (-1);
  L5:;
    goto L3;
  L6:;
#line 441
    dx = (heroX - WorldObj_objects[i].x);
#line 442
    dy = (heroY - WorldObj_objects[i].y);
    if (dx < 16) goto L15; else goto L11;
  L7:;
    if (WorldObj_objects[i].region == Assets_currentRegion) goto L6; else goto L10;
  L8:;
    if (WorldObj_revealHidden) goto L9; else goto L5;
  L9:;
    if (WorldObj_objects[i].status == 5) goto L7; else goto L5;
  L10:;
    if (WorldObj_objects[i].region == (-1)) goto L6; else goto L5;
  L11:;
    goto L5;
  L12:;
#line 444
    id = WorldObj_objects[i].objId;
#line 445
    WorldObj_objects[i].status = 2;
    return id;
  L13:;
    if (dy > (-16)) goto L12; else goto L11;
  L14:;
    if (dy < 16) goto L13; else goto L11;
  L15:;
    if (dx > (-16)) goto L14; else goto L11;
}

/* Imported Module NPC */

typedef struct NPC_SetfigDef NPC_SetfigDef;
static const int32_t NPC_DarkPriestRace = 15;
static const int32_t NPC_AppleRangerRace = 18;
static const int32_t NPC_MerchantWizardRace = 16;
static const int32_t NPC_ScrollPriestRace = 17;
static const int32_t NPC_PrayerSkeletonRace = 14;
static const int32_t NPC_MaxSpeeches = 69;
static const int32_t NPC_MaxNPCs = 19;
typedef struct NPC_SetfigDef NPC_SetfigDef;
struct NPC_SetfigDef {
    int32_t spriteBank;
    int32_t imageBase;
    int canTalk;
};

static void NPC_InitSetfigTable(void);
static void NPC_GetSetfigSprite(int32_t race, int32_t *bank, int32_t *frame);
static void NPC_InitSpeeches(void);
static void NPC_MaterializeNPCs(int32_t heroX, int32_t heroY, int32_t region);
static int32_t NPC_FindNearestNPC(int32_t heroX, int32_t heroY);
static void NPC_NpcName(int32_t race, char *name, uint32_t name_high);
static int NPC_LookAtNPC(int32_t heroX, int32_t heroY, char *desc, uint32_t desc_high);
static int32_t NPC_SelectSpeech(int32_t actorIdx);
static int NPC_TalkToNPC(int32_t heroX, int32_t heroY, char *speech, uint32_t speech_high);
static int NPC_GiveToNPC(int32_t heroX, int32_t heroY, int32_t itemIdx, char *response, uint32_t response_high);
static void NPC_GetSpeech(int32_t idx, char *text, uint32_t text_high);
static void NPC_ResetMaterialized(void);
static void NPC_InitNPCs(void);

NPC_SetfigDef NPC_sfTable[18 + 1];
int NPC_materialized[1599 + 1];
int NPC_priestStatueGiven;
int NPC_sorceressVisited;
int32_t NPC_darkChant;
int32_t NPC_rng;
char NPC_speeches[68 + 1][255 + 1];
static void NPC_InitSetfigTable(void) {
#line 41 "src/NPC.mod"
    NPC_sfTable[0].spriteBank = 0;
    NPC_sfTable[0].imageBase = 0;
    NPC_sfTable[0].canTalk = 1;
#line 42
    NPC_sfTable[1].spriteBank = 0;
    NPC_sfTable[1].imageBase = 4;
    NPC_sfTable[1].canTalk = 1;
#line 43
    NPC_sfTable[2].spriteBank = 1;
    NPC_sfTable[2].imageBase = 0;
    NPC_sfTable[2].canTalk = 0;
#line 44
    NPC_sfTable[3].spriteBank = 1;
    NPC_sfTable[3].imageBase = 1;
    NPC_sfTable[3].canTalk = 0;
#line 45
    NPC_sfTable[4].spriteBank = 1;
    NPC_sfTable[4].imageBase = 2;
    NPC_sfTable[4].canTalk = 0;
#line 46
    NPC_sfTable[5].spriteBank = 1;
    NPC_sfTable[5].imageBase = 4;
    NPC_sfTable[5].canTalk = 1;
#line 47
    NPC_sfTable[6].spriteBank = 1;
    NPC_sfTable[6].imageBase = 6;
    NPC_sfTable[6].canTalk = 0;
#line 48
    NPC_sfTable[7].spriteBank = 1;
    NPC_sfTable[7].imageBase = 7;
    NPC_sfTable[7].canTalk = 0;
#line 49
    NPC_sfTable[8].spriteBank = 2;
    NPC_sfTable[8].imageBase = 0;
    NPC_sfTable[8].canTalk = 0;
#line 50
    NPC_sfTable[9].spriteBank = 3;
    NPC_sfTable[9].imageBase = 0;
    NPC_sfTable[9].canTalk = 0;
#line 51
    NPC_sfTable[10].spriteBank = 3;
    NPC_sfTable[10].imageBase = 6;
    NPC_sfTable[10].canTalk = 0;
#line 52
    NPC_sfTable[11].spriteBank = 3;
    NPC_sfTable[11].imageBase = 7;
    NPC_sfTable[11].canTalk = 0;
#line 53
    NPC_sfTable[12].spriteBank = 4;
    NPC_sfTable[12].imageBase = 0;
    NPC_sfTable[12].canTalk = 1;
#line 54
    NPC_sfTable[13].spriteBank = 4;
    NPC_sfTable[13].imageBase = 4;
    NPC_sfTable[13].canTalk = 1;
#line 56
    NPC_sfTable[14].spriteBank = 0;
    NPC_sfTable[14].imageBase = 0;
    NPC_sfTable[14].canTalk = 1;
#line 58
    NPC_sfTable[15].spriteBank = 0;
    NPC_sfTable[15].imageBase = 4;
    NPC_sfTable[15].canTalk = 1;
#line 60
    NPC_sfTable[16].spriteBank = 0;
    NPC_sfTable[16].imageBase = 0;
    NPC_sfTable[16].canTalk = 1;
#line 61
    NPC_sfTable[17].spriteBank = 5;
    NPC_sfTable[17].imageBase = 0;
    NPC_sfTable[17].canTalk = 1;
#line 62
    NPC_sfTable[18].spriteBank = 4;
    NPC_sfTable[18].imageBase = 0;
    NPC_sfTable[18].canTalk = 1;
    return;
}

static void NPC_GetSetfigSprite(int32_t race, int32_t *bank, int32_t *frame) {
    (void)race;
    (void)bank;
    (void)frame;
    if (race >= 0) goto L4; else goto L3;
  L1:;
    return;
  L2:;
#line 68
    (*bank) = NPC_sfTable[race].spriteBank;
#line 69
    (*frame) = NPC_sfTable[race].imageBase;
    goto L1;
  L3:;
#line 71
    (*bank) = 0;
#line 72
    (*frame) = 0;
    goto L1;
  L4:;
    if (race < 19) goto L2; else goto L3;
}

static void NPC_InitSpeeches(void) {
#line 81
    m2_Strings_Assign("% attempted to communicate with the Ogre but a guttural snarl was the only response.", NPC_speeches[0], 255);
#line 82
    m2_Strings_Assign("\"Human must die!\" said the goblin-man.", NPC_speeches[1], 255);
#line 83
    m2_Strings_Assign("\"Doom!\" wailed the wraith.", NPC_speeches[2], 255);
#line 84
    m2_Strings_Assign("A clattering of bones was the only reply.", NPC_speeches[3], 255);
#line 85
    m2_Strings_Assign("% knew that it is a waste of time to talk to a snake.", NPC_speeches[4], 255);
#line 86
    m2_Strings_Assign("...", NPC_speeches[5], 255);
#line 87
    m2_Strings_Assign("There was no reply.", NPC_speeches[6], 255);
#line 88
    m2_Strings_Assign("\"Die, foolish mortal!\" he said.", NPC_speeches[7], 255);
#line 89
    m2_Strings_Assign("\"No need to shout, son!\" he said.", NPC_speeches[8], 255);
#line 90
    m2_Strings_Assign("Nice weather we're having, isn't it? queried the ranger.", NPC_speeches[9], 255);
#line 91
    m2_Strings_Assign("\"Good luck, sonny!\" said the ranger. \"Hope you win!\"", NPC_speeches[10], 255);
#line 92
    m2_Strings_Assign("\"If you need to cross the lake\" said the ranger, \"There is a raft just north of here.\"", NPC_speeches[11], 255);
#line 93
    m2_Strings_Assign("\"Would you like to buy something?\" said the tavern keeper. \"Or do you just need lodging for the night?\"", NPC_speeches[12], 255);
#line 94
    m2_Strings_Assign("\"Good Morning.\" said the tavern keeper. \"Hope you slept well.\"", NPC_speeches[13], 255);
#line 95
    m2_Strings_Assign("\"Have a drink!\" said the tavern keeper.", NPC_speeches[14], 255);
#line 96
    m2_Strings_Assign("\"State your business!\" said the guard. \"My business is with the king.\" stated %, respectfully.", NPC_speeches[15], 255);
#line 97
    m2_Strings_Assign("\"Please, sir, rescue me from this horrible prison!\" pleaded the princess.", NPC_speeches[16], 255);
#line 98
    m2_Strings_Assign("\"I cannot help you, young man.\" said the king. \"My armies are decimated, and I fear that with the loss of my children, I have lost all hope.\"", NPC_speeches[17], 255);
#line 99
    m2_Strings_Assign("\"Here is a writ designating you as my official agent. Be sure and show this to the Priest before you leave Marheim.\"", NPC_speeches[18], 255);
#line 100
    m2_Strings_Assign("\"I am afraid I cannot help you, young man. I already gave the golden statue to the other young man.\"", NPC_speeches[19], 255);
#line 101
    m2_Strings_Assign("If you could rescue the king's daughter, said Lord Trane, The King's courage would be restored.", NPC_speeches[20], 255);
#line 102
    m2_Strings_Assign("\"Sorry, I have no use for it.\"", NPC_speeches[21], 255);
#line 103
    m2_Strings_Assign("The dragon's cave is directly north of here. said the ranger.", NPC_speeches[22], 255);
#line 104
    m2_Strings_Assign("\"Alms! Alms for the poor!\"", NPC_speeches[23], 255);
#line 105
    m2_Strings_Assign("I have a prophecy for you, m'lord. said the beggar. You must seek two women, one Good, one Evil.", NPC_speeches[24], 255);
#line 106
    m2_Strings_Assign("\"Lovely Jewels, glint in the night - give to us the gift of Sight!\" he said.", NPC_speeches[25], 255);
#line 107
    m2_Strings_Assign("\"Where is the hidden city? How can you find it when you cannot even see it?\" said the beggar.", NPC_speeches[26], 255);
#line 108
    m2_Strings_Assign("\"Kind deeds could gain thee a friend from the sea.\"", NPC_speeches[27], 255);
#line 109
    m2_Strings_Assign("\"Seek the place that is darker than night - There you shall find your goal in sight!\" said the wizard, cryptically.", NPC_speeches[28], 255);
#line 110
    m2_Strings_Assign("\"Like the eye itself, a crystal Orb can help to find things concealed.\"", NPC_speeches[29], 255);
#line 111
    m2_Strings_Assign("\"The Witch lives in the dim forest of Grimwood, where the very trees are warped to her will. Her gaze is Death!\"", NPC_speeches[30], 255);
#line 112
    m2_Strings_Assign("Only the light of the Sun can destroy the Witch's Evil.", NPC_speeches[31], 255);
#line 113
    m2_Strings_Assign("\"The maiden you seek lies imprisoned in an unreachable castle surrounded by unclimbable mountains.\"", NPC_speeches[32], 255);
#line 114
    m2_Strings_Assign("\"Tame the golden beast and no mountain may deny you! But what rope could hold such a creature?\"", NPC_speeches[33], 255);
#line 115
    m2_Strings_Assign("\"Just what I needed!\" he said.", NPC_speeches[34], 255);
#line 116
    m2_Strings_Assign("\"Away with you, young ruffian!\" said the Wizard. \"Perhaps you can find some small animal to torment if that pleases you!\"", NPC_speeches[35], 255);
#line 117
    m2_Strings_Assign("\"You must seek your enemy on the spirit plane. It is hazardous in the extreme. Space may twist, and time itself may run backwards!\"", NPC_speeches[36], 255);
#line 118
    m2_Strings_Assign("\"When you wish to travel quickly, seek the power of the Stones.\" he said.", NPC_speeches[37], 255);
#line 119
    m2_Strings_Assign("\"Since you are brave of heart, I shall Heal all your wounds.\" Instantly % felt much better.", NPC_speeches[38], 255);
#line 120
    m2_Strings_Assign("Ah! You have a writ from the king. Here is one of the golden statues of Azal-Car-Ithil. Find all five and you'll find the vanishing city.", NPC_speeches[39], 255);
#line 121
    m2_Strings_Assign("\"Repent, Sinner! Thou art an uncouth brute and I have no interest in your conversation!\"", NPC_speeches[40], 255);
#line 122
    m2_Strings_Assign("\"Ho there, young traveler!\" said the black figure. \"None may enter the sacred shrine of the People who came Before!\"", NPC_speeches[41], 255);
#line 123
    m2_Strings_Assign("\"Your prowess in battle is great.\" said the Knight of Dreams. \"You have earned the right to enter and claim the prize.\"", NPC_speeches[42], 255);
#line 124
    m2_Strings_Assign("\"So this is the so-called Hero who has been sent to hinder my plans. Simply Pathetic. Well, try this, young Fool!\"", NPC_speeches[43], 255);
#line 125
    m2_Strings_Assign("% gasped. The Necromancer had been transformed into a normal man. All of his evil was gone.", NPC_speeches[44], 255);
#line 126
    m2_Strings_Assign("\"Welcome. Here is one of the five golden figurines you will need.\" \"Thank you.\" said %.", NPC_speeches[45], 255);
#line 127
    m2_Strings_Assign("\"Look into my eyes and Die!!\" hissed the witch. \"Not a chance!\" replied %", NPC_speeches[46], 255);
#line 128
    m2_Strings_Assign("The Spectre spoke. HE has usurped my place as lord of undead. Bring me bones of the ancient King and I'll help you destroy him.", NPC_speeches[47], 255);
#line 129
    m2_Strings_Assign("% gave him the ancient bones. \"Good! That spirit now rests quietly in my halls. Take this crystal shard.\"", NPC_speeches[48], 255);
#line 130
    m2_Strings_Assign("\"%...\" said the apparition. \"I am the ghost of your dead brother. Find my bones -- there you will find some things you need.", NPC_speeches[49], 255);
#line 131
    m2_Strings_Assign("% gave him some gold coins. \"Why, thank you, young sir!\"", NPC_speeches[50], 255);
#line 132
    m2_Strings_Assign("\"Sorry, but I have nothing to sell.\"", NPC_speeches[51], 255);
#line 133
    NPC_speeches[52][0] = '\0';
#line 134
    m2_Strings_Assign("The dragon's cave is east of here. said the ranger.", NPC_speeches[53], 255);
#line 135
    m2_Strings_Assign("The dragon's cave is west of here. said the ranger.", NPC_speeches[54], 255);
#line 136
    m2_Strings_Assign("The dragon's cave is south of here. said the ranger.", NPC_speeches[55], 255);
#line 137
    m2_Strings_Assign("\"Oh, thank you for saving my eggs, kind man!\" said the turtle. \"Take this seashell as a token of my gratitude.\"", NPC_speeches[56], 255);
#line 138
    m2_Strings_Assign("\"Just hop on my back if you need a ride somewhere.\" said the turtle.", NPC_speeches[57], 255);
#line 139
    m2_Strings_Assign("Stupid fool, you can't hurt me with that!", NPC_speeches[58], 255);
#line 140
    m2_Strings_Assign("Your magic won't work here, fool!", NPC_speeches[59], 255);
#line 141
    m2_Strings_Assign("The Sunstone has made the witch vulnerable!", NPC_speeches[60], 255);
#line 142
    m2_Strings_Assign("\"Ohm Ohm!\"", NPC_speeches[61], 255);
#line 143
    m2_Strings_Assign("\"Umbra, bind the wandering soul.\"", NPC_speeches[62], 255);
#line 144
    m2_Strings_Assign("\"Blood of night, awaken beneath the stones.\"", NPC_speeches[63], 255);
#line 145
    m2_Strings_Assign("\"Let the hollow stars drink the fading light.\"", NPC_speeches[64], 255);
#line 146
    m2_Strings_Assign("\"By bone and shadow, the sealed gate stirs.\"", NPC_speeches[65], 255);
#line 147
    m2_Strings_Assign("\"Roots that dream, leaves that whisper, blood that remembers. My little garden has answers for those carrying gold.\"", NPC_speeches[66], 255);
#line 148
    m2_Strings_Assign("\"Ink and parchment preserve powers that the cautious may purchase.\"", NPC_speeches[67], 255);
#line 149
    m2_Strings_Assign("\"Fresh apples for the road, traveler.\"", NPC_speeches[68], 255);
    return;
}

static void NPC_MaterializeNPCs(int32_t heroX, int32_t heroY, int32_t region) {
    (void)heroX;
    (void)heroY;
    (void)region;
    int32_t i;
    int32_t dx;
    int32_t dy;
    int32_t idx;
    int32_t race;
    int32_t seq;
#line 157
    seq = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= (WorldObj_objCount - 1)) goto L2; else goto L4;
  L2:;
    if (WorldObj_objects[i].status == 3) goto L7; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
#line 161
    dx = (heroX - WorldObj_objects[i].x);
#line 162
    dy = (heroY - WorldObj_objects[i].y);
    if (dx < 0) goto L10; else goto L9;
  L7:;
    if (WorldObj_objects[i].region == region) goto L6; else goto L8;
  L8:;
    if (WorldObj_objects[i].region == (-1)) goto L6; else goto L5;
  L9:;
    if (dy < 0) goto L12; else goto L11;
  L10:;
#line 163
    dx = (-dx);
    goto L9;
  L11:;
    if (dx < 400) goto L15; else goto L13;
  L12:;
#line 164
    dy = (-dy);
    goto L11;
  L13:;
#line 192
    (seq++);
    goto L5;
  L14:;
    if (NPC_materialized[i]) goto L16; else goto L17;
  L15:;
    if (dy < 400) goto L14; else goto L13;
  L16:;
    goto L13;
  L17:;
    if (Actor_actorCount < 48) goto L19; else goto L18;
  L18:;
    goto L16;
  L19:;
#line 170
    idx = Actor_actorCount;
#line 171
    race = WorldObj_objects[i].objId;
    if (race >= 19) goto L21; else goto L20;
  L20:;
#line 173
    Actor_actors[idx].absX = WorldObj_objects[i].x;
#line 174
    Actor_actors[idx].absY = WorldObj_objects[i].y;
#line 175
    Actor_actors[idx].actorType = 4;
#line 176
    Actor_actors[idx].race = race;
#line 177
    Actor_actors[idx].state = 13;
#line 178
    Actor_actors[idx].goal = seq;
#line 179
    Actor_actors[idx].vitality = ((2 + race) + race);
#line 180
    Actor_actors[idx].weapon = 0;
#line 181
    Actor_actors[idx].facing = 4;
#line 182
    Actor_actors[idx].visible = 1;
#line 183
    Actor_actors[idx].environ = 0;
#line 184
    Actor_actors[idx].tactic = 0;
#line 185
    Actor_actors[idx].velX = 0;
#line 186
    Actor_actors[idx].velY = 0;
#line 187
    (Actor_actorCount++);
#line 188
    NPC_materialized[i] = 1;
    goto L18;
  L21:;
#line 172
    race = 0;
    goto L20;
}

static int32_t NPC_FindNearestNPC(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t i;
    int32_t dx;
    int32_t dy;
    int32_t bestDist;
    int32_t dist;
    int32_t bestIdx;
#line 202
    bestDist = 9999;
#line 203
    bestIdx = (-1);
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Actor_actors[i].actorType == 4) goto L7; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return bestIdx;
  L5:;
    goto L3;
  L6:;
#line 207
    dx = (heroX - Actor_actors[i].absX);
#line 208
    dy = (heroY - Actor_actors[i].absY);
    if (dx < 0) goto L9; else goto L8;
  L7:;
    if (Actor_actors[i].state != 15) goto L6; else goto L5;
  L8:;
    if (dy < 0) goto L11; else goto L10;
  L9:;
#line 209
    dx = (-dx);
    goto L8;
  L10:;
#line 211
    dist = (dx + dy);
    if (dx < 40) goto L15; else goto L12;
  L11:;
#line 210
    dy = (-dy);
    goto L10;
  L12:;
    goto L5;
  L13:;
#line 213
    bestDist = dist;
#line 214
    bestIdx = i;
    if ((heroX - Actor_actors[i].absX) > 5) goto L17; else goto L18;
  L14:;
    if (dist < bestDist) goto L13; else goto L12;
  L15:;
    if (dy < 40) goto L14; else goto L12;
  L16:;
    goto L12;
  L17:;
#line 216
    Actor_actors[i].facing = 2;
    goto L16;
  L18:;
    if ((heroX - Actor_actors[i].absX) < (-5)) goto L19; else goto L20;
  L19:;
#line 217
    Actor_actors[i].facing = 6;
    goto L16;
  L20:;
    if ((heroY - Actor_actors[i].absY) > 5) goto L21; else goto L22;
  L21:;
#line 218
    Actor_actors[i].facing = 4;
    goto L16;
  L22:;
    if ((heroY - Actor_actors[i].absY) < (-5)) goto L23; else goto L16;
  L23:;
#line 219
    Actor_actors[i].facing = 0;
    goto L16;
}

static void NPC_NpcName(int32_t race, char *name, uint32_t name_high) {
    (void)race;
    (void)name;
    (void)name_high;
    if ((race == 0)) goto L3;
    if ((race == 1)) goto L4;
    if ((race == 2) || (race == 3)) goto L5;
    if ((race == 4)) goto L6;
    if ((race == 5)) goto L7;
    if ((race == 6)) goto L8;
    if ((race == 7)) goto L9;
    if ((race == 8)) goto L10;
    if ((race == 9)) goto L11;
    if ((race == 10)) goto L12;
    if ((race == 11)) goto L13;
    if ((race == 12)) goto L14;
    if ((race == 13)) goto L15;
    if ((race == 14)) goto L16;
    if ((race == 15)) goto L17;
    if ((race == 16)) goto L18;
    if ((race == 17)) goto L19;
    if ((race == 18)) goto L20;
    goto L2;
  L1:;
    return;
  L2:;
#line 249
    m2_Strings_Assign("someone", name, name_high);
    goto L1;
  L3:;
#line 230
    m2_Strings_Assign("a wizard", name, name_high);
    goto L1;
  L4:;
#line 231
    m2_Strings_Assign("a priest", name, name_high);
    goto L1;
  L5:;
#line 232
    m2_Strings_Assign("a guard", name, name_high);
    goto L1;
  L6:;
#line 233
    m2_Strings_Assign("the princess", name, name_high);
    goto L1;
  L7:;
#line 234
    m2_Strings_Assign("the king", name, name_high);
    goto L1;
  L8:;
#line 235
    m2_Strings_Assign("a noble", name, name_high);
    goto L1;
  L9:;
#line 236
    m2_Strings_Assign("the sorceress", name, name_high);
    goto L1;
  L10:;
#line 237
    m2_Strings_Assign("the tavern keeper", name, name_high);
    goto L1;
  L11:;
#line 238
    m2_Strings_Assign("the witch", name, name_high);
    goto L1;
  L12:;
#line 239
    m2_Strings_Assign("a spectre", name, name_high);
    goto L1;
  L13:;
#line 240
    m2_Strings_Assign("a ghost", name, name_high);
    goto L1;
  L14:;
#line 241
    m2_Strings_Assign("a ranger", name, name_high);
    goto L1;
  L15:;
#line 242
    m2_Strings_Assign("a beggar", name, name_high);
    goto L1;
  L16:;
#line 243
    m2_Strings_Assign("a praying skeleton", name, name_high);
    goto L1;
  L17:;
#line 244
    m2_Strings_Assign("a dark priest", name, name_high);
    goto L1;
  L18:;
#line 245
    m2_Strings_Assign("a mysterious herb wizard", name, name_high);
    goto L1;
  L19:;
#line 246
    m2_Strings_Assign("a scroll priest", name, name_high);
    goto L1;
  L20:;
#line 247
    m2_Strings_Assign("an apple ranger", name, name_high);
    goto L1;
}

static int NPC_LookAtNPC(int32_t heroX, int32_t heroY, char *desc, uint32_t desc_high) {
    (void)heroX;
    (void)heroY;
    (void)desc;
    (void)desc_high;
    int32_t idx;
#line 256
    idx = NPC_FindNearestNPC(heroX, heroY);
    if (idx >= 0) goto L2; else goto L1;
  L1:;
    return 0;
  L2:;
#line 258
    NPC_NpcName(Actor_actors[idx].race, desc, desc_high);
    return 1;
}

static int32_t NPC_SelectSpeech(int32_t actorIdx) {
    (void)actorIdx;
    int32_t race;
    int32_t goal;
    int32_t kind;
    int32_t i;
#line 269
    race = Actor_actors[actorIdx].race;
#line 270
    goal = Actor_actors[actorIdx].goal;
#line 271
    kind = Brothers_brothers[Brothers_activeBrother].kind;
    if ((race == 0)) goto L3;
    if ((race == 1)) goto L4;
    if ((race == 2) || (race == 3)) goto L5;
    if ((race == 4)) goto L6;
    if ((race == 5)) goto L7;
    if ((race == 6)) goto L8;
    if ((race == 7)) goto L9;
    if ((race == 8)) goto L10;
    if ((race == 9)) goto L11;
    if ((race == 10)) goto L12;
    if ((race == 11)) goto L13;
    if ((race == 12)) goto L14;
    if ((race == 13)) goto L15;
    if ((race == 14)) goto L16;
    if ((race == 15)) goto L17;
    if ((race == 16)) goto L18;
    if ((race == 17)) goto L19;
    if ((race == 18)) goto L20;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 49;
  L3:;
    if (kind < 10) goto L22; else goto L23;
  L4:;
    if (Brothers_HasStuff(28)) goto L27; else goto L26;
  L5:;
    return 15;
  L6:;
    return 16;
  L7:;
    return 17;
  L8:;
    return 20;
  L9:;
    if (NPC_sorceressVisited) goto L34; else goto L33;
  L10:;
    return 12;
  L11:;
    return 46;
  L12:;
    return 47;
  L13:;
    return 49;
  L14:;
    if (Assets_currentRegion == 2) goto L48; else goto L49;
  L15:;
    return 23;
  L16:;
    return 61;
  L17:;
#line 326
    i = (62 + m2_mod(NPC_darkChant, 4));
#line 327
    (NPC_darkChant++);
    return i;
  L18:;
    return 66;
  L19:;
    return 67;
  L20:;
    return 68;
  L21:;
    return 0;
  L22:;
    return 35;
  L23:;
    return (27 + m2_mod(goal, 7));
  L24:;
    return 0;
  L25:;
#line 280
    NPC_priestStatueGiven = 1;
#line 281
    Brothers_GiveStuff(25);
    return 39;
  L26:;
    if (NPC_priestStatueGiven) goto L28; else goto L29;
  L27:;
    if (NPC_priestStatueGiven) goto L26; else goto L25;
  L28:;
    return 37;
  L29:;
    if (kind < 10) goto L30; else goto L31;
  L30:;
    return 40;
  L31:;
#line 288
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
    return (36 + m2_mod(goal, 3));
  L32:;
    return 0;
  L33:;
#line 298
    NPC_sorceressVisited = 1;
    i = 0;
    goto L35;
  L34:;
#line 309
    NPC_rng = ((NPC_rng * 1103515245) + 12345);
    if (NPC_rng < 0) goto L44; else goto L43;
  L35:;
    if (i <= (WorldObj_objCount - 1)) goto L36; else goto L38;
  L36:;
    if (WorldObj_objects[i].x == 12025) goto L42; else goto L39;
  L37:;
    i = (i + 1);
    goto L35;
  L38:;
    return 45;
  L39:;
    goto L37;
  L40:;
#line 304
    WorldObj_objects[i].status = 1;
    goto L39;
  L41:;
    if (WorldObj_objects[i].objId == 149) goto L40; else goto L39;
  L42:;
    if (WorldObj_objects[i].y == 37639) goto L41; else goto L39;
  L43:;
    if (Brothers_brothers[Brothers_activeBrother].luck < m2_mod(m2_div(NPC_rng, 65536), 64)) goto L46; else goto L45;
  L44:;
#line 310
    NPC_rng = (-NPC_rng);
    goto L43;
  L45:;
    return (-1);
  L46:;
#line 312
    (Brothers_brothers[Brothers_activeBrother].luck += 5);
    goto L45;
  L47:;
    return 0;
  L48:;
    return 22;
  L49:;
    return (53 + m2_mod(goal, 3));
}

static int NPC_TalkToNPC(int32_t heroX, int32_t heroY, char *speech, uint32_t speech_high) {
    (void)heroX;
    (void)heroY;
    (void)speech;
    (void)speech_high;
    int32_t idx;
    int32_t race;
    int32_t speechIdx;
#line 340
    idx = NPC_FindNearestNPC(heroX, heroY);
    if (idx < 0) goto L2; else goto L1;
  L1:;
#line 342
    speechIdx = NPC_SelectSpeech(idx);
    if (speechIdx < 0) goto L4; else goto L5;
  L2:;
    return 0;
  L3:;
    return 1;
  L4:;
#line 344
    speech[0] = '\0';
    goto L3;
  L5:;
    if (speechIdx < 69) goto L6; else goto L7;
  L6:;
#line 346
    m2_Strings_Assign(NPC_speeches[speechIdx], speech, speech_high);
    goto L3;
  L7:;
#line 348
    m2_Strings_Assign("...", speech, speech_high);
    goto L3;
}

static int NPC_GiveToNPC(int32_t heroX, int32_t heroY, int32_t itemIdx, char *response, uint32_t response_high) {
    (void)heroX;
    (void)heroY;
    (void)itemIdx;
    (void)response;
    (void)response_high;
    int32_t idx;
    int32_t race;
#line 357
    idx = NPC_FindNearestNPC(heroX, heroY);
    if (idx < 0) goto L2; else goto L1;
  L1:;
#line 359
    race = Actor_actors[idx].race;
    if (itemIdx == 0) goto L4; else goto L3;
  L2:;
    return 0;
  L3:;
    if (itemIdx == 3) goto L12; else goto L10;
  L4:;
    if (Brothers_brothers[Brothers_activeBrother].wealth <= 2) goto L6; else goto L5;
  L5:;
#line 367
    Brothers_AddWealth((-2));
#line 368
    Brothers_IncKind();
    if (race == 13) goto L8; else goto L9;
  L6:;
#line 364
    m2_Strings_Assign("Not enough gold.", response, response_high);
    return 1;
  L7:;
    return 1;
  L8:;
#line 370
    NPC_GetSpeech((24 + m2_mod(Actor_actors[idx].goal, 4)), response, response_high);
    goto L7;
  L9:;
#line 372
    NPC_GetSpeech(49, response, response_high);
    goto L7;
  L10:;
#line 390
    m2_Strings_Assign("", response, response_high);
    return 1;
  L11:;
    if (race == 10) goto L14; else goto L15;
  L12:;
    if (Brothers_HasStuff(29)) goto L11; else goto L10;
  L13:;
    return 1;
  L14:;
#line 380
    Brothers_SetStuff(29, 0);
#line 381
    WorldObj_AddObj(Actor_actors[idx].absX, Actor_actors[idx].absY, 140, 1, (-1));
#line 382
    NPC_GetSpeech(48, response, response_high);
    goto L13;
  L15:;
#line 384
    NPC_GetSpeech(21, response, response_high);
    goto L13;
}

static void NPC_GetSpeech(int32_t idx, char *text, uint32_t text_high) {
    (void)idx;
    (void)text;
    (void)text_high;
    if (idx >= 0) goto L4; else goto L3;
  L1:;
    return;
  L2:;
#line 397
    m2_Strings_Assign(NPC_speeches[idx], text, text_high);
    goto L1;
  L3:;
#line 399
    m2_Strings_Assign("...", text, text_high);
    goto L1;
  L4:;
    if (idx < 69) goto L2; else goto L3;
}

static void NPC_ResetMaterialized(void) {
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (1600 - 1)) goto L2; else goto L4;
  L2:;
#line 406
    NPC_materialized[i] = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

static void NPC_InitNPCs(void) {
    int32_t i;
#line 412
    NPC_ResetMaterialized();
#line 413
    NPC_priestStatueGiven = 0;
#line 414
    NPC_sorceressVisited = 0;
#line 415
    NPC_darkChant = 0;
#line 416
    NPC_rng = 99991;
#line 417
    NPC_InitSetfigTable();
#line 418
    NPC_InitSpeeches();
    return;
}

/* Imported Module Carrier */

static const int32_t Carrier_RideSwan = 11;
static const int32_t Carrier_RideNone = 0;
static const int32_t Carrier_RideTurtle = 5;
static const int32_t Carrier_RideRaft = 1;
static const int32_t Carrier_RaftX = 13668;
static const int32_t Carrier_RaftY = 14470;
static const int32_t Carrier_RaftSlot = 1;
static const int32_t Carrier_CarrierSlot = 3;
static const int32_t Carrier_StLasso = 5;
static const int32_t Carrier_StShell = 6;
static int32_t Carrier_Abs(int32_t x);
static int Carrier_TalkToCarrier(char *speech, uint32_t speech_high);
static void Carrier_InitCarriers(void);
static void Carrier_CheckProximity(int32_t slot);
static void Carrier_UpdateRaft(void);
static void Carrier_UpdateTurtleCarrier(void);
static int Carrier_IsFireyDeath(void);
static void Carrier_UpdateSwanCarrier(void);
static void Carrier_PlaceTurtle(int32_t tx, int32_t ty);
static void Carrier_SpawnTurtle(void);
static void Carrier_CheckSwanExtent(void);
static void Carrier_UpdateCarriers(void);
static void Carrier_SpawnDragon(void);
static void Carrier_UpdateDragon(void);

int32_t Carrier_dismountResult;
int32_t Carrier_riding;
int32_t Carrier_activeCarrier;
int32_t Carrier_swanCooldown;
int Carrier_dragonFire;
int Carrier_turtleEggsDone;
int Carrier_swanDismount;
int Carrier_turtleEggs;
int32_t Carrier_raftProx;
int32_t Carrier_turtleTick;
int Carrier_dragonSpawned;
int32_t Carrier_dragonRng;
static int32_t Carrier_Abs(int32_t x) {
    (void)x;
    if (x < 0) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return (-x);
  L3:;
    return x;
}

static int Carrier_TalkToCarrier(char *speech, uint32_t speech_high) {
    (void)speech;
    (void)speech_high;
    int32_t dx;
    int32_t dy;
    if (Carrier_activeCarrier != 5) goto L2; else goto L1;
  L1:;
#line 40 "src/Carrier.mod"
    dx = Carrier_Abs((Actor_actors[0].absX - Actor_actors[3].absX));
#line 41
    dy = Carrier_Abs((Actor_actors[0].absY - Actor_actors[3].absY));
    if (dx > 40) goto L4; else goto L5;
  L2:;
    return 0;
  L3:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[6] > 0) goto L7; else goto L8;
  L4:;
    return 0;
  L5:;
    if (dy > 40) goto L4; else goto L3;
  L6:;
    return 1;
  L7:;
#line 45
    NPC_GetSpeech(57, speech, speech_high);
    goto L6;
  L8:;
#line 47
    Brothers_brothers[Brothers_activeBrother].stuff[6] = 1;
#line 48
    NPC_GetSpeech(56, speech, speech_high);
    goto L6;
}

static void Carrier_InitCarriers(void) {
#line 55
    Carrier_riding = 0;
#line 56
    Carrier_activeCarrier = 0;
#line 57
    Carrier_raftProx = 0;
#line 58
    Carrier_turtleTick = 0;
#line 59
    Carrier_turtleEggs = 0;
#line 60
    Carrier_turtleEggsDone = 0;
#line 63
    Actor_actors[1].absX = 13668;
#line 64
    Actor_actors[1].absY = 14470;
#line 65
    Actor_actors[1].actorType = 3;
#line 66
    Actor_actors[1].state = 13;
#line 67
    Actor_actors[1].vitality = 999;
#line 68
    Actor_actors[1].weapon = 0;
#line 69
    Actor_actors[1].environ = 0;
#line 70
    Actor_actors[1].visible = 1;
#line 71
    Actor_actors[1].race = 0;
    if (Actor_actorCount < 2) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 72
    Actor_actorCount = 2;
    goto L1;
}

static void Carrier_CheckProximity(int32_t slot) {
    (void)slot;
    int32_t dx;
    int32_t dy;
#line 79
    dx = (Actor_actors[0].absX - Actor_actors[slot].absX);
#line 80
    dy = (Actor_actors[0].absY - Actor_actors[slot].absY);
    if (Carrier_Abs(dx) < 9) goto L4; else goto L3;
  L1:;
    return;
  L2:;
#line 82
    Carrier_raftProx = 2;
    goto L1;
  L3:;
    if (Carrier_Abs(dx) < 16) goto L7; else goto L6;
  L4:;
    if (Carrier_Abs(dy) < 9) goto L2; else goto L3;
  L5:;
#line 84
    Carrier_raftProx = 1;
    goto L1;
  L6:;
#line 86
    Carrier_raftProx = 0;
    goto L1;
  L7:;
    if (Carrier_Abs(dy) < 16) goto L5; else goto L6;
}

static void Carrier_UpdateRaft(void) {
    int32_t terrain;
    if (Carrier_riding == 1) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 96
    Actor_actors[1].absX = Actor_actors[0].absX;
#line 97
    Actor_actors[1].absY = Actor_actors[0].absY;
#line 100
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY);
    if (terrain != 4) goto L7; else goto L4;
  L3:;
#line 106
    Carrier_CheckProximity(1);
    if (Carrier_raftProx == 2) goto L9; else goto L8;
  L4:;
    goto L1;
  L5:;
#line 102
    Carrier_riding = 0;
    goto L4;
  L6:;
    if (terrain != 3) goto L5; else goto L4;
  L7:;
    if (terrain != 5) goto L6; else goto L4;
  L8:;
    goto L1;
  L9:;
#line 108
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY);
    if (terrain == 4) goto L11; else goto L13;
  L10:;
    goto L8;
  L11:;
#line 110
    Carrier_riding = 1;
#line 111
    Actor_actors[1].absX = Actor_actors[0].absX;
#line 112
    Actor_actors[1].absY = Actor_actors[0].absY;
    goto L10;
  L12:;
    if (terrain == 3) goto L11; else goto L10;
  L13:;
    if (terrain == 5) goto L11; else goto L12;
}

static void Carrier_UpdateTurtleCarrier(void) {
    int32_t terrain;
    if (Carrier_activeCarrier != 5) goto L2; else goto L1;
  L1:;
    if (Carrier_riding == 5) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    return;
  L4:;
#line 126
    Actor_actors[3].absX = Actor_actors[0].absX;
#line 127
    Actor_actors[3].absY = Actor_actors[0].absY;
#line 128
    Actor_actors[3].facing = Actor_actors[0].facing;
#line 131
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY);
    if (terrain != 4) goto L9; else goto L6;
  L5:;
#line 138
    Carrier_CheckProximity(3);
    if (Carrier_raftProx == 2) goto L11; else goto L12;
  L6:;
    goto L3;
  L7:;
#line 133
    Carrier_riding = 0;
    goto L6;
  L8:;
    if (terrain != 3) goto L7; else goto L6;
  L9:;
    if (terrain != 5) goto L8; else goto L6;
  L10:;
    goto L3;
  L11:;
#line 140
    Carrier_riding = 5;
#line 141
    Actor_actors[3].absX = Actor_actors[0].absX;
#line 142
    Actor_actors[3].absY = Actor_actors[0].absY;
    goto L10;
  L12:;
    if (Carrier_raftProx == 0) goto L13; else goto L10;
  L13:;
    if (Actor_actors[0].absX > (Actor_actors[3].absX + 3)) goto L15; else goto L16;
  L14:;
    if (Actor_actors[0].absY > (Actor_actors[3].absY + 3)) goto L23; else goto L24;
  L15:;
    if (Assets_GetTerrainAt((Actor_actors[3].absX + 2), Actor_actors[3].absY) >= 3) goto L18; else goto L17;
  L16:;
    if (Actor_actors[0].absX < (Actor_actors[3].absX - 3)) goto L19; else goto L14;
  L17:;
    goto L14;
  L18:;
#line 148
    (Actor_actors[3].absX += 2);
#line 149
    Actor_actors[3].facing = 2;
    goto L17;
  L19:;
    if (Assets_GetTerrainAt((Actor_actors[3].absX - 2), Actor_actors[3].absY) >= 3) goto L21; else goto L20;
  L20:;
    goto L14;
  L21:;
#line 153
    (Actor_actors[3].absX -= 2);
#line 154
    Actor_actors[3].facing = 6;
    goto L20;
  L22:;
#line 168
    Actor_actors[3].state = 12;
    goto L10;
  L23:;
    if (Assets_GetTerrainAt(Actor_actors[3].absX, (Actor_actors[3].absY + 2)) >= 3) goto L26; else goto L25;
  L24:;
    if (Actor_actors[0].absY < (Actor_actors[3].absY - 3)) goto L27; else goto L22;
  L25:;
    goto L22;
  L26:;
#line 159
    (Actor_actors[3].absY += 2);
#line 160
    Actor_actors[3].facing = 4;
    goto L25;
  L27:;
    if (Assets_GetTerrainAt(Actor_actors[3].absX, (Actor_actors[3].absY - 2)) >= 3) goto L29; else goto L28;
  L28:;
    goto L22;
  L29:;
#line 164
    (Actor_actors[3].absY -= 2);
#line 165
    Actor_actors[3].facing = 0;
    goto L28;
}

static int Carrier_IsFireyDeath(void) {
    int32_t px;
    int32_t py;
#line 181
    px = (Actor_actors[0].absX - 144);
#line 182
    py = (Actor_actors[0].absY - 90);
    return (((px > 8802 && px < 13562) && py > 24744) && py < 29544);
}

static void Carrier_UpdateSwanCarrier(void) {
    int32_t terrain;
    int32_t yt;
    if (Carrier_activeCarrier != 11) goto L2; else goto L1;
  L1:;
    if (Carrier_riding == 11) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    return;
  L4:;
#line 194
    Actor_actors[3].absX = Actor_actors[0].absX;
#line 195
    Actor_actors[3].absY = Actor_actors[0].absY;
#line 196
    Actor_actors[3].facing = Actor_actors[0].facing;
#line 199
    Actor_actors[0].environ = (-2);
    if (Carrier_swanDismount) goto L7; else goto L6;
  L5:;
    if (Carrier_swanCooldown > 0) goto L33; else goto L34;
  L6:;
    goto L3;
  L7:;
#line 205
    Carrier_swanDismount = 0;
#line 206
    Carrier_dismountResult = 0;
    if (Carrier_IsFireyDeath()) goto L9; else goto L10;
  L8:;
    goto L6;
  L9:;
#line 208
    Carrier_dismountResult = 2;
    goto L8;
  L10:;
    if (Carrier_Abs(Actor_actors[0].velX) >= 15) goto L11; else goto L13;
  L11:;
#line 210
    Carrier_dismountResult = 1;
    goto L8;
  L12:;
#line 214
    yt = (Actor_actors[0].absY - 14);
#line 215
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, yt);
    if (terrain == 1) goto L15; else goto L17;
  L13:;
    if (Carrier_Abs(Actor_actors[0].velY) >= 15) goto L11; else goto L12;
  L14:;
    if (yt >= 0) goto L25; else goto L24;
  L15:;
#line 219
    yt = Actor_actors[0].absY;
#line 220
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, yt);
    if (terrain == 1) goto L20; else goto L22;
  L16:;
    if (terrain >= 10) goto L15; else goto L14;
  L17:;
    if (terrain >= 4) goto L18; else goto L16;
  L18:;
    if (terrain <= 5) goto L15; else goto L16;
  L19:;
    goto L14;
  L20:;
#line 223
    Carrier_dismountResult = 3;
#line 224
    yt = (-1);
    goto L19;
  L21:;
    if (terrain >= 10) goto L20; else goto L19;
  L22:;
    if (terrain >= 4) goto L23; else goto L21;
  L23:;
    if (terrain <= 5) goto L20; else goto L21;
  L24:;
    goto L8;
  L25:;
#line 228
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, (yt + 10));
    if (terrain == 1) goto L27; else goto L30;
  L26:;
    goto L24;
  L27:;
#line 231
    Carrier_dismountResult = 3;
    goto L26;
  L28:;
#line 233
    Carrier_riding = 0;
#line 234
    Actor_actors[0].absY = yt;
#line 235
    Actor_actors[0].environ = 0;
#line 236
    Actor_actors[0].velX = 0;
#line 237
    Actor_actors[0].velY = 0;
#line 238
    Actor_actors[0].state = 13;
#line 239
    Actor_actors[3].state = 13;
#line 240
    Carrier_swanCooldown = 30;
    goto L26;
  L29:;
    if (terrain >= 10) goto L27; else goto L28;
  L30:;
    if (terrain >= 4) goto L31; else goto L29;
  L31:;
    if (terrain <= 5) goto L27; else goto L29;
  L32:;
    goto L3;
  L33:;
#line 248
    Carrier_CheckProximity(3);
    if (Carrier_raftProx == 0) goto L36; else goto L35;
  L34:;
    if (Brothers_HasStuff(5)) goto L37; else goto L32;
  L35:;
    goto L32;
  L36:;
#line 250
    Carrier_swanCooldown = 0;
    goto L35;
  L37:;
#line 253
    Carrier_CheckProximity(3);
    if (Carrier_raftProx >= 1) goto L39; else goto L38;
  L38:;
    goto L32;
  L39:;
#line 255
    Carrier_riding = 11;
#line 256
    Actor_actors[3].absX = Actor_actors[0].absX;
#line 257
    Actor_actors[3].absY = Actor_actors[0].absY;
#line 258
    Actor_actors[0].velX = 0;
#line 259
    Actor_actors[0].velY = 0;
    goto L38;
}

static void Carrier_PlaceTurtle(int32_t tx, int32_t ty) {
    (void)tx;
    (void)ty;
#line 267
    Actor_actors[3].absX = tx;
#line 268
    Actor_actors[3].absY = ty;
#line 269
    Actor_actors[3].actorType = 5;
#line 270
    Actor_actors[3].state = 13;
#line 271
    Actor_actors[3].vitality = 50;
#line 272
    Actor_actors[3].weapon = 0;
#line 273
    Actor_actors[3].environ = 0;
#line 274
    Actor_actors[3].visible = 1;
#line 275
    Actor_actors[3].race = 5;
#line 276
    Carrier_activeCarrier = 5;
    if (Actor_actorCount < 4) goto L2; else goto L1;
  L1:;
#line 278
    m2_WriteString("Carrier: turtle spawned at ");
#line 279
    m2_WriteInt(tx, 1);
    m2_WriteString(",");
    m2_WriteInt(ty, 1);
    m2_WriteLn();
    return;
  L2:;
#line 277
    Actor_actorCount = 4;
    goto L1;
}

static void Carrier_SpawnTurtle(void) {
    int32_t i;
    int32_t dir;
    int32_t dist;
    int32_t tx;
    int32_t ty;
    int32_t terrain;
    int32_t rng;
    int32_t xdir[7 + 1];
    int32_t ydir[7 + 1];
#line 289
    xdir[0] = (-2);
    xdir[1] = 0;
    xdir[2] = 2;
    xdir[3] = 3;
#line 290
    xdir[4] = 2;
    xdir[5] = 0;
    xdir[6] = (-2);
    xdir[7] = (-3);
#line 291
    ydir[0] = (-2);
    ydir[1] = (-3);
    ydir[2] = (-2);
    ydir[3] = 0;
#line 292
    ydir[4] = 2;
    ydir[5] = 3;
    ydir[6] = 2;
    ydir[7] = 0;
#line 293
    rng = (((Actor_actors[0].absX * 31) + Actor_actors[0].absY) + Carrier_dragonRng);
    i = 0;
    goto L1;
  L1:;
    if (i <= 24) goto L2; else goto L4;
  L2:;
#line 295
    rng = ((rng * 1103515245) + 12345);
    if (rng < 0) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 309
    m2_WriteString("Carrier: no deep water found for turtle");
    m2_WriteLn();
    return;
  L5:;
#line 297
    dir = m2_mod(m2_div(rng, 65536), 8);
#line 298
    rng = ((rng * 1103515245) + 12345);
    if (rng < 0) goto L8; else goto L7;
  L6:;
#line 296
    rng = (-rng);
    goto L5;
  L7:;
#line 300
    dist = (150 + m2_mod(m2_div(rng, 65536), 64));
#line 301
    tx = (Actor_actors[0].absX + m2_div((xdir[dir] * dist), 2));
#line 302
    ty = (Actor_actors[0].absY + m2_div((ydir[dir] * dist), 2));
#line 303
    terrain = Assets_GetTerrainAt(tx, ty);
    if (terrain == 5) goto L10; else goto L9;
  L8:;
#line 299
    rng = (-rng);
    goto L7;
  L9:;
    goto L3;
  L10:;
#line 305
    Carrier_PlaceTurtle(tx, ty);
    return;
}

static void Carrier_CheckSwanExtent(void) {
    if (Carrier_activeCarrier == 11) goto L2; else goto L1;
  L1:;
    if (Brothers_HasStuff(5)) goto L3; else goto L4;
  L2:;
    return;
  L3:;
    if (Actor_actors[0].absX > 2118) goto L9; else goto L5;
  L4:;
    return;
  L5:;
    return;
  L6:;
#line 319
    Actor_actors[3].absX = 2368;
#line 320
    Actor_actors[3].absY = 27437;
#line 321
    Actor_actors[3].actorType = 5;
#line 322
    Actor_actors[3].state = 13;
#line 323
    Actor_actors[3].vitality = 50;
#line 324
    Actor_actors[3].weapon = 0;
#line 325
    Actor_actors[3].environ = 0;
#line 326
    Actor_actors[3].visible = 1;
#line 327
    Actor_actors[3].race = 11;
#line 328
    Actor_actors[3].facing = 4;
#line 329
    Carrier_activeCarrier = 11;
    if (Actor_actorCount < 4) goto L11; else goto L10;
  L7:;
    if (Actor_actors[0].absY < 27637) goto L6; else goto L5;
  L8:;
    if (Actor_actors[0].absY > 27237) goto L7; else goto L5;
  L9:;
    if (Actor_actors[0].absX < 2618) goto L8; else goto L5;
  L10:;
#line 331
    m2_WriteString("Carrier: swan spawned");
    m2_WriteLn();
    goto L5;
  L11:;
#line 330
    Actor_actorCount = 4;
    goto L10;
}

static void Carrier_UpdateCarriers(void) {
    if (Actor_actors[0].state >= 14) goto L2; else goto L1;
  L1:;
#line 339
    Carrier_UpdateRaft();
#line 340
    Carrier_CheckSwanExtent();
    if (Carrier_activeCarrier == 5) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    if (Carrier_riding != 0) goto L20; else goto L18;
  L4:;
#line 341
    Carrier_UpdateTurtleCarrier();
    goto L3;
  L5:;
    if (Carrier_activeCarrier == 11) goto L6; else goto L7;
  L6:;
#line 342
    Carrier_UpdateSwanCarrier();
    goto L3;
  L7:;
    if (Carrier_riding == 11) goto L8; else goto L10;
  L8:;
#line 349
    Carrier_activeCarrier = 11;
#line 350
    Carrier_UpdateSwanCarrier();
    goto L3;
  L9:;
    if (Actor_actorCount > 3) goto L17; else goto L3;
  L10:;
    if (Actor_actorCount > 3) goto L13; else goto L9;
  L11:;
    if (Actor_actors[3].vitality > 0) goto L8; else goto L9;
  L12:;
    if (Actor_actors[3].race == 11) goto L11; else goto L9;
  L13:;
    if (Actor_actors[3].actorType == 5) goto L12; else goto L9;
  L14:;
#line 356
    Carrier_activeCarrier = 5;
#line 357
    Carrier_UpdateTurtleCarrier();
    goto L3;
  L15:;
    if (Actor_actors[3].vitality > 0) goto L14; else goto L3;
  L16:;
    if (Actor_actors[3].race == 5) goto L15; else goto L3;
  L17:;
    if (Actor_actors[3].actorType == 5) goto L16; else goto L3;
  L18:;
    return;
  L19:;
#line 362
    Actor_actors[0].environ = 0;
    goto L18;
  L20:;
    if (Carrier_riding != 11) goto L19; else goto L18;
}

static void Carrier_SpawnDragon(void) {
    if (Carrier_dragonSpawned) goto L2; else goto L1;
  L1:;
#line 376
    Actor_actors[3].absX = 6999;
#line 377
    Actor_actors[3].absY = 35151;
#line 378
    Actor_actors[3].actorType = 6;
#line 379
    Actor_actors[3].state = 13;
#line 380
    Actor_actors[3].vitality = 50;
#line 381
    Actor_actors[3].weapon = 5;
#line 382
    Actor_actors[3].environ = 0;
#line 383
    Actor_actors[3].visible = 1;
#line 384
    Actor_actors[3].race = 10;
#line 385
    Actor_actors[3].facing = 5;
    if (Actor_actorCount < 4) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 387
    Carrier_dragonSpawned = 1;
#line 388
    m2_WriteString("Dragon spawned");
    m2_WriteLn();
    return;
  L4:;
#line 386
    Actor_actorCount = 4;
    goto L3;
}

static void Carrier_UpdateDragon(void) {
    int32_t dx;
    int32_t dy;
    if (Carrier_dragonSpawned) goto L1; else goto L2;
  L1:;
    if (Actor_actors[3].actorType != 6) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (Actor_actors[3].state == 15) goto L6; else goto L5;
  L4:;
    return;
  L5:;
    if (Actor_actors[3].state == 14) goto L8; else goto L7;
  L6:;
    return;
  L7:;
    if (Actor_actors[3].vitality < 1) goto L12; else goto L11;
  L8:;
#line 398
    (Actor_actors[3].tactic--);
    if (Actor_actors[3].tactic <= 0) goto L10; else goto L9;
  L9:;
    return;
  L10:;
#line 400
    Actor_actors[3].state = 15;
    goto L9;
  L11:;
#line 411
    dx = (Actor_actors[0].absX - Actor_actors[3].absX);
#line 412
    dy = (Actor_actors[0].absY - Actor_actors[3].absY);
    if (Carrier_Abs(dx) > Carrier_Abs(dy)) goto L14; else goto L15;
  L12:;
#line 405
    Actor_actors[3].state = 14;
#line 406
    Actor_actors[3].tactic = 30;
    return;
  L13:;
#line 423
    Carrier_dragonFire = 0;
#line 424
    Carrier_dragonRng = ((Carrier_dragonRng * 1103515245) + 12345);
    if (Carrier_dragonRng < 0) goto L23; else goto L22;
  L14:;
    if (dx > 0) goto L17; else goto L18;
  L15:;
    if (dy > 0) goto L20; else goto L21;
  L16:;
    goto L13;
  L17:;
#line 414
    Actor_actors[3].facing = 2;
    goto L16;
  L18:;
#line 415
    Actor_actors[3].facing = 6;
    goto L16;
  L19:;
    goto L13;
  L20:;
#line 417
    Actor_actors[3].facing = 4;
    goto L19;
  L21:;
#line 418
    Actor_actors[3].facing = 0;
    goto L19;
  L22:;
    if (m2_mod(m2_div(Carrier_dragonRng, 65536), 4) == 0) goto L25; else goto L24;
  L23:;
#line 425
    Carrier_dragonRng = (-Carrier_dragonRng);
    goto L22;
  L24:;
    return;
  L25:;
    if (Carrier_Abs(dx) < 200) goto L28; else goto L26;
  L26:;
    goto L24;
  L27:;
#line 428
    Carrier_dragonFire = 1;
    goto L26;
  L28:;
    if (Carrier_Abs(dy) < 200) goto L27; else goto L26;
}

static void Carrier_init(void) {
#line 434
    Carrier_dragonSpawned = 0;
#line 435
    Carrier_dragonFire = 0;
#line 436
    Carrier_swanDismount = 0;
#line 437
    Carrier_swanCooldown = 0;
#line 438
    Carrier_dismountResult = 0;
#line 439
    Carrier_dragonRng = 31337;
    return;
}

/* Imported Module Movement */

static void Movement_InitDirTables(void);
static int32_t Movement_NewX(int32_t x, int32_t dir, int32_t speed);
static int32_t Movement_NewY(int32_t y, int32_t dir, int32_t speed);
static int32_t Movement_ProxCheck(int32_t x, int32_t y, int32_t actorIdx);
static int32_t Movement_EnvironToSpeed(int32_t k);
static int32_t Movement_UpdateEnviron(int32_t terrCode, int32_t curEnv);
static int Movement_MoveActor(int32_t actorIdx, int32_t dir, int32_t speed);

int32_t Movement_xDir[9 + 1];
int32_t Movement_yDir[9 + 1];
static void Movement_InitDirTables(void) {
#line 22 "src/Movement.mod"
    Movement_xDir[0] = 0;
    Movement_yDir[0] = (-2);
#line 23
    Movement_xDir[1] = 2;
    Movement_yDir[1] = (-2);
#line 24
    Movement_xDir[2] = 2;
    Movement_yDir[2] = 0;
#line 25
    Movement_xDir[3] = 2;
    Movement_yDir[3] = 2;
#line 26
    Movement_xDir[4] = 0;
    Movement_yDir[4] = 2;
#line 27
    Movement_xDir[5] = (-2);
    Movement_yDir[5] = 2;
#line 28
    Movement_xDir[6] = (-2);
    Movement_yDir[6] = 0;
#line 29
    Movement_xDir[7] = (-2);
    Movement_yDir[7] = (-2);
#line 30
    Movement_xDir[8] = 0;
    Movement_yDir[8] = 0;
#line 31
    Movement_xDir[9] = 0;
    Movement_yDir[9] = 0;
    return;
}

static int32_t Movement_NewX(int32_t x, int32_t dir, int32_t speed) {
    (void)x;
    (void)dir;
    (void)speed;
    if (dir < 8) goto L2; else goto L1;
  L1:;
    return x;
  L2:;
    return (x + m2_div((Movement_xDir[dir] * speed), 2));
}

static int32_t Movement_NewY(int32_t y, int32_t dir, int32_t speed) {
    (void)y;
    (void)dir;
    (void)speed;
    if (dir < 8) goto L2; else goto L1;
  L1:;
    return y;
  L2:;
    return (y + m2_div((Movement_yDir[dir] * speed), 2));
}

static int32_t Movement_ProxCheck(int32_t x, int32_t y, int32_t actorIdx) {
    (void)x;
    (void)y;
    (void)actorIdx;
    int32_t terrain;
    int32_t t;
    int32_t j;
    int32_t dx;
    int32_t dy;
    if (actorIdx == 0) goto L3; else goto L1;
  L1:;
    if (Actor_actors[actorIdx].actorType == 2) goto L6; else goto L5;
  L2:;
    return 0;
  L3:;
    if (Carrier_riding == 11) goto L2; else goto L1;
  L4:;
    j = 0;
    goto L77;
  L5:;
    if (Assets_currentRegion >= 0) goto L8; else goto L9;
  L6:;
    if (Actor_actors[actorIdx].race == 2) goto L4; else goto L5;
  L7:;
    goto L4;
  L8:;
#line 62
    t = Assets_GetTerrainAt((x + 4), (y + 2));
    if (t == 1) goto L11; else goto L10;
  L9:;
#line 127
    terrain = World_GetTerrain(x, y);
    if (World_IsPassable(terrain)) goto L75; else goto L76;
  L10:;
    if (t >= 4) goto L16; else goto L12;
  L11:;
    return t;
  L12:;
    if (t >= 10) goto L18; else goto L17;
  L13:;
    return t;
  L14:;
    if (Actor_actors[actorIdx].race != 4) goto L13; else goto L12;
  L15:;
    if (actorIdx != 0) goto L14; else goto L12;
  L16:;
    if (t <= 5) goto L15; else goto L12;
  L17:;
#line 79
    t = Assets_GetTerrainAt((x - 4), (y + 2));
    if (t == 1) goto L28; else goto L27;
  L18:;
    if (actorIdx == 0) goto L22; else goto L21;
  L19:;
    goto L17;
  L20:;
#line 71
    Doors_OpenDoorTile(x, y);
    return 15;
  L21:;
    if (actorIdx == 0) goto L26; else goto L24;
  L22:;
    if (t == 15) goto L20; else goto L21;
  L23:;
    goto L19;
  L24:;
    return t;
  L25:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[30] > 0) goto L23; else goto L24;
  L26:;
    if (t == 12) goto L25; else goto L24;
  L27:;
    if (t >= 4) goto L33; else goto L29;
  L28:;
    return t;
  L29:;
    if (t >= 8) goto L35; else goto L34;
  L30:;
    return t;
  L31:;
    if (Actor_actors[actorIdx].race != 4) goto L30; else goto L29;
  L32:;
    if (actorIdx != 0) goto L31; else goto L29;
  L33:;
    if (t <= 5) goto L32; else goto L29;
  L34:;
#line 96
    t = Assets_GetTerrainAt(x, y);
    if (t == 1) goto L49; else goto L48;
  L35:;
    if (actorIdx == 0) goto L39; else goto L38;
  L36:;
    goto L34;
  L37:;
    goto L36;
  L38:;
    if (actorIdx == 0) goto L44; else goto L42;
  L39:;
    if (t == 8) goto L37; else goto L40;
  L40:;
    if (t == 9) goto L37; else goto L38;
  L41:;
    goto L36;
  L42:;
    if (actorIdx == 0) goto L47; else goto L46;
  L43:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[30] > 0) goto L41; else goto L42;
  L44:;
    if (t == 12) goto L43; else goto L42;
  L45:;
#line 90
    Doors_OpenDoorTile(x, y);
    return 15;
  L46:;
    return t;
  L47:;
    if (t == 15) goto L45; else goto L46;
  L48:;
    if (t >= 4) goto L54; else goto L50;
  L49:;
    return t;
  L50:;
    if (t >= 10) goto L56; else goto L55;
  L51:;
    return t;
  L52:;
    if (Actor_actors[actorIdx].race != 4) goto L51; else goto L50;
  L53:;
    if (actorIdx != 0) goto L52; else goto L50;
  L54:;
    if (t <= 5) goto L53; else goto L50;
  L55:;
    terrain = (-8);
    goto L65;
  L56:;
    if (actorIdx == 0) goto L60; else goto L59;
  L57:;
    goto L55;
  L58:;
#line 102
    Doors_OpenDoorTile(x, y);
    return 15;
  L59:;
    if (actorIdx == 0) goto L64; else goto L62;
  L60:;
    if (t == 15) goto L58; else goto L59;
  L61:;
    goto L57;
  L62:;
    return t;
  L63:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[30] > 0) goto L61; else goto L62;
  L64:;
    if (t == 12) goto L63; else goto L62;
  L65:;
    if (terrain <= 8) goto L66; else goto L68;
  L66:;
#line 114
    t = Assets_GetTerrainAt((x + terrain), (y - 4));
    if (t == 15) goto L71; else goto L69;
  L67:;
    terrain = (terrain + 4);
    goto L65;
  L68:;
    goto L7;
  L69:;
#line 119
    t = Assets_GetTerrainAt((x + terrain), (y + 8));
    if (t == 15) goto L74; else goto L72;
  L70:;
#line 116
    Doors_OpenDoorTile((x + terrain), (y - 4));
    return 15;
  L71:;
    if (actorIdx == 0) goto L70; else goto L69;
  L72:;
    goto L67;
  L73:;
#line 121
    Doors_OpenDoorTile((x + terrain), (y + 8));
    return 15;
  L74:;
    if (actorIdx == 0) goto L73; else goto L72;
  L75:;
    goto L7;
  L76:;
    return terrain;
  L77:;
    if (j <= (Actor_actorCount - 1)) goto L78; else goto L80;
  L78:;
    if (j != actorIdx) goto L84; else goto L81;
  L79:;
    j = (j + 1);
    goto L77;
  L80:;
    return 0;
  L81:;
    goto L79;
  L82:;
    if (Actor_actors[j].state == 14) goto L88; else goto L87;
  L83:;
    if (Actor_actors[j].actorType != 5) goto L82; else goto L81;
  L84:;
    if (Actor_actors[j].state != 15) goto L83; else goto L81;
  L85:;
    goto L81;
  L86:;
    goto L85;
  L87:;
#line 146
    dx = (x - Actor_actors[j].absX);
#line 147
    dy = (y - Actor_actors[j].absY);
    if (dx < 11) goto L93; else goto L89;
  L88:;
    if (Actor_actors[j].tactic < 15) goto L86; else goto L87;
  L89:;
    goto L85;
  L90:;
    return 16;
  L91:;
    if (dy > (-9)) goto L90; else goto L89;
  L92:;
    if (dy < 9) goto L91; else goto L89;
  L93:;
    if (dx > (-11)) goto L92; else goto L89;
}

static int32_t Movement_EnvironToSpeed(int32_t k) {
    (void)k;
    if (k == (-1)) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return 4;
  L3:;
    if (k == (-2)) goto L4; else goto L5;
  L4:;
    return 1;
  L5:;
    if (k == (-3)) goto L6; else goto L7;
  L6:;
    return 2;
  L7:;
    if (k == 2) goto L8; else goto L10;
  L8:;
    return 1;
  L9:;
    return 2;
  L10:;
    if (k > 6) goto L8; else goto L9;
}

static int32_t Movement_UpdateEnviron(int32_t terrCode, int32_t curEnv) {
    (void)terrCode;
    (void)curEnv;
    int32_t target;
    if ((terrCode == 0)) goto L3;
    if ((terrCode == 2)) goto L4;
    if ((terrCode == 3)) goto L5;
    if ((terrCode == 6)) goto L6;
    if ((terrCode == 7)) goto L7;
    if ((terrCode == 8)) goto L8;
    if ((terrCode == 9)) goto L9;
    if ((terrCode == 10)) goto L10;
    if ((terrCode == 15)) goto L11;
    if ((terrCode == 4)) goto L12;
    if ((terrCode == 5)) goto L13;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    if (curEnv > 0) goto L15; else goto L16;
  L4:;
    return 2;
  L5:;
    if (curEnv > 5) goto L20; else goto L21;
  L6:;
    return (-1);
  L7:;
    return (-2);
  L8:;
    return (-3);
  L9:;
    return (-2);
  L10:;
    return 0;
  L11:;
    return 0;
  L12:;
#line 199
    target = 10;
    if (curEnv > target) goto L25; else goto L26;
  L13:;
#line 206
    target = 30;
    if (curEnv > target) goto L30; else goto L31;
  L14:;
    return 0;
  L15:;
    return (curEnv - 1);
  L16:;
    if (curEnv < 0) goto L17; else goto L18;
  L17:;
    return 0;
  L18:;
    return 0;
  L19:;
    return 0;
  L20:;
    return (curEnv - 1);
  L21:;
    if (curEnv < 5) goto L22; else goto L23;
  L22:;
    return (curEnv + 1);
  L23:;
    return 5;
  L24:;
    return 0;
  L25:;
    return (curEnv - 1);
  L26:;
    if (curEnv < target) goto L27; else goto L28;
  L27:;
    return (curEnv + 1);
  L28:;
    return curEnv;
  L29:;
    return 0;
  L30:;
    return (curEnv - 1);
  L31:;
    if (curEnv < target) goto L32; else goto L33;
  L32:;
    return (curEnv + 1);
  L33:;
    return curEnv;
}

static int Movement_MoveActor(int32_t actorIdx, int32_t dir, int32_t speed) {
    (void)actorIdx;
    (void)dir;
    (void)speed;
    int32_t xTest;
    int32_t yTest;
    int32_t effSpeed;
    int32_t terrCode;
    int32_t env;
    if (Assets_currentRegion >= 0) goto L2; else goto L3;
  L1:;
#line 226
    xTest = Movement_NewX(Actor_actors[actorIdx].absX, dir, effSpeed);
#line 227
    yTest = Movement_NewY(Actor_actors[actorIdx].absY, dir, effSpeed);
    if (xTest < 0) goto L5; else goto L4;
  L2:;
#line 221
    effSpeed = Movement_EnvironToSpeed(Actor_actors[actorIdx].environ);
    goto L1;
  L3:;
#line 223
    effSpeed = speed;
    goto L1;
  L4:;
    if (yTest < 0) goto L7; else goto L6;
  L5:;
    return 0;
  L6:;
    if (xTest > 32767) goto L9; else goto L8;
  L7:;
    return 0;
  L8:;
    if (yTest > 40959) goto L11; else goto L10;
  L9:;
    return 0;
  L10:;
    if (Movement_ProxCheck(xTest, yTest, actorIdx) != 0) goto L13; else goto L12;
  L11:;
    return 0;
  L12:;
#line 238
    Actor_actors[actorIdx].absX = xTest;
#line 239
    Actor_actors[actorIdx].absY = yTest;
    if (Assets_currentRegion >= 0) goto L16; else goto L14;
  L13:;
    return 0;
  L14:;
    return 1;
  L15:;
#line 244
    terrCode = Assets_GetTerrainAt(Actor_actors[actorIdx].absX, Actor_actors[actorIdx].absY);
#line 245
    Actor_actors[actorIdx].environ = Movement_UpdateEnviron(terrCode, Actor_actors[actorIdx].environ);
    goto L14;
  L16:;
    if (actorIdx == 0) goto L17; else goto L15;
  L17:;
    if (Carrier_riding == 11) goto L14; else goto L15;
}

static void Movement_init(void) {
#line 251
    Movement_InitDirTables();
    return;
}

/* Imported Module Combat */

static int32_t Combat_Rand(int32_t limit);
static void Combat_DoHit(int32_t attacker, int32_t defender);
static void Combat_Dist(int32_t a, int32_t b, int32_t *xd, int32_t *yd);
static int Combat_IsFacing(int32_t attacker, int32_t target);
static void Combat_UpdateCombat(void);

int32_t Combat_wardTimer;
int32_t Combat_hitCooldown[47 + 1];
int32_t Combat_rng;
static int32_t Combat_Rand(int32_t limit) {
    (void)limit;
#line 21 "src/Combat.mod"
    Combat_rng = ((Combat_rng * 1103515245) + 12345);
    if (Combat_rng < 0) goto L2; else goto L1;
  L1:;
    if (limit <= 0) goto L4; else goto L3;
  L2:;
#line 22
    Combat_rng = (-Combat_rng);
    goto L1;
  L3:;
    return m2_mod(m2_div(Combat_rng, 65536), limit);
  L4:;
    return 0;
}

static void Combat_DoHit(int32_t attacker, int32_t defender) {
    (void)attacker;
    (void)defender;
    int32_t damage;
    int32_t wt;
    int kb;
    if (Actor_actors[defender].actorType == 2) goto L4; else goto L1;
  L1:;
#line 44
    wt = Actor_actors[attacker].weapon;
    if (wt >= 8) goto L6; else goto L5;
  L2:;
    return;
  L3:;
    if (Actor_actors[attacker].weapon < 4) goto L2; else goto L1;
  L4:;
    if (Actor_actors[defender].race == 9) goto L3; else goto L1;
  L5:;
#line 46
    damage = (wt + Combat_Rand(2));
    if (damage < 1) goto L8; else goto L7;
  L6:;
#line 45
    wt = 5;
    goto L5;
  L7:;
    if (defender == 0) goto L11; else goto L9;
  L8:;
#line 47
    damage = 1;
    goto L7;
  L9:;
#line 52
    (Actor_actors[defender].vitality -= damage);
    if (defender == 0) goto L13; else goto L14;
  L10:;
#line 49
    damage = m2_div((damage + 1), 2);
    goto L9;
  L11:;
    if (Combat_wardTimer > 0) goto L10; else goto L9;
  L12:;
    if (Actor_actors[defender].actorType != 4) goto L17; else goto L15;
  L13:;
#line 55
    SFX_PlayEffect(0);
    goto L12;
  L14:;
#line 57
    SFX_PlayEffect(3);
    goto L12;
  L15:;
    if (Actor_actors[defender].vitality <= 0) goto L22; else goto L21;
  L16:;
#line 64
    kb = Movement_MoveActor(defender, Actor_actors[attacker].facing, 2);
    if (kb) goto L20; else goto L18;
  L17:;
    if (Actor_actors[defender].actorType != 6) goto L16; else goto L15;
  L18:;
    goto L15;
  L19:;
#line 66
    kb = Movement_MoveActor(attacker, Actor_actors[attacker].facing, 2);
    goto L18;
  L20:;
    if (attacker >= 0) goto L19; else goto L18;
  L21:;
    return;
  L22:;
#line 72
    Actor_actors[defender].vitality = 0;
#line 73
    Actor_actors[defender].state = 14;
#line 74
    Actor_actors[defender].goal = 7;
#line 75
    Actor_actors[defender].tactic = 7;
    if (defender > 0) goto L24; else goto L25;
  L23:;
    if (Actor_actors[defender].actorType == 4) goto L27; else goto L26;
  L24:;
#line 79
    Brothers_IncBrave();
    goto L23;
  L25:;
#line 81
    Brothers_DecLuck(5);
    goto L23;
  L26:;
    goto L21;
  L27:;
#line 85
    Brothers_DecKind(3);
    goto L26;
}

static void Combat_Dist(int32_t a, int32_t b, int32_t *xd, int32_t *yd) {
    (void)a;
    (void)b;
    (void)xd;
    (void)yd;
#line 94
    (*xd) = (Actor_actors[a].absX - Actor_actors[b].absX);
#line 95
    (*yd) = (Actor_actors[a].absY - Actor_actors[b].absY);
    if ((*xd) < 0) goto L2; else goto L1;
  L1:;
    if ((*yd) < 0) goto L4; else goto L3;
  L2:;
#line 96
    (*xd) = (-(*xd));
    goto L1;
  L3:;
    return;
  L4:;
#line 97
    (*yd) = (-(*yd));
    goto L3;
}

static int Combat_IsFacing(int32_t attacker, int32_t target) {
    (void)attacker;
    (void)target;
    int32_t dx;
    int32_t dy;
    int32_t f;
#line 106
    dx = (Actor_actors[target].absX - Actor_actors[attacker].absX);
#line 107
    dy = (Actor_actors[target].absY - Actor_actors[attacker].absY);
#line 108
    f = Actor_actors[attacker].facing;
    if ((f == 0)) goto L3;
    if ((f == 1)) goto L4;
    if ((f == 2)) goto L5;
    if ((f == 3)) goto L6;
    if ((f == 4)) goto L7;
    if ((f == 5)) goto L8;
    if ((f == 6)) goto L9;
    if ((f == 7)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 1;
  L3:;
    return dy <= 0;
  L4:;
    return (dx >= 0 || dy <= 0);
  L5:;
    return dx >= 0;
  L6:;
    return (dx >= 0 || dy >= 0);
  L7:;
    return dy >= 0;
  L8:;
    return (dx <= 0 || dy >= 0);
  L9:;
    return dx <= 0;
  L10:;
    return (dx <= 0 || dy <= 0);
}

static void Combat_UpdateCombat(void) {
    int32_t i;
    int32_t xd;
    int32_t yd;
    int32_t bv;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Combat_hitCooldown[i] > 0) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    if (Actor_actors[0].state != 15) goto L10; else goto L7;
  L5:;
    goto L3;
  L6:;
#line 130
    (Combat_hitCooldown[i]--);
    goto L5;
  L7:;
    if (Actor_actors[0].state == 0) goto L27; else goto L25;
  L8:;
    i = 1;
    goto L11;
  L9:;
    if (Actor_actors[0].environ >= 0) goto L8; else goto L7;
  L10:;
    if (Actor_actors[0].state != 14) goto L9; else goto L7;
  L11:;
    if (i <= (Actor_actorCount - 1)) goto L12; else goto L14;
  L12:;
    if (Actor_actors[i].state == 0) goto L17; else goto L15;
  L13:;
    i = (i + 1);
    goto L11;
  L14:;
    goto L7;
  L15:;
    goto L13;
  L16:;
#line 138
    Combat_Dist(i, 0, &xd, &yd);
#line 140
    bv = Brothers_brothers[Brothers_activeBrother].brave;
    if (xd < 14) goto L22; else goto L20;
  L17:;
    if (Combat_hitCooldown[i] == 0) goto L16; else goto L15;
  L18:;
    goto L15;
  L19:;
#line 142
    Combat_DoHit(i, 0);
#line 143
    Combat_hitCooldown[i] = 12;
    goto L18;
  L20:;
    if (xd < 14) goto L24; else goto L18;
  L21:;
    if (Combat_Rand(256) > bv) goto L19; else goto L20;
  L22:;
    if (yd < 14) goto L21; else goto L20;
  L23:;
#line 145
    Combat_hitCooldown[i] = 8;
    goto L18;
  L24:;
    if (yd < 14) goto L23; else goto L18;
  L25:;
    return;
  L26:;
    i = 1;
    goto L28;
  L27:;
    if (Combat_hitCooldown[0] == 0) goto L26; else goto L25;
  L28:;
    if (i <= (Actor_actorCount - 1)) goto L29; else goto L31;
  L29:;
    if (Actor_actors[i].state != 15) goto L34; else goto L32;
  L30:;
    i = (i + 1);
    goto L28;
  L31:;
    goto L25;
  L32:;
    goto L30;
  L33:;
#line 155
    Combat_Dist(0, i, &xd, &yd);
    if (xd < 20) goto L38; else goto L35;
  L34:;
    if (Actor_actors[i].state != 14) goto L33; else goto L32;
  L35:;
    goto L32;
  L36:;
#line 157
    Combat_DoHit(0, i);
#line 158
    Combat_hitCooldown[0] = 8;
#line 159
    i = Actor_actorCount;
    goto L35;
  L37:;
    if (Combat_IsFacing(0, i)) goto L36; else goto L35;
  L38:;
    if (yd < 20) goto L37; else goto L35;
}

static void Combat_init(void) {
#line 167
    Combat_wardTimer = 0;
#line 168
    Combat_rng = 77777;
    Combat_rng = 0;
    goto L1;
  L1:;
    if (Combat_rng <= 47) goto L2; else goto L4;
  L2:;
#line 169
    Combat_hitCooldown[Combat_rng] = 0;
    goto L3;
  L3:;
    Combat_rng = (Combat_rng + 1);
    goto L1;
  L4:;
#line 170
    Combat_rng = 77777;
    return;
}

/* Imported Module DayNight */

static const int32_t DayNight_DayNightMax = 24000;
static void DayNight_InitDayNight(void);
static void DayNight_UpdateLightLevel(void);
static void DayNight_UpdateDayNight(void);
static void DayNight_GetFadeRGB(int32_t *r, int32_t *g, int32_t *b);
static int DayNight_PaletteTickDue(void);
static int DayNight_MusicTickDue(void);

int32_t DayNight_lightlevel;
int32_t DayNight_brightness;
int32_t DayNight_daynight;
int DayNight_isNight;
int32_t DayNight_tickAccum;
static void DayNight_InitDayNight(void) {
#line 13 "src/DayNight.mod"
    DayNight_daynight = 12000;
#line 14
    DayNight_tickAccum = 0;
#line 15
    DayNight_UpdateLightLevel();
    return;
}

static void DayNight_UpdateLightLevel(void) {
#line 20
    DayNight_lightlevel = m2_div(DayNight_daynight, 40);
    if (DayNight_lightlevel >= 300) goto L2; else goto L1;
  L1:;
#line 24
    DayNight_isNight = DayNight_lightlevel <= 120;
#line 26
    DayNight_brightness = m2_div((DayNight_lightlevel * 100), 300);
    if (DayNight_brightness > 100) goto L4; else goto L3;
  L2:;
#line 22
    DayNight_lightlevel = (600 - DayNight_lightlevel);
    goto L1;
  L3:;
    return;
  L4:;
#line 27
    DayNight_brightness = 100;
    goto L3;
}

static void DayNight_UpdateDayNight(void) {
#line 34
    (DayNight_tickAccum += 5);
    goto L1;
  L1:;
    if (DayNight_tickAccum >= 6) goto L2; else goto L3;
  L2:;
#line 36
    (DayNight_tickAccum -= 6);
#line 37
    (DayNight_daynight++);
    if (DayNight_daynight >= 24000) goto L5; else goto L4;
  L3:;
#line 40
    DayNight_UpdateLightLevel();
    return;
  L4:;
    goto L1;
  L5:;
#line 38
    DayNight_daynight = 0;
    goto L4;
}

static void DayNight_GetFadeRGB(int32_t *r, int32_t *g, int32_t *b) {
    (void)r;
    (void)g;
    (void)b;
#line 49
    (*r) = (DayNight_lightlevel - 80);
#line 50
    (*g) = (DayNight_lightlevel - 61);
#line 51
    (*b) = (DayNight_lightlevel - 62);
    if ((*r) < 10) goto L2; else goto L1;
  L1:;
    if ((*g) < 25) goto L4; else goto L3;
  L2:;
#line 53
    (*r) = 10;
    goto L1;
  L3:;
    if ((*b) < 60) goto L6; else goto L5;
  L4:;
#line 54
    (*g) = 25;
    goto L3;
  L5:;
    if ((*r) > 100) goto L8; else goto L7;
  L6:;
#line 55
    (*b) = 60;
    goto L5;
  L7:;
    if ((*g) > 100) goto L10; else goto L9;
  L8:;
#line 56
    (*r) = 100;
    goto L7;
  L9:;
    if ((*b) > 100) goto L12; else goto L11;
  L10:;
#line 57
    (*g) = 100;
    goto L9;
  L11:;
    return;
  L12:;
#line 58
    (*b) = 100;
    goto L11;
}

static int DayNight_PaletteTickDue(void) {
    return ((uint32_t)(((uint32_t)(DayNight_daynight))) & (uint32_t)(3)) == 0;
}

static int DayNight_MusicTickDue(void) {
    return ((uint32_t)(((uint32_t)(DayNight_daynight))) & (uint32_t)(7)) == 0;
}

/* Imported Module Missile */

typedef struct Missile_MissileRec Missile_MissileRec;
static const int32_t Missile_MaxMissiles = 6;
typedef struct Missile_MissileRec Missile_MissileRec;
struct Missile_MissileRec {
    int32_t absX;
    int32_t absY;
    int32_t mtype;
    int32_t timeOfFlight;
    int32_t speed;
    int32_t direction;
    int32_t archer;
};

static const int32_t Missile_MaxFlight = 40;
static const int32_t Missile_ArrowSpeed = 3;
static const int32_t Missile_WandSpeed = 5;
static const int32_t Missile_ArrowHitR = 6;
static const int32_t Missile_WandHitR = 9;
static const int32_t Missile_ObjSprH = 16;
static const int32_t Missile_ObjSprW = 16;
static void Missile_InitMissiles(void);
static void Missile_FireMissile(int32_t actorIdx);
static void Missile_UpdateMissiles(void);
static int32_t Missile_S(int32_t v);
static void Missile_DrawMissiles(void);

int32_t Missile_nextSlot;
Missile_MissileRec Missile_missiles[5 + 1];
int32_t Missile_bowShotX[7 + 1];
int32_t Missile_bowShotY[7 + 1];
static void Missile_InitMissiles(void) {
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (6 - 1)) goto L2; else goto L4;
  L2:;
#line 36 "src/Missile.mod"
    Missile_missiles[i].mtype = 0;
#line 37
    Missile_missiles[i].speed = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 39
    Missile_nextSlot = 0;
#line 41
    Missile_bowShotX[0] = 0;
    Missile_bowShotY[0] = (-6);
#line 42
    Missile_bowShotX[1] = 0;
    Missile_bowShotY[1] = (-6);
#line 43
    Missile_bowShotX[2] = 3;
    Missile_bowShotY[2] = (-1);
#line 44
    Missile_bowShotX[3] = 6;
    Missile_bowShotY[3] = 0;
#line 45
    Missile_bowShotX[4] = (-3);
    Missile_bowShotY[4] = 6;
#line 46
    Missile_bowShotX[5] = (-3);
    Missile_bowShotY[5] = 8;
#line 47
    Missile_bowShotX[6] = (-3);
    Missile_bowShotY[6] = 0;
#line 48
    Missile_bowShotX[7] = (-6);
    Missile_bowShotY[7] = (-1);
    return;
}

static void Missile_FireMissile(int32_t actorIdx) {
    (void)actorIdx;
    int32_t weapon;
    int32_t d;
#line 54
    weapon = Actor_actors[actorIdx].weapon;
    if (weapon < 4) goto L2; else goto L3;
  L1:;
#line 57
    d = Actor_actors[actorIdx].facing;
    if (d > 7) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (weapon > 5) goto L2; else goto L1;
  L4:;
#line 60
    Missile_missiles[Missile_nextSlot].absX = (Actor_actors[actorIdx].absX + Missile_bowShotX[d]);
#line 61
    Missile_missiles[Missile_nextSlot].absY = (Actor_actors[actorIdx].absY + Missile_bowShotY[d]);
#line 62
    Missile_missiles[Missile_nextSlot].direction = d;
#line 63
    Missile_missiles[Missile_nextSlot].archer = actorIdx;
#line 64
    Missile_missiles[Missile_nextSlot].timeOfFlight = 0;
    if (weapon == 4) goto L7; else goto L8;
  L5:;
#line 58
    d = 0;
    goto L4;
  L6:;
#line 76
    Missile_nextSlot = m2_mod((Missile_nextSlot + 1), 6);
    return;
  L7:;
#line 67
    Missile_missiles[Missile_nextSlot].mtype = 1;
#line 68
    Missile_missiles[Missile_nextSlot].speed = 3;
#line 69
    SFX_PlayEffect(4);
    goto L6;
  L8:;
#line 71
    Missile_missiles[Missile_nextSlot].mtype = 2;
#line 72
    Missile_missiles[Missile_nextSlot].speed = 5;
#line 73
    SFX_PlayEffect(5);
    goto L6;
}

static void Missile_UpdateMissiles(void) {
    int32_t i;
    int32_t j;
    int32_t s;
    int32_t dx;
    int32_t dy;
    int32_t dist;
    int32_t hitR;
    i = 0;
    goto L1;
  L1:;
    if (i <= (6 - 1)) goto L2; else goto L4;
  L2:;
    if (Missile_missiles[i].mtype == 0) goto L6; else goto L9;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
#line 85
    Missile_missiles[i].mtype = 0;
    goto L5;
  L7:;
#line 87
    (Missile_missiles[i].timeOfFlight++);
    if (Missile_missiles[i].timeOfFlight > 40) goto L11; else goto L12;
  L8:;
    if (Missile_missiles[i].speed == 0) goto L6; else goto L7;
  L9:;
    if (Missile_missiles[i].mtype == 3) goto L6; else goto L8;
  L10:;
    goto L5;
  L11:;
#line 89
    Missile_missiles[i].mtype = 0;
    goto L10;
  L12:;
#line 92
    s = (Missile_missiles[i].speed * 2);
#line 93
    Missile_missiles[i].absX = Movement_NewX(Missile_missiles[i].absX, Missile_missiles[i].direction, s);
#line 94
    Missile_missiles[i].absY = Movement_NewY(Missile_missiles[i].absY, Missile_missiles[i].direction, s);
    if (Assets_currentRegion >= 0) goto L14; else goto L13;
  L13:;
    if (Missile_missiles[i].mtype == 2) goto L19; else goto L20;
  L14:;
#line 99
    j = Assets_GetTerrainAt(Missile_missiles[i].absX, Missile_missiles[i].absY);
    if (j == 1) goto L16; else goto L17;
  L15:;
    goto L13;
  L16:;
#line 101
    Missile_missiles[i].mtype = 0;
    goto L15;
  L17:;
    if (j == 15) goto L16; else goto L15;
  L18:;
    j = 0;
    goto L21;
  L19:;
#line 107
    hitR = 9;
    goto L18;
  L20:;
#line 109
    hitR = 6;
    goto L18;
  L21:;
    if (j <= (Actor_actorCount - 1)) goto L22; else goto L24;
  L22:;
    if (j != Missile_missiles[i].archer) goto L28; else goto L25;
  L23:;
    j = (j + 1);
    goto L21;
  L24:;
    goto L10;
  L25:;
    goto L23;
  L26:;
#line 117
    dx = (Actor_actors[j].absX - Missile_missiles[i].absX);
#line 118
    dy = (Actor_actors[j].absY - Missile_missiles[i].absY);
    if (dx < 0) goto L30; else goto L29;
  L27:;
    if (Actor_actors[j].state != 14) goto L26; else goto L25;
  L28:;
    if (Actor_actors[j].state != 15) goto L27; else goto L25;
  L29:;
    if (dy < 0) goto L32; else goto L31;
  L30:;
#line 119
    dx = (-dx);
    goto L29;
  L31:;
    if (dx > dy) goto L34; else goto L35;
  L32:;
#line 120
    dy = (-dy);
    goto L31;
  L33:;
    if (dist < hitR) goto L37; else goto L36;
  L34:;
#line 122
    dist = dx;
    goto L33;
  L35:;
    dist = dy;
    goto L33;
  L36:;
    goto L25;
  L37:;
#line 124
    Combat_DoHit(Missile_missiles[i].archer, j);
#line 125
    SFX_PlayEffect(2);
#line 126
    Missile_missiles[i].speed = 0;
#line 127
    Missile_missiles[i].mtype = 3;
#line 128
    j = Actor_actorCount;
    goto L36;
}

static int32_t Missile_S(int32_t v) {
    (void)v;
    return (v * 3);
}

static void Missile_DrawMissiles(void) {
    int32_t i;
    int32_t sx;
    int32_t sy;
    int32_t frame;
    int32_t srcY;
    if (WorldObj_objTex == NULL) goto L2; else goto L1;
  L1:;
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= (6 - 1)) goto L4; else goto L6;
  L4:;
    if (Missile_missiles[i].mtype > 0) goto L8; else goto L7;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
    return;
  L7:;
    goto L5;
  L8:;
#line 147
    sx = ((Missile_missiles[i].absX - World_camX) * 3);
#line 148
    sy = ((Missile_missiles[i].absY - World_camY) * 3);
    if (sx > (-Missile_S(20))) goto L13; else goto L9;
  L9:;
    goto L7;
  L10:;
    if (Missile_missiles[i].mtype == 3) goto L15; else goto L16;
  L11:;
    if (sy < (Missile_S(143) + 20)) goto L10; else goto L9;
  L12:;
    if (sy > (-Missile_S(20))) goto L11; else goto L9;
  L13:;
    if (sx < (Missile_S(320) + 20)) goto L12; else goto L9;
  L14:;
#line 164
    srcY = (frame * 16);
#line 165
    Platform_DrawTexRegion(WorldObj_objTex, 0, srcY, 16, 16, (sx - Missile_S(8)), (sy - Missile_S(8)), Missile_S(16), Missile_S(16));
    goto L9;
  L15:;
#line 153
    frame = 88;
    goto L14;
  L16:;
    if (Missile_missiles[i].mtype == 2) goto L17; else goto L18;
  L17:;
#line 156
    frame = (m2_mod((Missile_missiles[i].direction + 1), 8) + 89);
    goto L14;
  L18:;
#line 161
    frame = m2_mod((Missile_missiles[i].direction + 1), 8);
    goto L14;
}

/* Imported Module MathLib */

static const int32_t MathLib_RandMax = 2147483647;
static float MathLib_sqrt(float x);
static float MathLib_sin(float x);
static float MathLib_cos(float x);
static float MathLib_exp(float x);
static float MathLib_ln(float x);
static float MathLib_arctan(float x);
static int32_t MathLib_entier(float x);
static float MathLib_Random(void);
static void MathLib_Randomize(uint32_t seed);

static float MathLib_sqrt(float x) {
    (void)x;
    return sqrtf(x);
}

static float MathLib_sin(float x) {
    (void)x;
    return sinf(x);
}

static float MathLib_cos(float x) {
    (void)x;
    return cosf(x);
}

static float MathLib_exp(float x) {
    (void)x;
    return expf(x);
}

static float MathLib_ln(float x) {
    (void)x;
    return logf(x);
}

static float MathLib_arctan(float x) {
    (void)x;
    return atanf(x);
}

static int32_t MathLib_entier(float x) {
    (void)x;
    return ((int32_t)(floorf(x)));
}

static float MathLib_Random(void) {
    return ((double)(((float)(rand()))) / (double)((((float)(2147483647)) + 1.0)));
}

static void MathLib_Randomize(uint32_t seed) {
    (void)seed;
#line 35 "/Users/aoesterer/.mx/lib/m2stdlib/src/MathLib.mod"
    srand(seed);
    return;
}

/* Imported Module Music */

typedef struct Music_Voice Music_Voice;
static const int32_t Music_MoodBattle = 4;
static const int32_t Music_MoodIndoor = 20;
static const int32_t Music_MoodDeath = 24;
static const int32_t Music_MoodDay = 0;
static const int32_t Music_MoodNight = 8;
static const int32_t Music_MoodSpec = 16;
typedef struct Music_Voice Music_Voice;
struct Music_Voice {
    int32_t waveNum;
    int32_t volNum;
    int32_t volDelay;
    int32_t vceStat;
    int64_t eventStart;
    int64_t eventStop;
    int32_t volPos;
    int32_t volume;
    int32_t trakPtr;
    int32_t trakBeg;
    int32_t period;
    int32_t waveOff;
    int32_t waveLen;
    double phase;
    int active;
};

static const int32_t Music_NumVoices = 4;
static const int32_t Music_MaxTracks = 28;
static const int32_t Music_WavBufSize = 1024;
static const int32_t Music_VolBufSize = 2560;
static const int32_t Music_WaveLen = 128;
static const int32_t Music_EnvLen = 256;
static const int32_t Music_OutputRate = 44100;
static const int32_t Music_TickRate = 50;
static const int32_t Music_SamplesPerTick = 882;
static const int32_t Music_AmigaClock = 3546895;
static void Music_InitPtable(void);
static void Music_InitNotevals(void);
static void Music_InitInsMap(void);
static int Music_LoadData(void);
static void Music_InitVoice(Music_Voice *v);
static int Music_InitMusic(void);
static void Music_ShutdownMusic(void);
static void Music_StopMusic(void);
static void Music_ResumeMusic(void);
static int Music_IsPlaying(void);
static void Music_SetMood(int32_t mood);
static void Music_ProcessVoice(Music_Voice *v);
static double Music_GetWavSample(int32_t idx);
static double Music_RenderSample(void);
static void Music_SequencerTick(void);
static void Music_UpdateMusic(void);
static void Music_SetCaveWave(int cave);

int32_t Music_dev;
Music_Voice Music_voices[3 + 1];
int64_t Music_timeclock;
int32_t Music_tempo;
int Music_nosound;
int32_t Music_currentMood;
char Music_wavMem[1023 + 1];
char Music_volMem[2559 + 1];
char Music_trackData[5999 + 1];
int32_t Music_trackOff[27 + 1];
int32_t Music_trackLen[27 + 1];
int32_t Music_numTracks;
int32_t Music_insMap[11 + 1];
double Music_outBuf[1023 + 1];
int32_t Music_ptable[95 + 1];
int32_t Music_ptableOff[95 + 1];
int32_t Music_notevals[63 + 1];
int Music_inited;
int32_t Music_dbgCount;
int32_t Music_tickAccum;
double Music_lpState;
static void Music_InitPtable(void) {
    int32_t i;
#line 84 "src/Music.mod"
    Music_ptable[0] = 1440;
    Music_ptableOff[0] = 0;
#line 85
    Music_ptable[1] = 1356;
    Music_ptableOff[1] = 0;
#line 86
    Music_ptable[2] = 1280;
    Music_ptableOff[2] = 0;
#line 87
    Music_ptable[3] = 1208;
    Music_ptableOff[3] = 0;
#line 88
    Music_ptable[4] = 1140;
    Music_ptableOff[4] = 0;
#line 89
    Music_ptable[5] = 1076;
    Music_ptableOff[5] = 0;
#line 91
    Music_ptable[6] = 1016;
    Music_ptableOff[6] = 0;
#line 92
    Music_ptable[7] = 960;
    Music_ptableOff[7] = 0;
#line 93
    Music_ptable[8] = 906;
    Music_ptableOff[8] = 0;
#line 94
    Music_ptable[9] = 856;
    Music_ptableOff[9] = 0;
#line 95
    Music_ptable[10] = 808;
    Music_ptableOff[10] = 0;
#line 96
    Music_ptable[11] = 762;
    Music_ptableOff[11] = 0;
#line 97
    Music_ptable[12] = 720;
    Music_ptableOff[12] = 0;
#line 98
    Music_ptable[13] = 678;
    Music_ptableOff[13] = 0;
#line 99
    Music_ptable[14] = 640;
    Music_ptableOff[14] = 0;
#line 100
    Music_ptable[15] = 604;
    Music_ptableOff[15] = 0;
#line 101
    Music_ptable[16] = 570;
    Music_ptableOff[16] = 0;
#line 102
    Music_ptable[17] = 538;
    Music_ptableOff[17] = 0;
#line 104
    Music_ptable[18] = 508;
    Music_ptableOff[18] = 0;
#line 105
    Music_ptable[19] = 480;
    Music_ptableOff[19] = 0;
#line 106
    Music_ptable[20] = 453;
    Music_ptableOff[20] = 0;
#line 107
    Music_ptable[21] = 428;
    Music_ptableOff[21] = 0;
#line 108
    Music_ptable[22] = 404;
    Music_ptableOff[22] = 0;
#line 109
    Music_ptable[23] = 381;
    Music_ptableOff[23] = 0;
#line 110
    Music_ptable[24] = 360;
    Music_ptableOff[24] = 0;
#line 111
    Music_ptable[25] = 339;
    Music_ptableOff[25] = 0;
#line 112
    Music_ptable[26] = 320;
    Music_ptableOff[26] = 0;
#line 113
    Music_ptable[27] = 302;
    Music_ptableOff[27] = 0;
#line 114
    Music_ptable[28] = 285;
    Music_ptableOff[28] = 0;
#line 115
    Music_ptable[29] = 269;
    Music_ptableOff[29] = 0;
#line 117
    Music_ptable[30] = 508;
    Music_ptableOff[30] = 16;
#line 118
    Music_ptable[31] = 480;
    Music_ptableOff[31] = 16;
#line 119
    Music_ptable[32] = 453;
    Music_ptableOff[32] = 16;
#line 120
    Music_ptable[33] = 428;
    Music_ptableOff[33] = 16;
#line 121
    Music_ptable[34] = 404;
    Music_ptableOff[34] = 16;
#line 122
    Music_ptable[35] = 381;
    Music_ptableOff[35] = 16;
#line 123
    Music_ptable[36] = 360;
    Music_ptableOff[36] = 16;
#line 124
    Music_ptable[37] = 339;
    Music_ptableOff[37] = 16;
#line 125
    Music_ptable[38] = 320;
    Music_ptableOff[38] = 16;
#line 126
    Music_ptable[39] = 302;
    Music_ptableOff[39] = 16;
#line 127
    Music_ptable[40] = 285;
    Music_ptableOff[40] = 16;
#line 128
    Music_ptable[41] = 269;
    Music_ptableOff[41] = 16;
#line 130
    Music_ptable[42] = 508;
    Music_ptableOff[42] = 24;
#line 131
    Music_ptable[43] = 480;
    Music_ptableOff[43] = 24;
#line 132
    Music_ptable[44] = 453;
    Music_ptableOff[44] = 24;
#line 133
    Music_ptable[45] = 428;
    Music_ptableOff[45] = 24;
#line 134
    Music_ptable[46] = 404;
    Music_ptableOff[46] = 24;
#line 135
    Music_ptable[47] = 381;
    Music_ptableOff[47] = 24;
#line 136
    Music_ptable[48] = 360;
    Music_ptableOff[48] = 24;
#line 137
    Music_ptable[49] = 339;
    Music_ptableOff[49] = 24;
#line 138
    Music_ptable[50] = 320;
    Music_ptableOff[50] = 24;
#line 139
    Music_ptable[51] = 302;
    Music_ptableOff[51] = 24;
#line 140
    Music_ptable[52] = 285;
    Music_ptableOff[52] = 24;
#line 141
    Music_ptable[53] = 269;
    Music_ptableOff[53] = 24;
#line 143
    Music_ptable[54] = 508;
    Music_ptableOff[54] = 28;
#line 144
    Music_ptable[55] = 480;
    Music_ptableOff[55] = 28;
#line 145
    Music_ptable[56] = 453;
    Music_ptableOff[56] = 28;
#line 146
    Music_ptable[57] = 428;
    Music_ptableOff[57] = 28;
#line 147
    Music_ptable[58] = 404;
    Music_ptableOff[58] = 28;
#line 148
    Music_ptable[59] = 381;
    Music_ptableOff[59] = 28;
#line 149
    Music_ptable[60] = 360;
    Music_ptableOff[60] = 28;
#line 150
    Music_ptable[61] = 339;
    Music_ptableOff[61] = 28;
#line 151
    Music_ptable[62] = 320;
    Music_ptableOff[62] = 28;
#line 152
    Music_ptable[63] = 302;
    Music_ptableOff[63] = 28;
#line 153
    Music_ptable[64] = 285;
    Music_ptableOff[64] = 28;
#line 154
    Music_ptable[65] = 269;
    Music_ptableOff[65] = 28;
#line 156
    Music_ptable[66] = 254;
    Music_ptableOff[66] = 28;
#line 157
    Music_ptable[67] = 240;
    Music_ptableOff[67] = 28;
#line 158
    Music_ptable[68] = 226;
    Music_ptableOff[68] = 28;
#line 159
    Music_ptable[69] = 214;
    Music_ptableOff[69] = 28;
#line 160
    Music_ptable[70] = 202;
    Music_ptableOff[70] = 28;
#line 161
    Music_ptable[71] = 190;
    Music_ptableOff[71] = 28;
#line 162
    Music_ptable[72] = 180;
    Music_ptableOff[72] = 28;
#line 163
    Music_ptable[73] = 170;
    Music_ptableOff[73] = 28;
#line 164
    Music_ptable[74] = 160;
    Music_ptableOff[74] = 28;
#line 165
    Music_ptable[75] = 151;
    Music_ptableOff[75] = 28;
#line 166
    Music_ptable[76] = 143;
    Music_ptableOff[76] = 28;
#line 167
    Music_ptable[77] = 135;
    Music_ptableOff[77] = 28;
    i = 78;
    goto L1;
  L1:;
    if (i <= 95) goto L2; else goto L4;
  L2:;
#line 169
    Music_ptable[i] = 269;
    Music_ptableOff[i] = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

static void Music_InitNotevals(void) {
#line 175
    Music_notevals[0] = 26880;
    Music_notevals[1] = 13440;
    Music_notevals[2] = 6720;
#line 176
    Music_notevals[3] = 3360;
    Music_notevals[4] = 1680;
    Music_notevals[5] = 840;
#line 177
    Music_notevals[6] = 420;
    Music_notevals[7] = 210;
#line 178
    Music_notevals[8] = 40320;
    Music_notevals[9] = 20160;
    Music_notevals[10] = 10080;
#line 179
    Music_notevals[11] = 5040;
    Music_notevals[12] = 2520;
    Music_notevals[13] = 1260;
#line 180
    Music_notevals[14] = 630;
    Music_notevals[15] = 315;
#line 181
    Music_notevals[16] = 17920;
    Music_notevals[17] = 8960;
    Music_notevals[18] = 4480;
#line 182
    Music_notevals[19] = 2240;
    Music_notevals[20] = 1120;
    Music_notevals[21] = 560;
#line 183
    Music_notevals[22] = 280;
    Music_notevals[23] = 140;
#line 184
    Music_notevals[24] = 26880;
    Music_notevals[25] = 13440;
    Music_notevals[26] = 6720;
#line 185
    Music_notevals[27] = 3360;
    Music_notevals[28] = 1680;
    Music_notevals[29] = 840;
#line 186
    Music_notevals[30] = 420;
    Music_notevals[31] = 210;
#line 187
    Music_notevals[32] = 21504;
    Music_notevals[33] = 10752;
    Music_notevals[34] = 5376;
#line 188
    Music_notevals[35] = 2688;
    Music_notevals[36] = 1344;
    Music_notevals[37] = 672;
#line 189
    Music_notevals[38] = 336;
    Music_notevals[39] = 168;
#line 190
    Music_notevals[40] = 32256;
    Music_notevals[41] = 16128;
    Music_notevals[42] = 8064;
#line 191
    Music_notevals[43] = 4032;
    Music_notevals[44] = 2016;
    Music_notevals[45] = 1008;
#line 192
    Music_notevals[46] = 504;
    Music_notevals[47] = 252;
#line 193
    Music_notevals[48] = 23040;
    Music_notevals[49] = 11520;
    Music_notevals[50] = 5760;
#line 194
    Music_notevals[51] = 2880;
    Music_notevals[52] = 1440;
    Music_notevals[53] = 720;
#line 195
    Music_notevals[54] = 360;
    Music_notevals[55] = 180;
#line 196
    Music_notevals[56] = 34560;
    Music_notevals[57] = 17280;
    Music_notevals[58] = 8640;
#line 197
    Music_notevals[59] = 4320;
    Music_notevals[60] = 2160;
    Music_notevals[61] = 1080;
#line 198
    Music_notevals[62] = 540;
    Music_notevals[63] = 270;
    return;
}

static void Music_InitInsMap(void) {
#line 203
    Music_insMap[0] = 0;
    Music_insMap[1] = 0;
    Music_insMap[2] = 0;
    Music_insMap[3] = 0;
#line 204
    Music_insMap[4] = 5;
    Music_insMap[5] = 514;
    Music_insMap[6] = 257;
    Music_insMap[7] = 259;
#line 205
    Music_insMap[8] = 4;
    Music_insMap[9] = 1284;
    Music_insMap[10] = 256;
    Music_insMap[11] = 1280;
    return;
}

static int Music_LoadData(void) {
    void * fd;
    int32_t n;
    int32_t i;
    int32_t off;
    int32_t packLen;
    char buf4[3 + 1];
    char p[127 + 1];
#line 215
    Assets_AssetPath("wavmem.bin", 10, p, 127);
#line 216
    m2_BinaryIO_OpenRead(p, &fd);
    if (fd == NULL) goto L2; else goto L1;
  L1:;
#line 221
    m2_BinaryIO_ReadBytes(fd, ((void *)&(Music_wavMem)), 1024, &n);
#line 222
    m2_BinaryIO_Close(fd);
#line 223
    m2_WriteString("Music: wav read ");
    m2_WriteInt(n, 5);
#line 224
    m2_WriteString(" bytes, wav[1]=");
    m2_WriteInt(((int32_t)((unsigned char)(Music_wavMem[1]))), 4);
    m2_WriteLn();
    if (n < 1024) goto L4; else goto L3;
  L2:;
#line 218
    m2_WriteString("Music: wavmem.bin not found");
    m2_WriteLn();
    return 0;
  L3:;
#line 231
    Assets_AssetPath("volmem.bin", 10, p, 127);
#line 232
    m2_BinaryIO_OpenRead(p, &fd);
    if (fd == NULL) goto L6; else goto L5;
  L4:;
#line 226
    m2_WriteString("Music: wav read failed!");
    m2_WriteLn();
    return 0;
  L5:;
#line 237
    m2_BinaryIO_ReadBytes(fd, ((void *)&(Music_volMem)), 2560, &n);
#line 238
    m2_BinaryIO_Close(fd);
    if (n < 2560) goto L8; else goto L7;
  L6:;
#line 234
    m2_WriteString("Music: volmem.bin not found");
    m2_WriteLn();
    return 0;
  L7:;
#line 245
    Assets_AssetPath("songs.bin", 9, p, 127);
#line 246
    m2_BinaryIO_OpenRead(p, &fd);
    if (fd == NULL) goto L10; else goto L9;
  L8:;
#line 240
    m2_WriteString("Music: vol read failed!");
    m2_WriteLn();
    return 0;
  L9:;
#line 251
    off = 0;
#line 252
    Music_numTracks = 0;
    i = 0;
    goto L11;
  L10:;
#line 248
    m2_WriteString("Music: songs.bin not found");
    m2_WriteLn();
    return 0;
  L11:;
    if (i <= (28 - 1)) goto L12; else goto L14;
  L12:;
#line 254
    m2_BinaryIO_ReadBytes(fd, ((void *)&(buf4)), 4, &n);
    if (n < 4) goto L16; else goto L17;
  L13:;
    i = (i + 1);
    goto L11;
  L14:;
#line 271
    m2_BinaryIO_Close(fd);
#line 273
    m2_WriteString("Music: ");
    m2_WriteInt(Music_numTracks, 1);
#line 274
    m2_WriteString(" tracks loaded");
    m2_WriteLn();
    return Music_numTracks > 0;
  L15:;
    goto L13;
  L16:;
#line 256
    i = 28;
    goto L15;
  L17:;
#line 258
    packLen = ((((((int32_t)((unsigned char)(buf4[0]))) * 16777216) + (((int32_t)((unsigned char)(buf4[1]))) * 65536)) + (((int32_t)((unsigned char)(buf4[2]))) * 256)) + ((int32_t)((unsigned char)(buf4[3]))));
#line 260
    Music_trackOff[Music_numTracks] = off;
#line 261
    Music_trackLen[Music_numTracks] = (packLen * 2);
    if ((off + (packLen * 2)) > 5999) goto L19; else goto L20;
  L18:;
    goto L15;
  L19:;
#line 263
    i = 28;
    goto L18;
  L20:;
#line 265
    m2_BinaryIO_ReadBytes(fd, ((void *)&(Music_trackData[off])), (packLen * 2), &n);
#line 266
    (off += (packLen * 2));
#line 267
    (Music_numTracks++);
    goto L18;
}

static void Music_InitVoice(Music_Voice *v) {
    (void)v;
#line 280
    (*v).waveNum = 0;
    (*v).volNum = 0;
#line 281
    (*v).volDelay = (-1);
    (*v).vceStat = 0;
#line 282
    (*v).eventStart = 0;
    (*v).eventStop = 0;
#line 283
    (*v).volPos = 0;
    (*v).volume = 0;
#line 284
    (*v).trakPtr = (-1);
    (*v).trakBeg = (-1);
#line 285
    (*v).period = 428;
    (*v).waveOff = 0;
    (*v).waveLen = 32;
#line 286
    (*v).phase = 0.0;
    (*v).active = 0;
    return;
}

static int Music_InitMusic(void) {
    int32_t i;
#line 292
    Music_inited = 0;
#line 293
    Music_nosound = 1;
#line 294
    Music_dbgCount = 0;
#line 295
    Music_tickAccum = 0;
#line 296
    Music_lpState = 0.0;
#line 297
    Music_currentMood = (-1);
#line 298
    Music_timeclock = 0;
#line 299
    Music_tempo = 150;
    i = 0;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
#line 301
    Music_InitVoice(&Music_voices[i]);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 302
    Music_InitPtable();
#line 303
    Music_InitNotevals();
#line 304
    Music_InitInsMap();
    if (Music_LoadData()) goto L5; else goto L6;
  L5:;
    if (Playback_InitAudio()) goto L7; else goto L8;
  L6:;
    return 0;
  L7:;
#line 313
    Music_dev = Playback_OpenDevice(44100, 1, Playback_FormatS16, 2048);
    if (Music_dev == 0) goto L10; else goto L9;
  L8:;
#line 309
    m2_WriteString("Music: SDL audio init failed");
    m2_WriteLn();
    return 0;
  L9:;
#line 320
    Playback_ResumeDevice(Music_dev);
#line 321
    Music_inited = 1;
#line 322
    m2_WriteString("Music: initialized");
    m2_WriteLn();
    return 1;
  L10:;
#line 315
    m2_WriteString("Music: audio device open failed");
    m2_WriteLn();
#line 316
    Playback_QuitAudio();
    return 0;
}

static void Music_ShutdownMusic(void) {
    if (Music_inited) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 329
    Playback_CloseDevice(Music_dev);
#line 330
    Playback_QuitAudio();
#line 331
    Music_inited = 0;
    goto L1;
}

static void Music_StopMusic(void) {
    int32_t i;
#line 338
    Music_nosound = 1;
    i = 0;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
#line 340
    Music_voices[i].active = 0;
#line 341
    Music_voices[i].volume = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 343
    Playback_ClearQueued(Music_dev);
    return;
}

static void Music_ResumeMusic(void) {
#line 348
    Music_currentMood = (-1);
#line 349
    Music_nosound = 0;
    return;
}

static int Music_IsPlaying(void) {
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
    if (Music_voices[i].active) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
    return 1;
}

static void Music_SetMood(int32_t mood) {
    (void)mood;
    int32_t i;
    int32_t tIdx;
    if (Music_inited) goto L1; else goto L2;
  L1:;
    if (mood == Music_currentMood) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 366
    Music_currentMood = mood;
#line 369
    Music_timeclock = 0;
#line 370
    Music_tickAccum = 0;
    i = 0;
    goto L5;
  L4:;
    return;
  L5:;
    if (i <= 3) goto L6; else goto L8;
  L6:;
#line 372
    tIdx = (mood + i);
    if (tIdx >= 0) goto L12; else goto L11;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 391
    Music_nosound = 0;
#line 392
    Music_tempo = 150;
    return;
  L9:;
    goto L7;
  L10:;
#line 374
    Music_voices[i].trakBeg = Music_trackOff[tIdx];
#line 375
    Music_voices[i].trakPtr = Music_trackOff[tIdx];
#line 376
    Music_voices[i].eventStart = 0;
#line 377
    Music_voices[i].eventStop = 0;
#line 378
    Music_voices[i].volDelay = (-1);
#line 379
    Music_voices[i].vceStat = 0;
#line 380
    Music_voices[i].volume = 0;
#line 381
    Music_voices[i].phase = 0.0;
#line 382
    Music_voices[i].active = 1;
#line 385
    Music_voices[i].waveNum = m2_div(Music_insMap[i], 256);
#line 386
    Music_voices[i].volNum = m2_mod(Music_insMap[i], 256);
    goto L9;
  L11:;
#line 388
    Music_voices[i].active = 0;
    goto L9;
  L12:;
    if (tIdx < Music_numTracks) goto L10; else goto L11;
}

static void Music_ProcessVoice(Music_Voice *v) {
    (void)v;
    int32_t cmd;
    int32_t val;
    int32_t noteIdx;
    int32_t durIdx;
    int64_t dur;
    int64_t gap;
    if ((*v).active) goto L1; else goto L2;
  L1:;
    if ((*v).trakPtr < 0) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (Music_timeclock < (*v).eventStart) goto L6; else goto L5;
  L4:;
    return;
  L5:;
    goto L17;
  L6:;
    if (Music_timeclock >= (*v).eventStop) goto L8; else goto L9;
  L7:;
    return;
  L8:;
#line 406
    (*v).volume = 0;
    goto L7;
  L9:;
    if ((*v).volDelay >= 0) goto L10; else goto L7;
  L10:;
    if ((*v).volPos < 256) goto L12; else goto L11;
  L11:;
    goto L7;
  L12:;
#line 410
    val = ((int32_t)((unsigned char)(Music_volMem[(((*v).volNum * 256) + (*v).volPos)])));
    if (val < 128) goto L14; else goto L13;
  L13:;
    goto L11;
  L14:;
    if (val > 64) goto L16; else goto L15;
  L15:;
#line 413
    (*v).volume = val;
#line 414
    ((*v).volPos++);
    goto L13;
  L16:;
#line 412
    val = 64;
    goto L15;
  L17:;
    if ((*v).trakPtr < 0) goto L20; else goto L21;
  L18:;
    return;
  L19:;
#line 428
    cmd = ((int32_t)((unsigned char)(Music_trackData[(*v).trakPtr])));
#line 429
    val = ((int32_t)((unsigned char)(Music_trackData[((*v).trakPtr + 1)])));
#line 430
    ((*v).trakPtr += 2);
    if (cmd == 255) goto L23; else goto L24;
  L20:;
#line 424
    (*v).active = 0;
    return;
  L21:;
    if (((*v).trakPtr + 1) >= 6000) goto L20; else goto L19;
  L22:;
    goto L17;
  L23:;
    if (val != 0) goto L26; else goto L27;
  L24:;
    if (cmd == 129) goto L28; else goto L29;
  L25:;
    goto L22;
  L26:;
#line 435
    (*v).trakPtr = (*v).trakBeg;
    goto L25;
  L27:;
#line 437
    (*v).active = 0;
    return;
  L28:;
#line 443
    val = ((uint32_t)(((uint32_t)(val))) & (uint32_t)(15));
    if (val < 12) goto L31; else goto L30;
  L29:;
    if (cmd == 144) goto L32; else goto L33;
  L30:;
    goto L22;
  L31:;
#line 445
    (*v).waveNum = m2_div(Music_insMap[val], 256);
#line 446
    (*v).volNum = m2_mod(Music_insMap[val], 256);
    goto L30;
  L32:;
#line 450
    Music_tempo = ((uint32_t)(((uint32_t)(val))) & (uint32_t)(255));
    goto L22;
  L33:;
#line 453
    noteIdx = ((uint32_t)(((uint32_t)(cmd))) & (uint32_t)(127));
#line 454
    durIdx = ((uint32_t)(((uint32_t)(val))) & (uint32_t)(63));
    if (durIdx > 63) goto L35; else goto L34;
  L34:;
#line 457
    dur = ((int64_t)(Music_notevals[durIdx]));
#line 458
    gap = (dur - 300);
    if (gap < 0) goto L37; else goto L36;
  L35:;
#line 456
    durIdx = 63;
    goto L34;
  L36:;
#line 461
    (*v).eventStop = ((*v).eventStart + gap);
#line 462
    (*v).eventStart = ((*v).eventStart + dur);
    if (cmd >= 128) goto L39; else goto L40;
  L37:;
#line 459
    gap = dur;
    goto L36;
  L38:;
    return;
  L39:;
#line 466
    (*v).volume = 0;
    goto L38;
  L40:;
    if (noteIdx < 78) goto L42; else goto L41;
  L41:;
#line 475
    (*v).volPos = 0;
#line 476
    (*v).volDelay = 0;
    if (((*v).volNum * 256) < 2560) goto L44; else goto L45;
  L42:;
#line 470
    (*v).period = Music_ptable[noteIdx];
#line 471
    (*v).waveOff = (Music_ptableOff[noteIdx] * 4);
#line 472
    (*v).waveLen = (32 - Music_ptableOff[noteIdx]);
    goto L41;
  L43:;
    goto L38;
  L44:;
#line 478
    (*v).volume = ((int32_t)((unsigned char)(Music_volMem[((*v).volNum * 256)])));
#line 479
    ((*v).volPos++);
    goto L43;
  L45:;
#line 481
    (*v).volume = 64;
    goto L43;
}

static double Music_GetWavSample(int32_t idx) {
    (void)idx;
    int32_t v;
    if (idx < 0) goto L2; else goto L3;
  L1:;
#line 496
    v = ((int32_t)((unsigned char)(Music_wavMem[idx])));
    if (v >= 128) goto L5; else goto L4;
  L2:;
    return 0.0;
  L3:;
    if (idx >= 1024) goto L2; else goto L1;
  L4:;
    return ((double)(((float)(v))) / (double)(128.0));
  L5:;
#line 497
    (v -= 256);
    goto L4;
}

static double Music_RenderSample(void) {
    int32_t i;
    int32_t waveBase;
    int32_t waveBytes;
    int32_t idx0;
    int32_t idx1;
    double freq;
    double phaseInc;
    double mix;
    double frac;
    double s0;
    double s1;
    double voiceSample;
#line 506
    mix = 0.0;
    i = 0;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
    if (Music_voices[i].active) goto L8; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 538
    Music_lpState = ((Music_lpState * 0.6) + (mix * 0.4));
    if (Music_lpState > 1.0) goto L17; else goto L18;
  L5:;
    goto L3;
  L6:;
#line 510
    waveBase = ((Music_voices[i].waveNum * 128) + Music_voices[i].waveOff);
#line 511
    waveBytes = (Music_voices[i].waveLen * 2);
    if (waveBytes <= 0) goto L10; else goto L9;
  L7:;
    if (Music_voices[i].period > 0) goto L6; else goto L5;
  L8:;
    if (Music_voices[i].volume > 0) goto L7; else goto L5;
  L9:;
#line 514
    freq = ((double)(((float)(3546895))) / (double)(((float)(Music_voices[i].period))));
#line 515
    phaseInc = ((double)(freq) / (double)(((float)(44100))));
#line 518
    idx0 = m2_mod(((int32_t)(Music_voices[i].phase)), waveBytes);
    if (idx0 < 0) goto L12; else goto L11;
  L10:;
#line 512
    waveBytes = 64;
    goto L9;
  L11:;
#line 520
    idx1 = m2_mod((idx0 + 1), waveBytes);
#line 521
    frac = (Music_voices[i].phase - ((float)(((int32_t)(Music_voices[i].phase)))));
#line 523
    s0 = Music_GetWavSample((waveBase + idx0));
#line 524
    s1 = Music_GetWavSample((waveBase + idx1));
#line 525
    voiceSample = (s0 + ((s1 - s0) * frac));
#line 527
    mix = (mix + ((double)((voiceSample * ((float)(Music_voices[i].volume)))) / (double)(256.0)));
#line 529
    Music_voices[i].phase = (Music_voices[i].phase + phaseInc);
    goto L13;
  L12:;
#line 519
    idx0 = 0;
    goto L11;
  L13:;
    if (Music_voices[i].phase >= ((float)(waveBytes))) goto L14; else goto L15;
  L14:;
#line 531
    Music_voices[i].phase = (Music_voices[i].phase - ((float)(waveBytes)));
    goto L13;
  L15:;
    goto L5;
  L16:;
    return Music_lpState;
  L17:;
    return 1.0;
  L18:;
    if (Music_lpState < (-1.0)) goto L19; else goto L16;
  L19:;
    return (-1.0);
}

static void Music_SequencerTick(void) {
    int32_t i;
#line 552
    (Music_timeclock += (((int64_t)(Music_tempo)) * 2));
    i = 0;
    goto L1;
  L1:;
    if (i <= 3) goto L2; else goto L4;
  L2:;
#line 554
    Music_ProcessVoice(&Music_voices[i]);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

static void Music_UpdateMusic(void) {
    static const int32_t MaxSamplesPerCall = 882;
    static const int32_t QueueTarget = 4410;
    int32_t s;
    int32_t toGenerate;
    int32_t sampleCount;
    uint32_t queued;
    int ok;
    if (Music_inited) goto L1; else goto L2;
  L1:;
    if (Music_nosound) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 571
    queued = Playback_GetQueuedBytes(Music_dev);
    if (queued >= ((uint32_t)((4410 * 2)))) goto L6; else goto L5;
  L4:;
    return;
  L5:;
#line 573
    toGenerate = (4410 - m2_div(((int32_t)(queued)), 2));
    if (toGenerate > 882) goto L8; else goto L7;
  L6:;
    return;
  L7:;
    if (toGenerate <= 0) goto L10; else goto L9;
  L8:;
#line 574
    toGenerate = 882;
    goto L7;
  L9:;
#line 579
    sampleCount = 0;
    goto L11;
  L10:;
    return;
  L11:;
    if (sampleCount < toGenerate) goto L12; else goto L13;
  L12:;
    if (Music_tickAccum >= 882) goto L15; else goto L14;
  L13:;
    if (sampleCount > 0) goto L19; else goto L18;
  L14:;
#line 588
    Music_outBuf[sampleCount] = Music_RenderSample();
#line 589
    (sampleCount++);
#line 590
    (Music_tickAccum++);
    if (sampleCount >= 1024) goto L17; else goto L16;
  L15:;
#line 583
    (Music_tickAccum -= 882);
#line 584
    Music_SequencerTick();
    goto L14;
  L16:;
    goto L11;
  L17:;
#line 594
    ok = Playback_QueueSamples(Music_dev, ((void *)&(Music_outBuf)), sampleCount, 1);
#line 595
    sampleCount = 0;
    goto L16;
  L18:;
    return;
  L19:;
#line 601
    ok = Playback_QueueSamples(Music_dev, ((void *)&(Music_outBuf)), sampleCount, 1);
    goto L18;
}

static void Music_SetCaveWave(int cave) {
    (void)cave;
    if (cave) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 610
    Music_insMap[10] = 775;
    goto L1;
  L3:;
#line 612
    Music_insMap[10] = 256;
    goto L1;
}

/* Imported Module EnemyAI */

static int32_t EnemyAI_Rand(int32_t limit);
static void EnemyAI_CalcDist(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t *xd, int32_t *yd);
static void EnemyAI_SetCourse(int32_t actorIdx, int32_t targetX, int32_t targetY);
static void EnemyAI_SetCourseAway(int32_t actorIdx, int32_t targetX, int32_t targetY);
static int EnemyAI_StepMove(int32_t i);
static void EnemyAI_SelectTactic(int32_t i, int32_t tactic);
static void EnemyAI_UpdateOne(int32_t i);
static void EnemyAI_UpdateEnemies(void);

int32_t EnemyAI_rng;
static int32_t EnemyAI_Rand(int32_t limit) {
    (void)limit;
#line 28 "src/EnemyAI.mod"
    EnemyAI_rng = ((EnemyAI_rng * 1103515245) + 12345);
    if (EnemyAI_rng < 0) goto L2; else goto L1;
  L1:;
    if (limit <= 0) goto L4; else goto L3;
  L2:;
#line 29
    EnemyAI_rng = (-EnemyAI_rng);
    goto L1;
  L3:;
    return m2_mod(m2_div(EnemyAI_rng, 65536), limit);
  L4:;
    return 0;
}

static void EnemyAI_CalcDist(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t *xd, int32_t *yd) {
    (void)ax;
    (void)ay;
    (void)bx;
    (void)by;
    (void)xd;
    (void)yd;
#line 36
    (*xd) = (ax - bx);
    (*yd) = (ay - by);
    if ((*xd) < 0) goto L2; else goto L1;
  L1:;
    if ((*yd) < 0) goto L4; else goto L3;
  L2:;
#line 37
    (*xd) = (-(*xd));
    goto L1;
  L3:;
    return;
  L4:;
#line 38
    (*yd) = (-(*yd));
    goto L3;
}

static void EnemyAI_SetCourse(int32_t actorIdx, int32_t targetX, int32_t targetY) {
    (void)actorIdx;
    (void)targetX;
    (void)targetY;
    int32_t dx;
    int32_t dy;
#line 44
    dx = (targetX - Actor_actors[actorIdx].absX);
#line 45
    dy = (targetY - Actor_actors[actorIdx].absY);
    if (dx > 3) goto L4; else goto L3;
  L1:;
#line 55
    Actor_actors[actorIdx].state = 12;
    return;
  L2:;
#line 46
    Actor_actors[actorIdx].facing = 1;
    goto L1;
  L3:;
    if (dx > 3) goto L7; else goto L6;
  L4:;
    if (dy < (-3)) goto L2; else goto L3;
  L5:;
#line 47
    Actor_actors[actorIdx].facing = 3;
    goto L1;
  L6:;
    if (dx < (-3)) goto L10; else goto L9;
  L7:;
    if (dy > 3) goto L5; else goto L6;
  L8:;
#line 48
    Actor_actors[actorIdx].facing = 5;
    goto L1;
  L9:;
    if (dx < (-3)) goto L13; else goto L12;
  L10:;
    if (dy > 3) goto L8; else goto L9;
  L11:;
#line 49
    Actor_actors[actorIdx].facing = 7;
    goto L1;
  L12:;
    if (dx > 3) goto L14; else goto L15;
  L13:;
    if (dy < (-3)) goto L11; else goto L12;
  L14:;
#line 50
    Actor_actors[actorIdx].facing = 2;
    goto L1;
  L15:;
    if (dx < (-3)) goto L16; else goto L17;
  L16:;
#line 51
    Actor_actors[actorIdx].facing = 6;
    goto L1;
  L17:;
    if (dy < (-3)) goto L18; else goto L19;
  L18:;
#line 52
    Actor_actors[actorIdx].facing = 0;
    goto L1;
  L19:;
    if (dy > 3) goto L20; else goto L1;
  L20:;
#line 53
    Actor_actors[actorIdx].facing = 4;
    goto L1;
}

static void EnemyAI_SetCourseAway(int32_t actorIdx, int32_t targetX, int32_t targetY) {
    (void)actorIdx;
    (void)targetX;
    (void)targetY;
#line 60
    EnemyAI_SetCourse(actorIdx, ((Actor_actors[actorIdx].absX * 2) - targetX), ((Actor_actors[actorIdx].absY * 2) - targetY));
    return;
}

static int EnemyAI_StepMove(int32_t i) {
    (void)i;
    int32_t orig;
#line 70
    orig = Actor_actors[i].facing;
    if (Movement_MoveActor(i, orig, 1)) goto L2; else goto L1;
  L1:;
#line 73
    Actor_actors[i].facing = m2_mod((orig + 1), 8);
    if (Movement_MoveActor(i, Actor_actors[i].facing, 1)) goto L4; else goto L3;
  L2:;
    return 1;
  L3:;
#line 76
    Actor_actors[i].facing = m2_mod((orig + 7), 8);
    if (Movement_MoveActor(i, Actor_actors[i].facing, 1)) goto L6; else goto L5;
  L4:;
    return 1;
  L5:;
#line 79
    Actor_actors[i].facing = orig;
#line 80
    Actor_actors[i].tactic = 9;
    return 0;
  L6:;
    return 1;
}

static void EnemyAI_SelectTactic(int32_t i, int32_t tactic) {
    (void)i;
    (void)tactic;
    int r;
    int32_t xd;
    int32_t yd;
    if (Actor_actors[i].goal == 2) goto L2; else goto L3;
  L1:;
#line 98
    Actor_actors[i].tactic = tactic;
    if (tactic == 1) goto L5; else goto L6;
  L2:;
#line 93
    r = EnemyAI_Rand(4) == 0;
    goto L1;
  L3:;
#line 95
    r = EnemyAI_Rand(8) == 0;
    goto L1;
  L4:;
    return;
  L5:;
    if (r) goto L8; else goto L7;
  L6:;
    if (tactic == 8) goto L9; else goto L10;
  L7:;
    goto L4;
  L8:;
#line 101
    EnemyAI_SetCourse(i, Actor_actors[0].absX, Actor_actors[0].absY);
    goto L7;
  L9:;
#line 106
    EnemyAI_CalcDist(Actor_actors[i].absX, Actor_actors[i].absY, Actor_actors[0].absX, Actor_actors[0].absY, &xd, &yd);
    if (EnemyAI_Rand(2) == 0) goto L15; else goto L13;
  L10:;
    if (tactic == 4) goto L19; else goto L20;
  L11:;
    goto L4;
  L12:;
#line 112
    EnemyAI_SetCourse(i, Actor_actors[0].absX, Actor_actors[0].absY);
#line 113
    Actor_actors[i].state = 24;
    goto L11;
  L13:;
#line 115
    EnemyAI_SetCourse(i, Actor_actors[0].absX, Actor_actors[0].absY);
    goto L11;
  L14:;
    if (Actor_actors[i].state < 24) goto L12; else goto L13;
  L15:;
    if (xd < 8) goto L14; else goto L17;
  L16:;
    if (xd > (yd - 5)) goto L18; else goto L13;
  L17:;
    if (yd < 8) goto L14; else goto L16;
  L18:;
    if (xd < (yd + 7)) goto L14; else goto L13;
  L19:;
    if (r) goto L22; else goto L21;
  L20:;
    if (tactic == 3) goto L23; else goto L24;
  L21:;
#line 120
    Actor_actors[i].state = 12;
    goto L4;
  L22:;
#line 119
    Actor_actors[i].facing = EnemyAI_Rand(8);
    goto L21;
  L23:;
    if (r) goto L26; else goto L25;
  L24:;
    if (tactic == 5) goto L27; else goto L28;
  L25:;
    goto L4;
  L26:;
#line 123
    EnemyAI_SetCourse(i, Actor_actors[0].absX, Actor_actors[0].absY);
    goto L25;
  L27:;
    if (r) goto L30; else goto L29;
  L28:;
    if (tactic == 2) goto L31; else goto L32;
  L29:;
    goto L4;
  L30:;
#line 126
    EnemyAI_SetCourseAway(i, Actor_actors[0].absX, Actor_actors[0].absY);
    goto L29;
  L31:;
    if (r) goto L34; else goto L33;
  L32:;
    if (tactic == 6) goto L40; else goto L41;
  L33:;
    goto L4;
  L34:;
    if (Actor_actorCount > 2) goto L36; else goto L35;
  L35:;
    goto L33;
  L36:;
    if (i > 1) goto L38; else goto L39;
  L37:;
    goto L35;
  L38:;
#line 132
    EnemyAI_SetCourse(i, Actor_actors[(i - 1)].absX, (Actor_actors[(i - 1)].absY + 20));
    goto L37;
  L39:;
#line 134
    EnemyAI_SetCourse(i, Actor_actors[2].absX, (Actor_actors[2].absY + 20));
    goto L37;
  L40:;
    if (r) goto L43; else goto L42;
  L41:;
    if (tactic == 9) goto L44; else goto L4;
  L42:;
    goto L4;
  L43:;
#line 141
    Actor_actors[i].facing = m2_mod((Actor_actors[i].facing + 2), 8);
#line 142
    Actor_actors[i].state = 12;
    goto L42;
  L44:;
    if (((uint32_t)(((uint32_t)(Actor_actors[i].weapon))) & (uint32_t)(4)) != 0) goto L46; else goto L47;
  L45:;
    goto L4;
  L46:;
#line 147
    EnemyAI_SelectTactic(i, (EnemyAI_Rand(4) + 2));
    goto L45;
  L47:;
#line 149
    EnemyAI_SelectTactic(i, (EnemyAI_Rand(2) + 3));
    goto L45;
}

static void EnemyAI_UpdateOne(int32_t i) {
    (void)i;
    int32_t xd;
    int32_t yd;
    int32_t mode;
    int32_t tactic;
    int32_t thresh;
    int32_t maxDist;
    int r;
    if (Actor_actors[i].state == 15) goto L2; else goto L1;
  L1:;
    if (Actor_actors[i].state == 14) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (Actor_actors[i].actorType == 4) goto L20; else goto L19;
  L4:;
    if (Actor_actors[i].tactic <= 0) goto L6; else goto L7;
  L5:;
    return;
  L6:;
#line 163
    Actor_actors[i].tactic = 30;
    goto L5;
  L7:;
#line 165
    (Actor_actors[i].tactic--);
    if (Actor_actors[i].tactic <= 0) goto L9; else goto L8;
  L8:;
    goto L5;
  L9:;
    if (Actor_actors[i].race == 8) goto L11; else goto L12;
  L10:;
    goto L8;
  L11:;
#line 170
    Actor_actors[i].state = 15;
#line 171
    Actor_actors[i].absX = 0;
#line 172
    Actor_actors[i].absY = 0;
#line 173
    Actor_actors[i].visible = 0;
    goto L10;
  L12:;
    if (Actor_actors[i].race == 9) goto L15; else goto L14;
  L13:;
#line 176
    WorldObj_AddObj(Actor_actors[i].absX, Actor_actors[i].absY, 139, 1, (-1));
#line 177
    Actor_actors[i].race = 10;
#line 178
    Actor_actors[i].vitality = 4;
#line 179
    Actor_actors[i].weapon = 0;
#line 180
    Actor_actors[i].state = 13;
#line 181
    Actor_actors[i].goal = 6;
    goto L10;
  L14:;
    if (Actor_actors[i].race == 9) goto L18; else goto L17;
  L15:;
    if (Actor_actors[i].actorType == 2) goto L13; else goto L14;
  L16:;
#line 184
    WorldObj_AddObj(Actor_actors[i].absX, Actor_actors[i].absY, 27, 1, (-1));
#line 185
    Actor_actors[i].state = 15;
    goto L10;
  L17:;
#line 188
    Actor_actors[i].state = 15;
    goto L10;
  L18:;
    if (Actor_actors[i].actorType == 4) goto L16; else goto L17;
  L19:;
    if (Actor_actors[i].actorType == 5) goto L22; else goto L21;
  L20:;
    return;
  L21:;
    if (Actor_actors[i].actorType == 6) goto L24; else goto L23;
  L22:;
    return;
  L23:;
    if (Actor_actors[i].vitality < 1) goto L26; else goto L25;
  L24:;
    return;
  L25:;
#line 200
    mode = Actor_actors[i].goal;
#line 201
    tactic = Actor_actors[i].tactic;
#line 202
    EnemyAI_CalcDist(Actor_actors[i].absX, Actor_actors[i].absY, Actor_actors[0].absX, Actor_actors[0].absY, &xd, &yd);
    if (xd > 300) goto L28; else goto L29;
  L26:;
    return;
  L27:;
    if (Actor_actors[0].state == 15) goto L31; else goto L30;
  L28:;
#line 207
    Actor_actors[i].state = 13;
    return;
  L29:;
    if (yd > 300) goto L28; else goto L27;
  L30:;
    if (Actor_actors[i].vitality < 2) goto L34; else goto L32;
  L31:;
#line 212
    mode = 5;
    goto L30;
  L32:;
    if (Actor_actors[i].state == 24) goto L37; else goto L36;
  L33:;
#line 215
    mode = 5;
    goto L32;
  L34:;
    if (Carrier_turtleEggs) goto L35; else goto L33;
  L35:;
    if (Actor_actors[i].race == 4) goto L32; else goto L33;
  L36:;
    if (Actor_actors[i].state == 25) goto L39; else goto L38;
  L37:;
#line 220
    Missile_FireMissile(i);
#line 221
    Actor_actors[i].state = 25;
#line 222
    Actor_actors[i].goal = mode;
    return;
  L38:;
    if (mode <= 4) goto L41; else goto L42;
  L39:;
#line 226
    Actor_actors[i].state = 13;
#line 227
    Actor_actors[i].goal = mode;
    return;
  L40:;
#line 329
    Actor_actors[i].goal = mode;
    return;
  L41:;
#line 234
    r = EnemyAI_Rand(16) == 0;
    if (Carrier_turtleEggs) goto L46; else goto L45;
  L42:;
    if (mode == 5) goto L79; else goto L80;
  L43:;
#line 262
    EnemyAI_SelectTactic(i, Actor_actors[i].tactic);
    if (Actor_actors[i].state == 12) goto L64; else goto L63;
  L44:;
#line 238
    EnemyAI_SetCourse(i, 23087, 5667);
#line 239
    Actor_actors[i].state = 12;
    goto L43;
  L45:;
    if (r) goto L47; else goto L43;
  L46:;
    if (Actor_actors[i].race == 4) goto L44; else goto L45;
  L47:;
    if (Actor_actors[i].weapon < 1) goto L49; else goto L50;
  L48:;
    goto L43;
  L49:;
#line 243
    mode = 10;
#line 244
    Actor_actors[i].tactic = 4;
    goto L48;
  L50:;
    if (Actor_actors[i].vitality < 6) goto L53; else goto L52;
  L51:;
#line 246
    Actor_actors[i].tactic = 6;
    goto L48;
  L52:;
    if (mode >= 3) goto L54; else goto L55;
  L53:;
    if (EnemyAI_Rand(2) == 0) goto L51; else goto L52;
  L54:;
    if (xd < 40) goto L59; else goto L58;
  L55:;
#line 256
    Actor_actors[i].tactic = 1;
    goto L48;
  L56:;
    goto L48;
  L57:;
#line 249
    Actor_actors[i].tactic = 5;
    goto L56;
  L58:;
    if (xd < 70) goto L62; else goto L61;
  L59:;
    if (yd < 30) goto L57; else goto L58;
  L60:;
#line 251
    Actor_actors[i].tactic = 8;
    goto L56;
  L61:;
#line 253
    Actor_actors[i].tactic = 1;
    goto L56;
  L62:;
    if (yd < 70) goto L60; else goto L61;
  L63:;
#line 272
    thresh = (14 - mode);
    if (thresh < 8) goto L68; else goto L67;
  L64:;
    if (EnemyAI_StepMove(i)) goto L65; else goto L66;
  L65:;
    goto L63;
  L66:;
#line 267
    Actor_actors[i].state = 13;
    goto L65;
  L67:;
    if (xd > yd) goto L70; else goto L71;
  L68:;
#line 273
    thresh = 8;
    goto L67;
  L69:;
    if (Actor_actors[i].state == 0) goto L73; else goto L74;
  L70:;
#line 274
    maxDist = xd;
    goto L69;
  L71:;
    maxDist = yd;
    goto L69;
  L72:;
    goto L40;
  L73:;
    if (maxDist >= (thresh + 6)) goto L76; else goto L75;
  L74:;
    if (((uint32_t)(((uint32_t)(Actor_actors[i].weapon))) & (uint32_t)(4)) == 0) goto L78; else goto L72;
  L75:;
    goto L72;
  L76:;
#line 277
    Actor_actors[i].state = 13;
    goto L75;
  L77:;
#line 282
    EnemyAI_SetCourse(i, Actor_actors[0].absX, Actor_actors[0].absY);
#line 283
    Actor_actors[i].state = 0;
    goto L72;
  L78:;
    if (maxDist < thresh) goto L77; else goto L72;
  L79:;
    if (Actor_actors[i].tactic == 9) goto L82; else goto L83;
  L80:;
    if (mode == 9) goto L88; else goto L89;
  L81:;
    if (Actor_actors[i].state == 12) goto L85; else goto L84;
  L82:;
#line 291
    Actor_actors[i].facing = EnemyAI_Rand(8);
#line 292
    Actor_actors[i].state = 12;
#line 293
    Actor_actors[i].tactic = 5;
    goto L81;
  L83:;
#line 295
    EnemyAI_SelectTactic(i, 5);
    goto L81;
  L84:;
    goto L40;
  L85:;
    if (EnemyAI_StepMove(i)) goto L86; else goto L87;
  L86:;
    goto L84;
  L87:;
#line 299
    Actor_actors[i].facing = EnemyAI_Rand(8);
#line 300
    Actor_actors[i].state = 12;
    goto L86;
  L88:;
    if (EnemyAI_Rand(8) == 0) goto L91; else goto L90;
  L89:;
    if (mode == 6) goto L96; else goto L97;
  L90:;
    if (Actor_actors[i].state == 12) goto L93; else goto L92;
  L91:;
#line 305
    EnemyAI_SelectTactic(i, 2);
    goto L90;
  L92:;
    goto L40;
  L93:;
    if (EnemyAI_StepMove(i)) goto L94; else goto L95;
  L94:;
    goto L92;
  L95:;
#line 307
    Actor_actors[i].state = 13;
    goto L94;
  L96:;
    if (EnemyAI_Rand(16) == 0) goto L99; else goto L98;
  L97:;
    if (mode == 8) goto L100; else goto L101;
  L98:;
#line 314
    Actor_actors[i].state = 13;
    goto L40;
  L99:;
#line 312
    EnemyAI_SetCourse(i, Actor_actors[0].absX, Actor_actors[0].absY);
    goto L98;
  L100:;
#line 317
    Actor_actors[i].state = 13;
    goto L40;
  L101:;
    if (mode == 10) goto L102; else goto L103;
  L102:;
    if (EnemyAI_Rand(8) == 0) goto L105; else goto L104;
  L103:;
#line 326
    Actor_actors[i].state = 13;
    goto L40;
  L104:;
    if (Actor_actors[i].state == 12) goto L107; else goto L106;
  L105:;
#line 320
    EnemyAI_SelectTactic(i, 4);
    goto L104;
  L106:;
    goto L40;
  L107:;
    if (EnemyAI_StepMove(i)) goto L108; else goto L109;
  L108:;
    goto L106;
  L109:;
#line 322
    Actor_actors[i].state = 13;
    goto L108;
}

static void EnemyAI_UpdateEnemies(void) {
    int32_t i;
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
#line 336
    EnemyAI_UpdateOne(i);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

static void EnemyAI_init(void) {
#line 341
    EnemyAI_rng = 77777;
    return;
}

/* Imported Module Items */

typedef struct Items_WorldItem Items_WorldItem;
static const int32_t Items_ItemNone = 0;
static const int32_t Items_ItemShield = 5;
static const int32_t Items_ItemGold = 1;
static const int32_t Items_ItemSword = 4;
static const int32_t Items_ItemKey = 3;
static const int32_t Items_MaxItems = 64;
static const int32_t Items_ItemScroll = 8;
static const int32_t Items_MaxInv = 16;
static const int32_t Items_ItemPotion = 6;
static const int32_t Items_ItemFood = 2;
static const int32_t Items_ItemGem = 7;
typedef struct Items_WorldItem Items_WorldItem;
struct Items_WorldItem {
    int32_t x;
    int32_t y;
    int32_t itemId;
    int active;
};

static void Items_InitItems(void);
static void Items_SpawnItem(int32_t wx, int32_t wy, int32_t id);
static int32_t Items_CheckPickup(int32_t playerX, int32_t playerY);
static void Items_AddToInventory(int32_t id);
static int Items_UseItem(int32_t id);
static int32_t Items_InventoryCount(int32_t id);

int32_t Items_itemCount;
Items_WorldItem Items_items[63 + 1];
int32_t Items_inventory[15 + 1];
static void Items_InitItems(void) {
    int32_t i;
#line 8 "src/Items.mod"
    Items_itemCount = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= (64 - 1)) goto L2; else goto L4;
  L2:;
#line 10
    Items_items[i].active = 0;
#line 11
    Items_items[i].x = 0;
#line 12
    Items_items[i].y = 0;
#line 13
    Items_items[i].itemId = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    i = 0;
    goto L5;
  L5:;
    if (i <= (16 - 1)) goto L6; else goto L8;
  L6:;
#line 16
    Items_inventory[i] = 0;
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 20
    Items_SpawnItem((28 * 16), (28 * 16), 1);
#line 21
    Items_SpawnItem((33 * 16), (28 * 16), 1);
#line 22
    Items_SpawnItem((30 * 16), (34 * 16), 2);
#line 23
    Items_SpawnItem((24 * 16), (24 * 16), 6);
#line 24
    Items_SpawnItem((38 * 16), (24 * 16), 4);
#line 25
    Items_SpawnItem((42 * 16), (10 * 16), 7);
#line 26
    Items_SpawnItem((10 * 16), (9 * 16), 3);
#line 27
    Items_SpawnItem((51 * 16), (50 * 16), 8);
#line 28
    Items_SpawnItem((7 * 16), (45 * 16), 5);
#line 29
    Items_SpawnItem((30 * 16), (15 * 16), 2);
#line 30
    Items_SpawnItem((45 * 16), (30 * 16), 1);
#line 31
    Items_SpawnItem((25 * 16), (40 * 16), 6);
    return;
}

static void Items_SpawnItem(int32_t wx, int32_t wy, int32_t id) {
    (void)wx;
    (void)wy;
    (void)id;
    if (Items_itemCount >= 64) goto L2; else goto L1;
  L1:;
#line 37
    Items_items[Items_itemCount].x = wx;
#line 38
    Items_items[Items_itemCount].y = wy;
#line 39
    Items_items[Items_itemCount].itemId = id;
#line 40
    Items_items[Items_itemCount].active = 1;
#line 41
    (Items_itemCount++);
    return;
  L2:;
    return;
}

static int32_t Items_CheckPickup(int32_t playerX, int32_t playerY) {
    (void)playerX;
    (void)playerY;
    int32_t i;
    int32_t dx;
    int32_t dy;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Items_itemCount - 1)) goto L2; else goto L4;
  L2:;
    if (Items_items[i].active) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
#line 49
    dx = (playerX - Items_items[i].x);
#line 50
    dy = (playerY - Items_items[i].y);
    if (dx < 12) goto L11; else goto L7;
  L7:;
    goto L5;
  L8:;
#line 52
    Items_items[i].active = 0;
#line 53
    Items_AddToInventory(Items_items[i].itemId);
    return Items_items[i].itemId;
  L9:;
    if (dy > (-12)) goto L8; else goto L7;
  L10:;
    if (dy < 12) goto L9; else goto L7;
  L11:;
    if (dx > (-12)) goto L10; else goto L7;
}

static void Items_AddToInventory(int32_t id) {
    (void)id;
    if (id > 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 64
    (Items_inventory[id]++);
    goto L1;
  L3:;
    if (id < 16) goto L2; else goto L1;
}

static int Items_UseItem(int32_t id) {
    (void)id;
    if (id > 0) goto L4; else goto L1;
  L1:;
    return 0;
  L2:;
#line 71
    (Items_inventory[id]--);
    return 1;
  L3:;
    if (Items_inventory[id] > 0) goto L2; else goto L1;
  L4:;
    if (id < 16) goto L3; else goto L1;
}

static int32_t Items_InventoryCount(int32_t id) {
    (void)id;
    if (id > 0) goto L3; else goto L1;
  L1:;
    return 0;
  L2:;
    return Items_inventory[id];
  L3:;
    if (id < 16) goto L2; else goto L1;
}

/* Imported Module Menu */

typedef struct Menu_MenuDef Menu_MenuDef;
static const int32_t Menu_MSpells = 11;
static const int32_t Menu_BtnH = 9;
static const int32_t Menu_MMagic = 1;
static const int32_t Menu_MItems = 0;
static const int32_t Menu_MSave = 5;
static const int32_t Menu_MTrade = 14;
static const int32_t Menu_MSell = 10;
static const int32_t Menu_MDo = 15;
static const int32_t Menu_MKeys = 6;
static const int32_t Menu_MFile = 9;
static const int32_t Menu_MHerbs = 13;
static const int32_t Menu_MBuy = 3;
static const int32_t Menu_MHerbSell = 17;
static const int32_t Menu_MGame = 4;
static const int32_t Menu_MaxOpts = 15;
static const int32_t Menu_PanelX = 430;
static const int32_t Menu_MTalk = 2;
static const int32_t Menu_BtnW = 52;
static const int32_t Menu_MScrollBuy = 18;
static const int32_t Menu_MHerbBuy = 16;
static const int32_t Menu_PanelY = 2;
static const int32_t Menu_MGive = 7;
static const int32_t Menu_MStudy = 12;
static const int32_t Menu_MUse = 8;
static const int32_t Menu_MAppleBuy = 19;
typedef struct Menu_MenuDef Menu_MenuDef;
struct Menu_MenuDef {
    char labels[74 + 1];
    int32_t num;
    int32_t color;
    int32_t enabled[14 + 1];
};

static const const char * Menu_TabLabels = "ItemsMagicTalk TradeGame ";
static const const char * Menu_LabItems = "List Take Look Use  Do   ";
static const const char * Menu_LabTalk = "Yell Say  Ask  ";
static const const char * Menu_LabGame = "PauseMusicSoundSave Load Exit ";
static const const char * Menu_LabBuy = "Food ArrowVial Mace SwordBow  Totem";
static const const char * Menu_LabMagic = "StoneJewelVial Orb  TotemRing SkullSpellStudyHerbs";
static const const char * Menu_LabUse = "Dirk Mace SwordBow  Wand LassoShellKey  Sun  Book ";
static const const char * Menu_LabSave = "Save Exit ";
static const const char * Menu_LabKeys = "Gold GreenBlue Red  Grey White";
static const const char * Menu_LabGive = "Gold Book Writ Bone ";
static const const char * Menu_LabFile = "  A    B    C    D    E    F    G    H  ";
static const const char * Menu_LabSell = "AppleGrey ";
static const const char * Menu_LabSpells = "Ward FreezFire Fear LightSanctHarvsHeal ";
static const const char * Menu_LabStudy = "Ward FreezFire Fear LightSanctHarvsHeal ";
static const const char * Menu_LabHerbs = "MandrWolfsMugwtYarroNightBlood";
static const const char * Menu_LabTrade = "Buy  Sell Give ";
static const const char * Menu_LabDo = "Camp Eat  ";
static const const char * Menu_LabHerbBuy = "MandrWolfsMugwtYarroNightBlood";
static const const char * Menu_LabHerbSell = "MandrWolfsMugwtYarroNightBlood";
static const const char * Menu_LabScrollBuy = "Ward FreezFire Fear LightSanctHarvsHeal ";
static const const char * Menu_LabAppleBuy = "Apple";
static void Menu_InitMenuDef(Menu_MenuDef *m, char *lab, uint32_t lab_high, int32_t n, int32_t col);
static void Menu_SetEnabled(Menu_MenuDef *m, int32_t idx, int32_t val);
static void Menu_InitMenus(void);
static void Menu_BuildOptions(void);
static int32_t Menu_StuffFlag(int32_t itemId);
static int32_t Menu_SF(int32_t stuffIdx);
static int32_t Menu_WF(int32_t weapIdx);
static void Menu_SetOptions(void);
static void Menu_GoMenu(int32_t mode);
static void Menu_HandleMenuKey(char ch);

int32_t Menu_optionCount;
int32_t Menu_cmode;
Menu_MenuDef Menu_menus[19 + 1];
int32_t Menu_realOptions[11 + 1];
static void Menu_InitMenuDef(Menu_MenuDef *m, char *lab, uint32_t lab_high, int32_t n, int32_t col) {
    (void)m;
    (void)lab;
    (void)lab_high;
    (void)n;
    (void)col;
    int32_t i;
#line 45 "src/Menu.mod"
    m2_Strings_Assign(lab, (*m).labels, 74);
#line 46
    (*m).num = n;
#line 47
    (*m).color = col;
    i = 0;
    goto L1;
  L1:;
    if (i <= (15 - 1)) goto L2; else goto L4;
  L2:;
#line 49
    (*m).enabled[i] = 0;
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
}

static void Menu_SetEnabled(Menu_MenuDef *m, int32_t idx, int32_t val) {
    (void)m;
    (void)idx;
    (void)val;
    if (idx >= 0) goto L3; else goto L1;
  L1:;
    return;
  L2:;
#line 56
    (*m).enabled[idx] = val;
    goto L1;
  L3:;
    if (idx < 15) goto L2; else goto L1;
}

static void Menu_InitMenus(void) {
    int32_t i;
#line 63
    Menu_cmode = 0;
#line 65
    Menu_InitMenuDef(&Menu_menus[0], "List Take Look Use  Do   ", 25, 10, 6);
#line 66
    Menu_InitMenuDef(&Menu_menus[1], "StoneJewelVial Orb  TotemRing SkullSpellStudyHerbs", 50, 15, 5);
#line 67
    Menu_InitMenuDef(&Menu_menus[2], "Yell Say  Ask  ", 15, 8, 9);
#line 68
    Menu_InitMenuDef(&Menu_menus[3], "Food ArrowVial Mace SwordBow  Totem", 35, 12, 10);
#line 69
    Menu_InitMenuDef(&Menu_menus[4], "PauseMusicSoundSave Load Exit ", 30, 11, 2);
#line 70
    Menu_InitMenuDef(&Menu_menus[5], "Save Exit ", 10, 7, 2);
#line 71
    Menu_InitMenuDef(&Menu_menus[6], "Gold GreenBlue Red  Grey White", 30, 11, 8);
#line 72
    Menu_InitMenuDef(&Menu_menus[7], "Gold Book Writ Bone ", 20, 9, 10);
#line 73
    Menu_InitMenuDef(&Menu_menus[8], "Dirk Mace SwordBow  Wand LassoShellKey  Sun  Book ", 50, 14, 8);
#line 74
    Menu_InitMenuDef(&Menu_menus[9], "  A    B    C    D    E    F    G    H  ", 40, 13, 5);
#line 75
    Menu_InitMenuDef(&Menu_menus[10], "AppleGrey ", 10, 7, 10);
#line 76
    Menu_InitMenuDef(&Menu_menus[11], "Ward FreezFire Fear LightSanctHarvsHeal ", 40, 13, 5);
#line 77
    Menu_InitMenuDef(&Menu_menus[12], "Ward FreezFire Fear LightSanctHarvsHeal ", 40, 13, 5);
#line 78
    Menu_InitMenuDef(&Menu_menus[13], "MandrWolfsMugwtYarroNightBlood", 30, 11, 6);
#line 79
    Menu_InitMenuDef(&Menu_menus[14], "Buy  Sell Give ", 15, 8, 10);
#line 80
    Menu_InitMenuDef(&Menu_menus[15], "Camp Eat  ", 10, 7, 6);
#line 81
    Menu_InitMenuDef(&Menu_menus[16], "MandrWolfsMugwtYarroNightBlood", 30, 11, 10);
#line 82
    Menu_InitMenuDef(&Menu_menus[17], "MandrWolfsMugwtYarroNightBlood", 30, 11, 10);
#line 83
    Menu_InitMenuDef(&Menu_menus[18], "Ward FreezFire Fear LightSanctHarvsHeal ", 40, 13, 10);
#line 84
    Menu_InitMenuDef(&Menu_menus[19], "Apple", 5, 6, 10);
#line 87
    Menu_SetEnabled(&Menu_menus[0], 0, 3);
#line 88
    Menu_SetEnabled(&Menu_menus[0], 1, 2);
#line 89
    Menu_SetEnabled(&Menu_menus[0], 2, 2);
#line 90
    Menu_SetEnabled(&Menu_menus[0], 3, 2);
#line 91
    Menu_SetEnabled(&Menu_menus[0], 4, 2);
    i = 5;
    goto L1;
  L1:;
    if (i <= 9) goto L2; else goto L4;
  L2:;
#line 92
    Menu_SetEnabled(&Menu_menus[0], i, 10);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 95
    Menu_SetEnabled(&Menu_menus[2], 0, 2);
#line 96
    Menu_SetEnabled(&Menu_menus[2], 1, 2);
#line 97
    Menu_SetEnabled(&Menu_menus[2], 2, 3);
#line 98
    Menu_SetEnabled(&Menu_menus[2], 3, 2);
#line 99
    Menu_SetEnabled(&Menu_menus[2], 4, 2);
    i = 5;
    goto L5;
  L5:;
    if (i <= 7) goto L6; else goto L8;
  L6:;
#line 100
    Menu_SetEnabled(&Menu_menus[2], i, 10);
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 103
    Menu_SetEnabled(&Menu_menus[4], 0, 2);
#line 104
    Menu_SetEnabled(&Menu_menus[4], 1, 2);
#line 105
    Menu_SetEnabled(&Menu_menus[4], 2, 2);
#line 106
    Menu_SetEnabled(&Menu_menus[4], 3, 2);
#line 107
    Menu_SetEnabled(&Menu_menus[4], 4, 3);
#line 108
    Menu_SetEnabled(&Menu_menus[4], 5, 6);
#line 109
    Menu_SetEnabled(&Menu_menus[4], 6, 7);
#line 110
    Menu_SetEnabled(&Menu_menus[4], 7, 7);
#line 111
    Menu_SetEnabled(&Menu_menus[4], 8, 10);
#line 112
    Menu_SetEnabled(&Menu_menus[4], 9, 10);
#line 113
    Menu_SetEnabled(&Menu_menus[4], 10, 10);
#line 116
    Menu_SetEnabled(&Menu_menus[3], 0, 2);
#line 117
    Menu_SetEnabled(&Menu_menus[3], 1, 2);
#line 118
    Menu_SetEnabled(&Menu_menus[3], 2, 2);
#line 119
    Menu_SetEnabled(&Menu_menus[3], 3, 3);
#line 120
    Menu_SetEnabled(&Menu_menus[3], 4, 2);
    i = 5;
    goto L9;
  L9:;
    if (i <= 11) goto L10; else goto L12;
  L10:;
#line 121
    Menu_SetEnabled(&Menu_menus[3], i, 10);
    goto L11;
  L11:;
    i = (i + 1);
    goto L9;
  L12:;
#line 124
    Menu_SetEnabled(&Menu_menus[1], 0, 2);
#line 125
    Menu_SetEnabled(&Menu_menus[1], 1, 3);
#line 126
    Menu_SetEnabled(&Menu_menus[1], 2, 2);
#line 127
    Menu_SetEnabled(&Menu_menus[1], 3, 2);
#line 128
    Menu_SetEnabled(&Menu_menus[1], 4, 2);
    i = 5;
    goto L13;
  L13:;
    if (i <= 11) goto L14; else goto L16;
  L14:;
#line 129
    Menu_SetEnabled(&Menu_menus[1], i, 8);
    goto L15;
  L15:;
    i = (i + 1);
    goto L13;
  L16:;
#line 130
    Menu_SetEnabled(&Menu_menus[1], 12, 10);
#line 131
    Menu_SetEnabled(&Menu_menus[1], 13, 10);
#line 132
    Menu_SetEnabled(&Menu_menus[1], 14, 10);
#line 135
    Menu_SetEnabled(&Menu_menus[5], 0, 2);
#line 136
    Menu_SetEnabled(&Menu_menus[5], 1, 2);
#line 137
    Menu_SetEnabled(&Menu_menus[5], 2, 2);
#line 138
    Menu_SetEnabled(&Menu_menus[5], 3, 2);
#line 139
    Menu_SetEnabled(&Menu_menus[5], 4, 2);
#line 140
    Menu_SetEnabled(&Menu_menus[5], 5, 10);
#line 141
    Menu_SetEnabled(&Menu_menus[5], 6, 10);
#line 144
    Menu_SetEnabled(&Menu_menus[6], 0, 2);
#line 145
    Menu_SetEnabled(&Menu_menus[6], 1, 2);
#line 146
    Menu_SetEnabled(&Menu_menus[6], 2, 2);
#line 147
    Menu_SetEnabled(&Menu_menus[6], 3, 2);
#line 148
    Menu_SetEnabled(&Menu_menus[6], 4, 2);
    i = 5;
    goto L17;
  L17:;
    if (i <= 10) goto L18; else goto L20;
  L18:;
#line 149
    Menu_SetEnabled(&Menu_menus[6], i, 10);
    goto L19;
  L19:;
    i = (i + 1);
    goto L17;
  L20:;
#line 152
    Menu_SetEnabled(&Menu_menus[7], 0, 2);
#line 153
    Menu_SetEnabled(&Menu_menus[7], 1, 2);
#line 154
    Menu_SetEnabled(&Menu_menus[7], 2, 2);
#line 155
    Menu_SetEnabled(&Menu_menus[7], 3, 2);
#line 156
    Menu_SetEnabled(&Menu_menus[7], 4, 2);
#line 157
    Menu_SetEnabled(&Menu_menus[7], 5, 10);
    i = 0;
    goto L21;
  L21:;
    if (i <= 11) goto L22; else goto L24;
  L22:;
#line 160
    Menu_SetEnabled(&Menu_menus[8], i, 10);
    goto L23;
  L23:;
    i = (i + 1);
    goto L21;
  L24:;
#line 161
    Menu_SetEnabled(&Menu_menus[8], 9, 0);
    i = 5;
    goto L25;
  L25:;
    if (i <= 12) goto L26; else goto L28;
  L26:;
#line 164
    Menu_SetEnabled(&Menu_menus[9], i, 10);
    goto L27;
  L27:;
    i = (i + 1);
    goto L25;
  L28:;
#line 167
    Menu_SetEnabled(&Menu_menus[10], 5, 8);
#line 168
    Menu_SetEnabled(&Menu_menus[10], 6, 8);
    i = 5;
    goto L29;
  L29:;
    if (i <= 12) goto L30; else goto L32;
  L30:;
#line 171
    Menu_SetEnabled(&Menu_menus[11], i, 0);
    goto L31;
  L31:;
    i = (i + 1);
    goto L29;
  L32:;
    i = 5;
    goto L33;
  L33:;
    if (i <= 12) goto L34; else goto L36;
  L34:;
#line 174
    Menu_SetEnabled(&Menu_menus[12], i, 0);
    goto L35;
  L35:;
    i = (i + 1);
    goto L33;
  L36:;
    i = 5;
    goto L37;
  L37:;
    if (i <= 10) goto L38; else goto L40;
  L38:;
#line 177
    Menu_SetEnabled(&Menu_menus[13], i, 10);
    goto L39;
  L39:;
    i = (i + 1);
    goto L37;
  L40:;
    i = 5;
    goto L41;
  L41:;
    if (i <= 7) goto L42; else goto L44;
  L42:;
#line 180
    Menu_SetEnabled(&Menu_menus[14], i, 10);
    goto L43;
  L43:;
    i = (i + 1);
    goto L41;
  L44:;
    i = 5;
    goto L45;
  L45:;
    if (i <= 6) goto L46; else goto L48;
  L46:;
#line 183
    Menu_SetEnabled(&Menu_menus[15], i, 10);
    goto L47;
  L47:;
    i = (i + 1);
    goto L45;
  L48:;
    i = 5;
    goto L49;
  L49:;
    if (i <= 10) goto L50; else goto L52;
  L50:;
#line 186
    Menu_SetEnabled(&Menu_menus[16], i, 10);
    goto L51;
  L51:;
    i = (i + 1);
    goto L49;
  L52:;
    i = 5;
    goto L53;
  L53:;
    if (i <= 10) goto L54; else goto L56;
  L54:;
#line 187
    Menu_SetEnabled(&Menu_menus[17], i, 8);
    goto L55;
  L55:;
    i = (i + 1);
    goto L53;
  L56:;
    i = 5;
    goto L57;
  L57:;
    if (i <= 12) goto L58; else goto L60;
  L58:;
#line 188
    Menu_SetEnabled(&Menu_menus[18], i, 10);
    goto L59;
  L59:;
    i = (i + 1);
    goto L57;
  L60:;
#line 189
    Menu_SetEnabled(&Menu_menus[19], 5, 10);
#line 191
    Menu_cmode = 0;
#line 192
    Menu_BuildOptions();
    return;
}

static void Menu_BuildOptions(void) {
    int32_t i;
    int32_t j;
    int32_t start;
#line 198
    j = 0;
    if (Menu_cmode == 0) goto L2; else goto L5;
  L1:;
    i = start;
    goto L6;
  L2:;
#line 203
    start = 0;
    goto L1;
  L3:;
#line 205
    start = 5;
    goto L1;
  L4:;
    if (Menu_cmode == 4) goto L2; else goto L3;
  L5:;
    if (Menu_cmode == 2) goto L2; else goto L4;
  L6:;
    if (i <= (Menu_menus[Menu_cmode].num - 1)) goto L7; else goto L9;
  L7:;
    if (Menu_menus[Menu_cmode].enabled[i] != 0) goto L12; else goto L10;
  L8:;
    i = (i + 1);
    goto L6;
  L9:;
#line 215
    Menu_optionCount = j;
    goto L15;
  L10:;
    goto L8;
  L11:;
#line 210
    Menu_realOptions[j] = i;
#line 211
    (j++);
    if (j > 11) goto L14; else goto L13;
  L12:;
    if (((uint32_t)(((uint32_t)(Menu_menus[Menu_cmode].enabled[i]))) & (uint32_t)(2)) != 0) goto L11; else goto L10;
  L13:;
    goto L10;
  L14:;
#line 212
    i = Menu_menus[Menu_cmode].num;
    goto L13;
  L15:;
    if (j <= 11) goto L16; else goto L17;
  L16:;
#line 217
    Menu_realOptions[j] = (-1);
#line 218
    (j++);
    goto L15;
  L17:;
    return;
}

static int32_t Menu_StuffFlag(int32_t itemId) {
    (void)itemId;
    if (Items_InventoryCount(itemId) > 0) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return 10;
  L3:;
    return 8;
}

static int32_t Menu_SF(int32_t stuffIdx) {
    (void)stuffIdx;
    if (Brothers_HasStuff(stuffIdx)) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return 10;
  L3:;
    return 8;
}

static int32_t Menu_WF(int32_t weapIdx) {
    (void)weapIdx;
    if (Brothers_HasWeapon(weapIdx)) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return 10;
  L3:;
    return 8;
}

static void Menu_SetOptions(void) {
    int32_t i;
    int32_t j;
#line 249
    Menu_menus[8].enabled[5] = Menu_WF(1);
#line 250
    Menu_menus[8].enabled[6] = Menu_WF(2);
#line 251
    Menu_menus[8].enabled[7] = Menu_WF(3);
#line 252
    Menu_menus[8].enabled[8] = Menu_WF(4);
#line 253
    Menu_menus[8].enabled[9] = Menu_WF(5);
#line 254
    Menu_menus[8].enabled[10] = Menu_SF(5);
#line 255
    Menu_menus[8].enabled[11] = Menu_SF(6);
#line 257
    j = 8;
    i = 0;
    goto L1;
  L1:;
    if (i <= 5) goto L2; else goto L4;
  L2:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[(16 + i)] > 0) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 261
    Menu_menus[8].enabled[12] = j;
#line 262
    Menu_menus[8].enabled[13] = Menu_SF(7);
    i = 0;
    goto L7;
  L5:;
    goto L3;
  L6:;
#line 259
    j = 10;
    goto L5;
  L7:;
    if (i <= 6) goto L8; else goto L10;
  L8:;
#line 266
    Menu_menus[1].enabled[(i + 5)] = Menu_SF((i + 9));
    goto L9;
  L9:;
    i = (i + 1);
    goto L7;
  L10:;
#line 268
    Menu_menus[1].enabled[12] = 10;
#line 269
    Menu_menus[1].enabled[13] = 10;
#line 270
    Menu_menus[1].enabled[14] = 10;
    i = 0;
    goto L11;
  L11:;
    if (i <= 7) goto L12; else goto L14;
  L12:;
    if (Brothers_HasStuff((37 + i))) goto L16; else goto L17;
  L13:;
    i = (i + 1);
    goto L11;
  L14:;
    i = 0;
    goto L18;
  L15:;
    goto L13;
  L16:;
#line 274
    Menu_menus[11].enabled[(5 + i)] = 10;
#line 275
    Menu_menus[12].enabled[(5 + i)] = 10;
    goto L15;
  L17:;
#line 277
    Menu_menus[11].enabled[(5 + i)] = 0;
#line 278
    Menu_menus[12].enabled[(5 + i)] = 0;
    goto L15;
  L18:;
    if (i <= 5) goto L19; else goto L21;
  L19:;
#line 284
    Menu_menus[6].enabled[(i + 5)] = Menu_SF((i + 16));
    goto L20;
  L20:;
    i = (i + 1);
    goto L18;
  L21:;
    if (Brothers_brothers[Brothers_activeBrother].wealth > 2) goto L23; else goto L24;
  L22:;
#line 293
    Menu_menus[7].enabled[6] = Menu_SF(26);
#line 294
    Menu_menus[7].enabled[7] = Menu_SF(28);
#line 295
    Menu_menus[7].enabled[8] = Menu_SF(29);
#line 298
    Menu_menus[10].enabled[5] = Menu_SF(24);
#line 299
    Menu_menus[10].enabled[6] = Menu_SF(20);
#line 302
    Menu_menus[17].enabled[5] = Menu_SF(31);
#line 303
    Menu_menus[17].enabled[6] = Menu_SF(32);
#line 304
    Menu_menus[17].enabled[7] = Menu_SF(33);
#line 305
    Menu_menus[17].enabled[8] = Menu_SF(34);
#line 306
    Menu_menus[17].enabled[9] = Menu_SF(35);
#line 307
    Menu_menus[17].enabled[10] = Menu_SF(36);
#line 309
    Menu_BuildOptions();
    return;
  L23:;
#line 289
    Menu_menus[7].enabled[5] = 10;
    goto L22;
  L24:;
#line 291
    Menu_menus[7].enabled[5] = 8;
    goto L22;
}

static void Menu_GoMenu(int32_t mode) {
    (void)mode;
    if (mode < 0) goto L2; else goto L3;
  L1:;
#line 315
    Menu_cmode = mode;
#line 316
    Menu_SetOptions();
    return;
  L2:;
    return;
  L3:;
    if (mode > 19) goto L2; else goto L1;
}

static void Menu_HandleMenuKey(char ch) {
    (void)ch;
    if ((ch == 'I')) goto L3;
    if ((ch == 'T')) goto L4;
    if ((ch == 'G')) goto L5;
    if ((ch == 'Q')) goto L6;
    if ((ch == 'L')) goto L7;
    if ((ch == 'Y')) goto L8;
    if ((ch == 'A')) goto L9;
    if ((ch == 'U')) goto L10;
    if ((ch == 'B')) goto L11;
    if ((ch == 'D')) goto L12;
    if ((ch == 'K')) goto L13;
    if ((ch == 'V')) goto L14;
    if ((ch == 'X')) goto L15;
    if ((ch == ((char)(27)))) goto L16;
    goto L2;
  L1:;
    return;
  L2:;
    goto L1;
  L3:;
#line 322
    Menu_GoMenu(0);
    goto L1;
  L4:;
#line 323
    Menu_GoMenu(2);
    goto L1;
  L5:;
#line 324
    Menu_GoMenu(14);
    goto L1;
  L6:;
#line 325
    Menu_GoMenu(4);
    goto L1;
  L7:;
#line 326
    Menu_GoMenu(4);
    goto L1;
  L8:;
#line 327
    Menu_GoMenu(2);
    goto L1;
  L9:;
#line 328
    Menu_GoMenu(2);
    goto L1;
  L10:;
#line 329
    Menu_GoMenu(8);
    goto L1;
  L11:;
#line 330
    Menu_GoMenu(14);
    goto L1;
  L12:;
#line 331
    Menu_GoMenu(15);
    goto L1;
  L13:;
#line 332
    Menu_GoMenu(6);
    goto L1;
  L14:;
#line 333
    Menu_GoMenu(5);
    goto L1;
  L15:;
#line 334
    Menu_GoMenu(5);
    goto L1;
  L16:;
#line 335
    Menu_GoMenu(0);
    goto L1;
}

/* Imported Module Encounter */

typedef struct Encounter_EncounterRec Encounter_EncounterRec;
typedef struct Encounter_ExtentRec Encounter_ExtentRec;
static const int32_t Encounter_MaxEncounter = 11;
static const int32_t Encounter_MaxExtents = 23;
typedef struct Encounter_EncounterRec Encounter_EncounterRec;
struct Encounter_EncounterRec {
    int32_t hitpoints;
    int aggressive;
    int32_t arms;
    int32_t cleverness;
    int32_t treasure;
    int32_t fileId;
};

typedef struct Encounter_ExtentRec Encounter_ExtentRec;
struct Encounter_ExtentRec {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    int32_t etype;
    int32_t v1;
    int32_t v2;
    int32_t v3;
};

static const int32_t Encounter_MaxTry = 10;
static const int32_t Encounter_MaxEncounterActors = 34;
static const int32_t Encounter_EnemySlotStart = 4;
static const int32_t Encounter_MaxEnemies = 30;
static const int32_t Encounter_EncounterChanceMultiplier = 20;
static int32_t Encounter_Rand(int32_t limit);
static void Encounter_SetChart(int32_t i, int32_t hp, int32_t arms, int32_t clev, int32_t treas, int32_t fid, int aggr);
static void Encounter_SetExtent(int32_t i, int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2, int32_t et, int32_t ev1, int32_t ev2, int32_t ev3);
static void Encounter_InitCharts(void);
static void Encounter_InitWeaponProbs(void);
static void Encounter_InitExtents(void);
static void Encounter_InitSpawnDirs(void);
static void Encounter_SetupEnemy(int32_t idx, int32_t race, int32_t x, int32_t y);
static void Encounter_ClearEnemySlots(void);
static int32_t Encounter_CountLivingEnemies(void);
static int32_t Encounter_FindFreeSlot(void);
static void Encounter_PlaceEncounter(int32_t heroX, int32_t heroY);
static int32_t Encounter_FindExtent(int32_t heroX, int32_t heroY);
static int Encounter_ActorsOnScreen(int32_t heroX, int32_t heroY);
static int Encounter_EnemiesNearby(int32_t heroX, int32_t heroY);
static void Encounter_SpawnGroup(int32_t heroX, int32_t heroY, int32_t race, int32_t count, int32_t spread);
static void Encounter_UpdateEncounters(int32_t heroX, int32_t heroY, int32_t region);
static void Encounter_InitEncounters(void);
static void Encounter_MoveExtent(int32_t extIdx, int32_t x, int32_t y);

int32_t Encounter_curExtent;
int32_t Encounter_xtype;
Encounter_EncounterRec Encounter_chart[10 + 1];
Encounter_ExtentRec Encounter_extents[22 + 1];
int32_t Encounter_weaponProbs[31 + 1];
int Encounter_loadPending;
int32_t Encounter_pendingRace;
int32_t Encounter_pendingCount;
int32_t Encounter_pendingMix;
int32_t Encounter_tick;
int32_t Encounter_prevXtype;
int32_t Encounter_rngState;
int32_t Encounter_spawnDirX[7 + 1];
int32_t Encounter_spawnDirY[7 + 1];
static int32_t Encounter_Rand(int32_t limit) {
    (void)limit;
#line 62 "src/Encounter.mod"
    Encounter_rngState = ((Encounter_rngState * 1103515245) + 12345);
    if (limit <= 0) goto L2; else goto L1;
  L1:;
    return m2_mod(((int32_t)(((uint32_t)(((uint32_t)(m2_div(Encounter_rngState, 65536)))) & (uint32_t)(32767)))), limit);
  L2:;
    return 0;
}

static void Encounter_SetChart(int32_t i, int32_t hp, int32_t arms, int32_t clev, int32_t treas, int32_t fid, int aggr) {
    (void)i;
    (void)hp;
    (void)arms;
    (void)clev;
    (void)treas;
    (void)fid;
    (void)aggr;
#line 71
    Encounter_chart[i].hitpoints = hp;
#line 72
    Encounter_chart[i].aggressive = aggr;
#line 73
    Encounter_chart[i].arms = arms;
#line 74
    Encounter_chart[i].cleverness = clev;
#line 75
    Encounter_chart[i].treasure = treas;
#line 76
    Encounter_chart[i].fileId = fid;
    return;
}

static void Encounter_SetExtent(int32_t i, int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2, int32_t et, int32_t ev1, int32_t ev2, int32_t ev3) {
    (void)i;
    (void)ax1;
    (void)ay1;
    (void)ax2;
    (void)ay2;
    (void)et;
    (void)ev1;
    (void)ev2;
    (void)ev3;
#line 81
    Encounter_extents[i].x1 = ax1;
    Encounter_extents[i].y1 = ay1;
#line 82
    Encounter_extents[i].x2 = ax2;
    Encounter_extents[i].y2 = ay2;
#line 83
    Encounter_extents[i].etype = et;
#line 84
    Encounter_extents[i].v1 = ev1;
#line 85
    Encounter_extents[i].v2 = ev2;
#line 86
    Encounter_extents[i].v3 = ev3;
    return;
}

static void Encounter_InitCharts(void) {
#line 91
    Encounter_SetChart(0, 18, 2, 0, 2, 6, 1);
#line 92
    Encounter_SetChart(1, 12, 4, 1, 1, 6, 1);
#line 93
    Encounter_SetChart(2, 16, 6, 1, 4, 7, 1);
#line 94
    Encounter_SetChart(3, 8, 3, 0, 3, 7, 1);
#line 95
    Encounter_SetChart(4, 16, 6, 1, 0, 8, 1);
#line 96
    Encounter_SetChart(5, 9, 3, 0, 0, 7, 1);
#line 97
    Encounter_SetChart(6, 10, 6, 1, 0, 8, 1);
#line 98
    Encounter_SetChart(7, 40, 7, 1, 0, 8, 1);
#line 99
    Encounter_SetChart(8, 12, 6, 1, 0, 9, 1);
#line 100
    Encounter_SetChart(9, 50, 5, 0, 0, 9, 1);
#line 101
    Encounter_SetChart(10, 4, 0, 0, 0, 9, 0);
    return;
}

static void Encounter_InitWeaponProbs(void) {
#line 106
    Encounter_weaponProbs[0] = 0;
    Encounter_weaponProbs[1] = 0;
#line 107
    Encounter_weaponProbs[2] = 0;
    Encounter_weaponProbs[3] = 0;
#line 108
    Encounter_weaponProbs[4] = 1;
    Encounter_weaponProbs[5] = 1;
#line 109
    Encounter_weaponProbs[6] = 1;
    Encounter_weaponProbs[7] = 1;
#line 110
    Encounter_weaponProbs[8] = 1;
    Encounter_weaponProbs[9] = 2;
#line 111
    Encounter_weaponProbs[10] = 1;
    Encounter_weaponProbs[11] = 2;
#line 112
    Encounter_weaponProbs[12] = 1;
    Encounter_weaponProbs[13] = 2;
#line 113
    Encounter_weaponProbs[14] = 3;
    Encounter_weaponProbs[15] = 2;
#line 114
    Encounter_weaponProbs[16] = 4;
    Encounter_weaponProbs[17] = 4;
#line 115
    Encounter_weaponProbs[18] = 3;
    Encounter_weaponProbs[19] = 2;
#line 116
    Encounter_weaponProbs[20] = 5;
    Encounter_weaponProbs[21] = 5;
#line 117
    Encounter_weaponProbs[22] = 5;
    Encounter_weaponProbs[23] = 5;
#line 118
    Encounter_weaponProbs[24] = 8;
    Encounter_weaponProbs[25] = 8;
#line 119
    Encounter_weaponProbs[26] = 8;
    Encounter_weaponProbs[27] = 8;
#line 120
    Encounter_weaponProbs[28] = 3;
    Encounter_weaponProbs[29] = 3;
#line 121
    Encounter_weaponProbs[30] = 3;
    Encounter_weaponProbs[31] = 3;
    return;
}

static void Encounter_InitExtents(void) {
#line 126
    Encounter_SetExtent(0, 2118, 27237, 2618, 27637, 70, 0, 1, 11);
#line 127
    Encounter_SetExtent(1, 0, 0, 0, 0, 70, 0, 1, 5);
#line 128
    Encounter_SetExtent(2, 6749, 34951, 7249, 35351, 70, 0, 1, 10);
#line 129
    Encounter_SetExtent(3, 4063, 34819, 4909, 35125, 53, 4, 1, 6);
#line 130
    Encounter_SetExtent(4, 9563, 33883, 10144, 34462, 60, 1, 1, 9);
#line 131
    Encounter_SetExtent(5, 22945, 5597, 23225, 5747, 61, 3, 2, 4);
#line 132
    Encounter_SetExtent(6, 10820, 35646, 10877, 35670, 83, 1, 1, 0);
#line 133
    Encounter_SetExtent(7, 19596, 17123, 19974, 17401, 48, 8, 8, 2);
#line 134
    Encounter_SetExtent(8, 19400, 17034, 20240, 17484, 80, 4, 20, 0);
#line 135
    Encounter_SetExtent(9, 9216, 33280, 12544, 35328, 52, 3, 1, 8);
#line 136
    Encounter_SetExtent(10, 5272, 33300, 6112, 34200, 81, 0, 1, 0);
#line 137
    Encounter_SetExtent(11, 11712, 37350, 12416, 38020, 82, 0, 1, 0);
#line 138
    Encounter_SetExtent(12, 2752, 33300, 8632, 35400, 80, 0, 1, 0);
#line 139
    Encounter_SetExtent(13, 10032, 35550, 12976, 40270, 80, 0, 1, 0);
#line 140
    Encounter_SetExtent(14, 4712, 38100, 10032, 40350, 80, 0, 1, 0);
#line 141
    Encounter_SetExtent(15, 21405, 25583, 21827, 26028, 60, 1, 1, 7);
#line 142
    Encounter_SetExtent(16, 6156, 12755, 12316, 15905, 7, 1, 8, 0);
#line 143
    Encounter_SetExtent(17, 5140, 34860, 6260, 37260, 8, 1, 8, 0);
#line 144
    Encounter_SetExtent(18, 660, 33510, 2060, 34560, 8, 1, 8, 0);
#line 145
    Encounter_SetExtent(19, 18687, 15338, 19211, 16136, 80, 0, 1, 0);
#line 146
    Encounter_SetExtent(20, 16953, 17484, 20240, 18719, 3, 1, 3, 0);
#line 147
    Encounter_SetExtent(21, 20593, 18719, 23113, 22769, 3, 1, 3, 0);
#line 148
    Encounter_SetExtent(22, 0, 0, 32767, 40959, 3, 1, 8, 0);
    return;
}

static void Encounter_InitSpawnDirs(void) {
#line 153
    Encounter_spawnDirX[0] = 0;
    Encounter_spawnDirY[0] = (-2);
#line 154
    Encounter_spawnDirX[1] = 2;
    Encounter_spawnDirY[1] = (-2);
#line 155
    Encounter_spawnDirX[2] = 2;
    Encounter_spawnDirY[2] = 0;
#line 156
    Encounter_spawnDirX[3] = 2;
    Encounter_spawnDirY[3] = 2;
#line 157
    Encounter_spawnDirX[4] = 0;
    Encounter_spawnDirY[4] = 2;
#line 158
    Encounter_spawnDirX[5] = (-2);
    Encounter_spawnDirY[5] = 2;
#line 159
    Encounter_spawnDirX[6] = (-2);
    Encounter_spawnDirY[6] = 0;
#line 160
    Encounter_spawnDirX[7] = (-2);
    Encounter_spawnDirY[7] = (-2);
    return;
}

static void Encounter_SetupEnemy(int32_t idx, int32_t race, int32_t x, int32_t y) {
    (void)idx;
    (void)race;
    (void)x;
    (void)y;
    int32_t wt;
    int32_t w;
#line 168
    Actor_actors[idx].absX = x;
#line 169
    Actor_actors[idx].absY = y;
#line 170
    Actor_actors[idx].actorType = 2;
#line 171
    Actor_actors[idx].race = race;
#line 172
    Actor_actors[idx].state = 13;
#line 173
    Actor_actors[idx].environ = 0;
#line 174
    Actor_actors[idx].facing = 0;
#line 175
    Actor_actors[idx].visible = 1;
#line 176
    Actor_actors[idx].looted = 0;
#line 177
    wt = Encounter_Rand(4);
#line 178
    w = ((Encounter_chart[race].arms * 4) + wt);
    if (w > 31) goto L2; else goto L1;
  L1:;
#line 180
    Actor_actors[idx].weapon = Encounter_weaponProbs[w];
    if (((uint32_t)(((uint32_t)(Actor_actors[idx].weapon))) & (uint32_t)(4)) != 0) goto L4; else goto L5;
  L2:;
#line 179
    w = 31;
    goto L1;
  L3:;
    if (Actor_actors[idx].goal > 4) goto L7; else goto L6;
  L4:;
#line 182
    Actor_actors[idx].goal = (3 + Encounter_chart[race].cleverness);
    goto L3;
  L5:;
#line 184
    Actor_actors[idx].goal = (1 + Encounter_chart[race].cleverness);
    goto L3;
  L6:;
#line 189
    Actor_actors[idx].vitality = Encounter_chart[race].hitpoints;
#line 190
    Actor_actors[idx].tactic = 0;
#line 191
    Actor_actors[idx].velX = 0;
#line 192
    Actor_actors[idx].velY = 0;
    return;
  L7:;
#line 187
    Actor_actors[idx].goal = 4;
    goto L6;
}

static void Encounter_ClearEnemySlots(void) {
    int32_t i;
    i = 4;
    goto L1;
  L1:;
    if (i <= (34 - 1)) goto L2; else goto L4;
  L2:;
    if (i < Actor_actorCount) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
#line 204
    Actor_actors[i].state = 15;
#line 205
    Actor_actors[i].vitality = 0;
#line 206
    Actor_actors[i].visible = 0;
#line 207
    Actor_actors[i].absX = 0;
#line 208
    Actor_actors[i].absY = 0;
    goto L5;
}

static int32_t Encounter_CountLivingEnemies(void) {
    int32_t i;
    int32_t n;
#line 218
    n = 0;
    i = 4;
    goto L1;
  L1:;
    if (i <= (34 - 1)) goto L2; else goto L4;
  L2:;
    if (i < Actor_actorCount) goto L8; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return n;
  L5:;
    goto L3;
  L6:;
#line 223
    (n++);
    goto L5;
  L7:;
    if (Actor_actors[i].state != 15) goto L6; else goto L5;
  L8:;
    if (Actor_actors[i].actorType == 2) goto L7; else goto L5;
}

static int32_t Encounter_FindFreeSlot(void) {
    int32_t i;
    if (Encounter_CountLivingEnemies() >= 30) goto L2; else goto L1;
  L1:;
    i = 4;
    goto L3;
  L2:;
    return (-1);
  L3:;
    if (i <= (34 - 1)) goto L4; else goto L6;
  L4:;
    if (i < Actor_actorCount) goto L10; else goto L7;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
    if (Actor_actorCount < 4) goto L12; else goto L13;
  L7:;
    goto L5;
  L8:;
    return i;
  L9:;
    if (Actor_actors[i].state == 15) goto L8; else goto L7;
  L10:;
    if (Actor_actors[i].actorType == 2) goto L9; else goto L7;
  L11:;
    return (-1);
  L12:;
    return 4;
  L13:;
    if (Actor_actorCount < 34) goto L14; else goto L11;
  L14:;
    return Actor_actorCount;
}

static void Encounter_PlaceEncounter(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t slot;
    int32_t j;
    int32_t k;
    int32_t xtest;
    int32_t ytest;
    int32_t spawned;
    int32_t dir;
    int32_t dist;
    int32_t race;
    int32_t encX;
    int32_t encY;
    if (Encounter_pendingRace < 0) goto L2; else goto L3;
  L1:;
    k = 0;
    goto L4;
  L2:;
#line 258
    Encounter_loadPending = 0;
    return;
  L3:;
    if (Encounter_pendingRace > 10) goto L2; else goto L1;
  L4:;
    if (k <= (10 - 1)) goto L5; else goto L7;
  L5:;
#line 264
    dir = Encounter_Rand(8);
#line 265
    dist = (150 + Encounter_Rand(64));
#line 266
    encX = (heroX + m2_div((Encounter_spawnDirX[dir] * dist), 2));
#line 267
    encY = (heroY + m2_div((Encounter_spawnDirY[dir] * dist), 2));
    if (Assets_currentRegion >= 0) goto L10; else goto L8;
  L6:;
    k = (k + 1);
    goto L4;
  L7:;
    return;
  L8:;
    goto L6;
  L9:;
#line 273
    spawned = 0;
    goto L11;
  L10:;
    if (Assets_GetTerrainAt(encX, encY) == 0) goto L9; else goto L8;
  L11:;
    if (spawned < Encounter_pendingCount) goto L12; else goto L13;
  L12:;
#line 275
    slot = Encounter_FindFreeSlot();
    if (slot < 0) goto L15; else goto L14;
  L13:;
#line 306
    Encounter_loadPending = 0;
    return;
  L14:;
#line 280
    j = 0;
    goto L16;
  L15:;
#line 277
    Encounter_loadPending = 0;
    return;
  L16:;
#line 282
    xtest = ((encX + Encounter_Rand(64)) - 32);
#line 283
    ytest = ((encY + Encounter_Rand(64)) - 32);
    if (Movement_ProxCheck(xtest, ytest, slot) == 0) goto L19; else goto L18;
  L17:;
    if (j < 10) goto L23; else goto L24;
  L18:;
#line 285
    (j++);
    if (j >= 10) goto L21; else goto L20;
  L19:;
    goto L17;
  L20:;
    goto L16;
  L21:;
    goto L17;
  L22:;
    goto L11;
  L23:;
    if (((uint32_t)(((uint32_t)(Encounter_pendingMix))) & (uint32_t)(2)) != 0) goto L28; else goto L27;
  L24:;
#line 302
    Encounter_loadPending = 0;
    return;
  L25:;
#line 297
    Encounter_SetupEnemy(slot, race, xtest, ytest);
    if (slot >= Actor_actorCount) goto L30; else goto L29;
  L26:;
#line 293
    race = (((uint32_t)(((uint32_t)(Encounter_pendingRace))) & (uint32_t)(14)) + Encounter_Rand(2));
    goto L25;
  L27:;
#line 295
    race = Encounter_pendingRace;
    goto L25;
  L28:;
    if (Encounter_pendingRace != 4) goto L26; else goto L27;
  L29:;
#line 299
    (spawned++);
    goto L22;
  L30:;
#line 298
    Actor_actorCount = (slot + 1);
    goto L29;
}

static int32_t Encounter_FindExtent(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t i;
    i = 0;
    goto L1;
  L1:;
    if (i <= (23 - 2)) goto L2; else goto L4;
  L2:;
    if (heroX > Encounter_extents[i].x1) goto L9; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return (23 - 1);
  L5:;
    goto L3;
  L6:;
    return i;
  L7:;
    if (heroY < Encounter_extents[i].y2) goto L6; else goto L5;
  L8:;
    if (heroY > Encounter_extents[i].y1) goto L7; else goto L5;
  L9:;
    if (heroX < Encounter_extents[i].x2) goto L8; else goto L5;
}

static int Encounter_ActorsOnScreen(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t i;
    int32_t dx;
    int32_t dy;
    i = 2;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Actor_actors[i].actorType == 2) goto L6; else goto L7;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
#line 335
    dx = (Actor_actors[i].absX - heroX);
#line 336
    dy = (Actor_actors[i].absY - heroY);
    if (dx < 0) goto L9; else goto L8;
  L7:;
    if (Actor_actors[i].actorType == 4) goto L6; else goto L5;
  L8:;
    if (dy < 0) goto L11; else goto L10;
  L9:;
#line 337
    dx = (-dx);
    goto L8;
  L10:;
    if (dx < 300) goto L14; else goto L12;
  L11:;
#line 338
    dy = (-dy);
    goto L10;
  L12:;
    goto L5;
  L13:;
    return 1;
  L14:;
    if (dy < 300) goto L13; else goto L12;
}

static int Encounter_EnemiesNearby(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t i;
    int32_t dx;
    int32_t dy;
    i = 2;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Actor_actors[i].actorType == 2) goto L8; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
#line 356
    dx = (Actor_actors[i].absX - heroX);
#line 357
    dy = (Actor_actors[i].absY - heroY);
    if (dx < 0) goto L10; else goto L9;
  L7:;
    if (Actor_actors[i].visible) goto L6; else goto L5;
  L8:;
    if (Actor_actors[i].vitality >= 1) goto L7; else goto L5;
  L9:;
    if (dy < 0) goto L12; else goto L11;
  L10:;
#line 358
    dx = (-dx);
    goto L9;
  L11:;
    if (dx < 300) goto L15; else goto L13;
  L12:;
#line 359
    dy = (-dy);
    goto L11;
  L13:;
    goto L5;
  L14:;
    return 1;
  L15:;
    if (dy < 300) goto L14; else goto L13;
}

static void Encounter_SpawnGroup(int32_t heroX, int32_t heroY, int32_t race, int32_t count, int32_t spread) {
    (void)heroX;
    (void)heroY;
    (void)race;
    (void)count;
    (void)spread;
    int32_t slot;
    int32_t j;
    int32_t xtest;
    int32_t ytest;
    int32_t spawned;
    if (race < 0) goto L2; else goto L3;
  L1:;
#line 376
    spawned = 0;
    goto L4;
  L2:;
    return;
  L3:;
    if (race > 10) goto L2; else goto L1;
  L4:;
    if (spawned < count) goto L5; else goto L6;
  L5:;
#line 378
    slot = Encounter_FindFreeSlot();
    if (slot < 0) goto L8; else goto L7;
  L6:;
    return;
  L7:;
#line 380
    j = 0;
    goto L9;
  L8:;
    return;
  L9:;
#line 382
    xtest = ((heroX + Encounter_Rand(spread)) - m2_div(spread, 2));
#line 383
    ytest = ((heroY + Encounter_Rand(spread)) - m2_div(spread, 2));
    if (Movement_ProxCheck(xtest, ytest, slot) == 0) goto L12; else goto L11;
  L10:;
    if (j < 10) goto L16; else goto L15;
  L11:;
#line 385
    (j++);
    if (j >= 10) goto L14; else goto L13;
  L12:;
    goto L10;
  L13:;
    goto L9;
  L14:;
    goto L10;
  L15:;
#line 392
    (spawned++);
    goto L4;
  L16:;
#line 389
    Encounter_SetupEnemy(slot, race, xtest, ytest);
    if (slot >= Actor_actorCount) goto L18; else goto L17;
  L17:;
    goto L15;
  L18:;
#line 390
    Actor_actorCount = (slot + 1);
    goto L17;
}

static void Encounter_UpdateEncounters(int32_t heroX, int32_t heroY, int32_t region) {
    (void)heroX;
    (void)heroY;
    (void)region;
    int32_t ei;
    int32_t et;
    int32_t race;
    int32_t cnt;
    int32_t dangerLevel;
    if (region < 0) goto L2; else goto L1;
  L1:;
#line 402
    (Encounter_tick++);
#line 404
    ei = Encounter_FindExtent(heroX, heroY);
#line 405
    et = Encounter_extents[ei].etype;
#line 406
    Encounter_curExtent = ei;
#line 407
    Encounter_xtype = et;
    if (et < 70) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (et >= 80) goto L6; else goto L5;
  L4:;
#line 411
    Carrier_activeCarrier = 0;
    goto L3;
  L5:;
    if (et == 70) goto L9; else goto L7;
  L6:;
#line 414
    Encounter_prevXtype = et;
    return;
  L7:;
    if (et >= 70) goto L11; else goto L10;
  L8:;
#line 418
    Carrier_SpawnDragon();
#line 419
    Encounter_prevXtype = et;
    return;
  L9:;
    if (Encounter_extents[ei].v3 == 10) goto L8; else goto L7;
  L10:;
    if (Encounter_loadPending) goto L14; else goto L12;
  L11:;
#line 424
    Encounter_prevXtype = et;
    return;
  L12:;
    if (et != Encounter_prevXtype) goto L18; else goto L17;
  L13:;
    if (Encounter_ActorsOnScreen(heroX, heroY)) goto L15; else goto L16;
  L14:;
    if (((uint32_t)(((uint32_t)(Encounter_tick))) & (uint32_t)(15)) == 0) goto L13; else goto L12;
  L15:;
    goto L12;
  L16:;
#line 431
    Encounter_PlaceEncounter(heroX, heroY);
    goto L15;
  L17:;
    if (((uint32_t)(((uint32_t)(Encounter_tick))) & (uint32_t)(31)) != 0) goto L54; else goto L53;
  L18:;
#line 439
    Encounter_prevXtype = et;
    if (et == 60) goto L21; else goto L22;
  L19:;
    if (et == 52) goto L38; else goto L37;
  L20:;
    if (et == 61) goto L27; else goto L26;
  L21:;
    if (Actor_actorCount < 4) goto L20; else goto L23;
  L22:;
    if (et == 61) goto L21; else goto L19;
  L23:;
    if (Actor_actors[3].race != Encounter_extents[ei].v3) goto L20; else goto L19;
  L24:;
    goto L19;
  L25:;
    goto L24;
  L26:;
    if (et == 61) goto L30; else goto L29;
  L27:;
    if (Carrier_turtleEggs) goto L25; else goto L28;
  L28:;
    if (Carrier_turtleEggsDone) goto L25; else goto L26;
  L29:;
#line 447
    Encounter_ClearEnemySlots();
#line 448
    cnt = Encounter_extents[ei].v1;
    if (Encounter_extents[ei].v2 > 0) goto L32; else goto L31;
  L30:;
#line 446
    Carrier_turtleEggs = 1;
    goto L29;
  L31:;
    if (cnt > 30) goto L34; else goto L33;
  L32:;
#line 449
    (cnt += Encounter_Rand(Encounter_extents[ei].v2));
    goto L31;
  L33:;
    if (cnt < 1) goto L36; else goto L35;
  L34:;
#line 450
    cnt = 30;
    goto L33;
  L35:;
#line 452
    Encounter_SpawnGroup(m2_div((Encounter_extents[ei].x1 + Encounter_extents[ei].x2), 2), m2_div((Encounter_extents[ei].y1 + Encounter_extents[ei].y2), 2), Encounter_extents[ei].v3, cnt, 63);
    return;
  L36:;
#line 451
    cnt = 1;
    goto L35;
  L37:;
    if (et == 53) goto L46; else goto L45;
  L38:;
#line 463
    Encounter_pendingRace = 8;
#line 464
    cnt = Encounter_extents[ei].v1;
    if (Encounter_extents[ei].v2 > 0) goto L40; else goto L39;
  L39:;
    if (cnt > 30) goto L42; else goto L41;
  L40:;
#line 465
    (cnt += Encounter_Rand(Encounter_extents[ei].v2));
    goto L39;
  L41:;
    if (cnt < 1) goto L44; else goto L43;
  L42:;
#line 466
    cnt = 30;
    goto L41;
  L43:;
#line 468
    Encounter_pendingCount = cnt;
#line 469
    Encounter_pendingMix = 0;
#line 470
    Encounter_loadPending = 1;
    return;
  L44:;
#line 467
    cnt = 1;
    goto L43;
  L45:;
    goto L17;
  L46:;
#line 475
    Encounter_ClearEnemySlots();
#line 476
    cnt = Encounter_extents[ei].v1;
    if (Encounter_extents[ei].v2 > 0) goto L48; else goto L47;
  L47:;
    if (cnt > 30) goto L50; else goto L49;
  L48:;
#line 477
    (cnt += Encounter_Rand(Encounter_extents[ei].v2));
    goto L47;
  L49:;
    if (cnt < 1) goto L52; else goto L51;
  L50:;
#line 478
    cnt = 30;
    goto L49;
  L51:;
#line 480
    Encounter_SpawnGroup(heroX, heroY, Encounter_extents[ei].v3, cnt, 63);
    return;
  L52:;
#line 479
    cnt = 1;
    goto L51;
  L53:;
    if (Encounter_loadPending) goto L56; else goto L55;
  L54:;
    return;
  L55:;
    if (Encounter_ActorsOnScreen(heroX, heroY)) goto L58; else goto L57;
  L56:;
    return;
  L57:;
    if (Carrier_activeCarrier != 0) goto L60; else goto L59;
  L58:;
    return;
  L59:;
    if (et >= 50) goto L62; else goto L61;
  L60:;
    return;
  L61:;
    if (Encounter_FindFreeSlot() < 0) goto L64; else goto L63;
  L62:;
    return;
  L63:;
    if (region > 7) goto L66; else goto L67;
  L64:;
    return;
  L65:;
#line 513
    dangerLevel = (dangerLevel * 20);
    if (dangerLevel > 63) goto L69; else goto L68;
  L66:;
#line 509
    dangerLevel = (5 + et);
    goto L65;
  L67:;
#line 511
    dangerLevel = (2 + et);
    goto L65;
  L68:;
    if (Encounter_Rand(64) > dangerLevel) goto L71; else goto L70;
  L69:;
#line 514
    dangerLevel = 63;
    goto L68;
  L70:;
#line 521
    cnt = Encounter_Rand(256);
    if (et >= 50) goto L73; else goto L74;
  L71:;
    return;
  L72:;
#line 524
    race = Encounter_Rand(4);
    if (et == 7) goto L77; else goto L75;
  L73:;
#line 522
    cnt = 0;
    goto L72;
  L74:;
    if (((uint32_t)(((uint32_t)(et))) & (uint32_t)(3)) == 0) goto L73; else goto L72;
  L75:;
    if (et == 8) goto L79; else goto L78;
  L76:;
#line 525
    race = 4;
    cnt = 0;
    goto L75;
  L77:;
    if (race == 2) goto L76; else goto L75;
  L78:;
    if (et == 49) goto L81; else goto L80;
  L79:;
#line 526
    race = 6;
    cnt = 0;
    goto L78;
  L80:;
#line 529
    Encounter_pendingMix = cnt;
#line 532
    cnt = Encounter_extents[ei].v1;
    if (Encounter_extents[ei].v2 > 0) goto L83; else goto L82;
  L81:;
#line 527
    race = 2;
    cnt = 0;
    goto L80;
  L82:;
    if (cnt > (30 - Encounter_CountLivingEnemies())) goto L85; else goto L84;
  L83:;
#line 534
    (cnt += Encounter_Rand(Encounter_extents[ei].v2));
    goto L82;
  L84:;
    if (cnt < 1) goto L87; else goto L86;
  L85:;
#line 537
    cnt = (30 - Encounter_CountLivingEnemies());
    goto L84;
  L86:;
#line 541
    Encounter_pendingRace = race;
#line 542
    Encounter_pendingCount = cnt;
#line 543
    Encounter_loadPending = 1;
    return;
  L87:;
    return;
}

static void Encounter_InitEncounters(void) {
#line 548
    Encounter_rngState = Platform_GetTicks();
    if (Encounter_rngState == 0) goto L2; else goto L1;
  L1:;
#line 550
    Encounter_curExtent = (-1);
#line 551
    Encounter_xtype = 0;
#line 552
    Encounter_loadPending = 0;
#line 553
    Encounter_pendingRace = 0;
#line 554
    Encounter_pendingCount = 0;
#line 555
    Encounter_pendingMix = 0;
#line 556
    Encounter_tick = 0;
#line 557
    Encounter_prevXtype = (-1);
#line 558
    Encounter_InitCharts();
#line 559
    Encounter_InitWeaponProbs();
#line 560
    Encounter_InitExtents();
#line 561
    Encounter_InitSpawnDirs();
    return;
  L2:;
#line 549
    Encounter_rngState = 54321;
    goto L1;
}

static void Encounter_MoveExtent(int32_t extIdx, int32_t x, int32_t y) {
    (void)extIdx;
    (void)x;
    (void)y;
    if (extIdx < 0) goto L2; else goto L3;
  L1:;
#line 569
    Encounter_extents[extIdx].x1 = (x - 250);
#line 570
    Encounter_extents[extIdx].y1 = (y - 200);
#line 571
    Encounter_extents[extIdx].x2 = (x + 250);
#line 572
    Encounter_extents[extIdx].y2 = (y + 200);
    return;
  L2:;
    return;
  L3:;
    if (extIdx >= 23) goto L2; else goto L1;
}

/* Imported Module Narration */

static const int32_t Narration_OutTblSize = 29;
static const int32_t Narration_InTblSize = 36;
static void Narration_SetTrigger(int32_t *tab, uint32_t tab_high, int32_t idx, int32_t mn, int32_t mx, int32_t id);
static void Narration_InitOutdoorTriggers(void);
static void Narration_InitIndoorTriggers(void);
static void Narration_InitPlaceMessages(void);
static void Narration_InitInsideMessages(void);
static void Narration_InitEventMessages(void);
static void Narration_FormatMsg(char *src, uint32_t src_high, char *dst, uint32_t dst_high);
static void Narration_DispatchPlace(int32_t id);
static void Narration_DispatchInside(int32_t id);
static void Narration_DispatchEvent(int32_t id);
static int32_t Narration_LookupPlace(int32_t sector, int32_t *tab, uint32_t tab_high, int32_t count);
static void Narration_InitPlace(int32_t heroX, int32_t heroY, int32_t region);
static void Narration_UpdatePlace(int32_t heroX, int32_t heroY, int32_t region);
static void Narration_Event(int32_t n);

int32_t Narration_heroPlace;
int32_t Narration_outTab[86 + 1];
int32_t Narration_inTab[107 + 1];
char Narration_placeMsg[26 + 1][63 + 1];
char Narration_insideMsg[22 + 1][63 + 1];
char Narration_eventMsg[38 + 1][63 + 1];
static void Narration_SetTrigger(int32_t *tab, uint32_t tab_high, int32_t idx, int32_t mn, int32_t mx, int32_t id) {
    (void)tab;
    (void)tab_high;
    (void)idx;
    (void)mn;
    (void)mx;
    (void)id;
#line 32 "src/Narration.mod"
    tab[(idx * 3)] = mn;
#line 33
    tab[((idx * 3) + 1)] = mx;
#line 34
    tab[((idx * 3) + 2)] = id;
    return;
}

static void Narration_InitOutdoorTriggers(void) {
#line 39
    Narration_SetTrigger(Narration_outTab, 86, 0, 51, 51, 19);
#line 40
    Narration_SetTrigger(Narration_outTab, 86, 1, 64, 69, 2);
#line 41
    Narration_SetTrigger(Narration_outTab, 86, 2, 70, 73, 3);
#line 42
    Narration_SetTrigger(Narration_outTab, 86, 3, 80, 95, 6);
#line 43
    Narration_SetTrigger(Narration_outTab, 86, 4, 96, 99, 7);
#line 44
    Narration_SetTrigger(Narration_outTab, 86, 5, 138, 139, 8);
#line 45
    Narration_SetTrigger(Narration_outTab, 86, 6, 144, 144, 9);
#line 46
    Narration_SetTrigger(Narration_outTab, 86, 7, 147, 147, 10);
#line 47
    Narration_SetTrigger(Narration_outTab, 86, 8, 148, 148, 20);
#line 48
    Narration_SetTrigger(Narration_outTab, 86, 9, 159, 162, 17);
#line 49
    Narration_SetTrigger(Narration_outTab, 86, 10, 163, 163, 18);
#line 50
    Narration_SetTrigger(Narration_outTab, 86, 11, 164, 167, 12);
#line 51
    Narration_SetTrigger(Narration_outTab, 86, 12, 168, 168, 21);
#line 52
    Narration_SetTrigger(Narration_outTab, 86, 13, 170, 170, 22);
#line 53
    Narration_SetTrigger(Narration_outTab, 86, 14, 171, 174, 14);
#line 54
    Narration_SetTrigger(Narration_outTab, 86, 15, 176, 176, 13);
#line 55
    Narration_SetTrigger(Narration_outTab, 86, 16, 178, 178, 23);
#line 56
    Narration_SetTrigger(Narration_outTab, 86, 17, 179, 179, 24);
#line 57
    Narration_SetTrigger(Narration_outTab, 86, 18, 180, 180, 25);
#line 58
    Narration_SetTrigger(Narration_outTab, 86, 19, 175, 180, 0);
#line 59
    Narration_SetTrigger(Narration_outTab, 86, 20, 208, 221, 11);
#line 60
    Narration_SetTrigger(Narration_outTab, 86, 21, 243, 243, 16);
#line 61
    Narration_SetTrigger(Narration_outTab, 86, 22, 250, 252, 0);
#line 62
    Narration_SetTrigger(Narration_outTab, 86, 23, 255, 255, 26);
#line 63
    Narration_SetTrigger(Narration_outTab, 86, 24, 78, 78, 4);
#line 64
    Narration_SetTrigger(Narration_outTab, 86, 25, 187, 239, 4);
#line 65
    Narration_SetTrigger(Narration_outTab, 86, 26, 0, 79, 0);
#line 66
    Narration_SetTrigger(Narration_outTab, 86, 27, 185, 254, 15);
#line 67
    Narration_SetTrigger(Narration_outTab, 86, 28, 0, 255, 0);
    return;
}

static void Narration_InitIndoorTriggers(void) {
#line 72
    Narration_SetTrigger(Narration_inTab, 107, 0, 2, 2, 2);
#line 73
    Narration_SetTrigger(Narration_inTab, 107, 1, 7, 7, 3);
#line 74
    Narration_SetTrigger(Narration_inTab, 107, 2, 4, 4, 4);
#line 75
    Narration_SetTrigger(Narration_inTab, 107, 3, 5, 6, 5);
#line 76
    Narration_SetTrigger(Narration_inTab, 107, 4, 9, 10, 6);
#line 77
    Narration_SetTrigger(Narration_inTab, 107, 5, 30, 30, 7);
#line 78
    Narration_SetTrigger(Narration_inTab, 107, 6, 19, 33, 14);
#line 79
    Narration_SetTrigger(Narration_inTab, 107, 7, 101, 101, 14);
#line 80
    Narration_SetTrigger(Narration_inTab, 107, 8, 130, 134, 14);
#line 81
    Narration_SetTrigger(Narration_inTab, 107, 9, 36, 36, 13);
#line 82
    Narration_SetTrigger(Narration_inTab, 107, 10, 37, 42, 12);
#line 83
    Narration_SetTrigger(Narration_inTab, 107, 11, 46, 46, 0);
#line 84
    Narration_SetTrigger(Narration_inTab, 107, 12, 43, 59, 11);
#line 85
    Narration_SetTrigger(Narration_inTab, 107, 13, 100, 100, 11);
#line 86
    Narration_SetTrigger(Narration_inTab, 107, 14, 143, 149, 11);
#line 87
    Narration_SetTrigger(Narration_inTab, 107, 15, 62, 62, 16);
#line 88
    Narration_SetTrigger(Narration_inTab, 107, 16, 65, 66, 18);
#line 89
    Narration_SetTrigger(Narration_inTab, 107, 17, 60, 78, 17);
#line 90
    Narration_SetTrigger(Narration_inTab, 107, 18, 82, 82, 17);
#line 91
    Narration_SetTrigger(Narration_inTab, 107, 19, 86, 87, 17);
#line 92
    Narration_SetTrigger(Narration_inTab, 107, 20, 92, 92, 17);
#line 93
    Narration_SetTrigger(Narration_inTab, 107, 21, 94, 95, 17);
#line 94
    Narration_SetTrigger(Narration_inTab, 107, 22, 97, 99, 17);
#line 95
    Narration_SetTrigger(Narration_inTab, 107, 23, 120, 120, 17);
#line 96
    Narration_SetTrigger(Narration_inTab, 107, 24, 116, 119, 17);
#line 97
    Narration_SetTrigger(Narration_inTab, 107, 25, 139, 141, 17);
#line 98
    Narration_SetTrigger(Narration_inTab, 107, 26, 79, 96, 9);
#line 99
    Narration_SetTrigger(Narration_inTab, 107, 27, 104, 104, 19);
#line 100
    Narration_SetTrigger(Narration_inTab, 107, 28, 114, 114, 20);
#line 101
    Narration_SetTrigger(Narration_inTab, 107, 29, 105, 115, 8);
#line 102
    Narration_SetTrigger(Narration_inTab, 107, 30, 135, 138, 8);
#line 103
    Narration_SetTrigger(Narration_inTab, 107, 31, 125, 125, 21);
#line 104
    Narration_SetTrigger(Narration_inTab, 107, 32, 127, 127, 10);
#line 105
    Narration_SetTrigger(Narration_inTab, 107, 33, 142, 142, 22);
#line 106
    Narration_SetTrigger(Narration_inTab, 107, 34, 121, 129, 22);
#line 107
    Narration_SetTrigger(Narration_inTab, 107, 35, 150, 161, 15);
    return;
}

static void Narration_InitPlaceMessages(void) {
#line 112
    Narration_placeMsg[0][0] = '\0';
#line 113
    Narration_placeMsg[1][0] = '\0';
#line 114
    m2_Strings_Assign("% returned to the village of Tambry.", Narration_placeMsg[2], 63);
#line 115
    m2_Strings_Assign("% came to Vermillion Manor.", Narration_placeMsg[3], 63);
#line 116
    m2_Strings_Assign("% reached the Mountains of Frost.", Narration_placeMsg[4], 63);
#line 117
    m2_Strings_Assign("% reached the Plain of Grief.", Narration_placeMsg[5], 63);
#line 118
    m2_Strings_Assign("% came to the city of Marheim.", Narration_placeMsg[6], 63);
#line 119
    m2_Strings_Assign("% came to the Witch's castle.", Narration_placeMsg[7], 63);
#line 120
    m2_Strings_Assign("% came to the Graveyard.", Narration_placeMsg[8], 63);
#line 121
    m2_Strings_Assign("% came to a great stone ring.", Narration_placeMsg[9], 63);
#line 122
    m2_Strings_Assign("% came to a watchtower.", Narration_placeMsg[10], 63);
#line 123
    m2_Strings_Assign("% traveled to the great Bog.", Narration_placeMsg[11], 63);
#line 124
    m2_Strings_Assign("% came to the Crystal Palace.", Narration_placeMsg[12], 63);
#line 125
    m2_Strings_Assign("% came to mysterious Pixle Grove.", Narration_placeMsg[13], 63);
#line 126
    m2_Strings_Assign("% entered the Citadel of Doom.", Narration_placeMsg[14], 63);
#line 127
    m2_Strings_Assign("% entered the Burning Waste.", Narration_placeMsg[15], 63);
#line 128
    m2_Strings_Assign("% found an oasis.", Narration_placeMsg[16], 63);
#line 129
    m2_Strings_Assign("% came to the hidden city of Azal.", Narration_placeMsg[17], 63);
#line 130
    m2_Strings_Assign("% discovered an outlying fort.", Narration_placeMsg[18], 63);
#line 131
    m2_Strings_Assign("% came to a small keep.", Narration_placeMsg[19], 63);
#line 132
    m2_Strings_Assign("% came to an old castle.", Narration_placeMsg[20], 63);
#line 133
    m2_Strings_Assign("% came to a log cabin.", Narration_placeMsg[21], 63);
#line 134
    m2_Strings_Assign("% came to a dark stone tower.", Narration_placeMsg[22], 63);
#line 135
    m2_Strings_Assign("% came to an isolated cabin.", Narration_placeMsg[23], 63);
#line 136
    m2_Strings_Assign("% came to the Tombs of Hemsath.", Narration_placeMsg[24], 63);
#line 137
    m2_Strings_Assign("% reached the Forbidden Keep.", Narration_placeMsg[25], 63);
#line 138
    m2_Strings_Assign("% found a cave in the hillside.", Narration_placeMsg[26], 63);
    return;
}

static void Narration_InitInsideMessages(void) {
#line 143
    Narration_insideMsg[0][0] = '\0';
#line 144
    Narration_insideMsg[1][0] = '\0';
#line 145
    m2_Strings_Assign("% came to a small chamber.", Narration_insideMsg[2], 63);
#line 146
    m2_Strings_Assign("% came to a large chamber.", Narration_insideMsg[3], 63);
#line 147
    m2_Strings_Assign("% came to a long passageway.", Narration_insideMsg[4], 63);
#line 148
    m2_Strings_Assign("% came to a twisting tunnel.", Narration_insideMsg[5], 63);
#line 149
    m2_Strings_Assign("% came to a forked intersection.", Narration_insideMsg[6], 63);
#line 150
    m2_Strings_Assign("He entered the keep.", Narration_insideMsg[7], 63);
#line 151
    m2_Strings_Assign("He entered the castle.", Narration_insideMsg[8], 63);
#line 152
    m2_Strings_Assign("He entered the castle of King Mar.", Narration_insideMsg[9], 63);
#line 153
    m2_Strings_Assign("He entered the sanctuary of the temple.", Narration_insideMsg[10], 63);
#line 154
    m2_Strings_Assign("% entered the Spirit Plane.", Narration_insideMsg[11], 63);
#line 155
    m2_Strings_Assign("% came to a large room.", Narration_insideMsg[12], 63);
#line 156
    m2_Strings_Assign("% came to an octagonal room.", Narration_insideMsg[13], 63);
#line 157
    m2_Strings_Assign("% traveled along a stone corridor.", Narration_insideMsg[14], 63);
#line 158
    m2_Strings_Assign("% came to a stone maze.", Narration_insideMsg[15], 63);
#line 159
    m2_Strings_Assign("He entered a small building.", Narration_insideMsg[16], 63);
#line 160
    m2_Strings_Assign("He entered the building.", Narration_insideMsg[17], 63);
#line 161
    m2_Strings_Assign("He entered the tavern.", Narration_insideMsg[18], 63);
#line 162
    m2_Strings_Assign("He went inside the inn.", Narration_insideMsg[19], 63);
#line 163
    m2_Strings_Assign("He entered the crypt.", Narration_insideMsg[20], 63);
#line 164
    m2_Strings_Assign("He walked into the cabin.", Narration_insideMsg[21], 63);
#line 165
    m2_Strings_Assign("He unlocked the door and entered.", Narration_insideMsg[22], 63);
    return;
}

static void Narration_InitEventMessages(void) {
#line 172
    m2_Strings_Assign("% was getting rather hungry.", Narration_eventMsg[0], 63);
#line 173
    m2_Strings_Assign("% was getting very hungry.", Narration_eventMsg[1], 63);
#line 174
    m2_Strings_Assign("% was starving!", Narration_eventMsg[2], 63);
#line 175
    m2_Strings_Assign("% was getting tired.", Narration_eventMsg[3], 63);
#line 176
    m2_Strings_Assign("% was getting sleepy.", Narration_eventMsg[4], 63);
#line 178
    m2_Strings_Assign("% was hit and killed!", Narration_eventMsg[5], 63);
#line 179
    m2_Strings_Assign("% was drowned in the water!", Narration_eventMsg[6], 63);
#line 180
    m2_Strings_Assign("% was burned in the lava.", Narration_eventMsg[7], 63);
#line 181
    m2_Strings_Assign("% was turned to stone by the witch.", Narration_eventMsg[8], 63);
#line 182
    m2_Strings_Assign("% started the journey in his home village of Tambry", Narration_eventMsg[9], 63);
#line 184
    m2_Strings_Assign("as had his brother before him.", Narration_eventMsg[10], 63);
#line 185
    m2_Strings_Assign("as had his brothers before him.", Narration_eventMsg[11], 63);
#line 186
    m2_Strings_Assign("% just couldn't stay awake any longer!", Narration_eventMsg[12], 63);
#line 187
    m2_Strings_Assign("% was feeling quite full.", Narration_eventMsg[13], 63);
#line 188
    m2_Strings_Assign("% was feeling quite rested.", Narration_eventMsg[14], 63);
#line 190
    m2_Strings_Assign("Even % would not draw weapon in here.", Narration_eventMsg[15], 63);
#line 191
    m2_Strings_Assign("A calming influence prevents % from drawing.", Narration_eventMsg[16], 63);
#line 192
    m2_Strings_Assign("% picked up a scrap of paper.", Narration_eventMsg[17], 63);
#line 193
    m2_Strings_Assign("It read: Find the turtle!", Narration_eventMsg[18], 63);
#line 194
    m2_Strings_Assign("It read: Meet me at midnight at the Crypt.", Narration_eventMsg[19], 63);
#line 196
    m2_Strings_Assign("% looked around but discovered nothing.", Narration_eventMsg[20], 63);
#line 197
    m2_Strings_Assign("% does not have that item.", Narration_eventMsg[21], 63);
#line 198
    m2_Strings_Assign("% bought some food and ate it.", Narration_eventMsg[22], 63);
#line 199
    m2_Strings_Assign("% bought some arrows.", Narration_eventMsg[23], 63);
#line 200
    m2_Strings_Assign("% passed out from hunger!", Narration_eventMsg[24], 63);
#line 202
    m2_Strings_Assign("% is not sleepy.", Narration_eventMsg[25], 63);
#line 203
    m2_Strings_Assign("% decided to lie down and sleep.", Narration_eventMsg[26], 63);
#line 204
    m2_Strings_Assign("% perished in the hot lava!", Narration_eventMsg[27], 63);
#line 205
    m2_Strings_Assign("It was midnight.", Narration_eventMsg[28], 63);
#line 206
    m2_Strings_Assign("It was morning.", Narration_eventMsg[29], 63);
#line 208
    m2_Strings_Assign("It was midday.", Narration_eventMsg[30], 63);
#line 209
    m2_Strings_Assign("Evening was drawing near.", Narration_eventMsg[31], 63);
#line 210
    m2_Strings_Assign("Ground is too hot for swan to land.", Narration_eventMsg[32], 63);
#line 211
    m2_Strings_Assign("Flying too fast to dismount.", Narration_eventMsg[33], 63);
#line 212
    m2_Strings_Assign("They're all dead! he cried.", Narration_eventMsg[34], 63);
#line 214
    m2_Strings_Assign("No time for that now!", Narration_eventMsg[35], 63);
#line 215
    m2_Strings_Assign("% put an apple away for later.", Narration_eventMsg[36], 63);
#line 216
    m2_Strings_Assign("% ate one of his apples.", Narration_eventMsg[37], 63);
#line 217
    m2_Strings_Assign("% discovered a hidden object.", Narration_eventMsg[38], 63);
    return;
}

static void Narration_FormatMsg(char *src, uint32_t src_high, char *dst, uint32_t dst_high) {
    (void)src;
    (void)src_high;
    (void)dst;
    (void)dst_high;
    int32_t si;
    int32_t di;
    int32_t ni;
    char name[15 + 1];
#line 226
    Brothers_ActiveName(name, 15);
#line 227
    si = 0;
    di = 0;
    goto L1;
  L1:;
    if (si <= src_high) goto L5; else goto L3;
  L2:;
    if (src[si] == '%') goto L7; else goto L8;
  L3:;
    if (di <= dst_high) goto L15; else goto L14;
  L4:;
    if (di < dst_high) goto L2; else goto L3;
  L5:;
    if (src[si] != '\0') goto L4; else goto L3;
  L6:;
    goto L1;
  L7:;
#line 230
    ni = 0;
    goto L9;
  L8:;
#line 237
    dst[di] = src[si];
#line 238
    (di++);
    (si++);
    goto L6;
  L9:;
    if (ni <= 15) goto L13; else goto L11;
  L10:;
#line 232
    dst[di] = name[ni];
#line 233
    (di++);
    (ni++);
    goto L9;
  L11:;
#line 235
    (si++);
    goto L6;
  L12:;
    if (di < dst_high) goto L10; else goto L11;
  L13:;
    if (name[ni] != '\0') goto L12; else goto L11;
  L14:;
    return;
  L15:;
#line 241
    dst[di] = '\0';
    goto L14;
}

static void Narration_DispatchPlace(int32_t id) {
    (void)id;
    char buf[79 + 1];
    if (id < 0) goto L2; else goto L3;
  L1:;
    if (Narration_placeMsg[id][0] == '\0') goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (id > 26) goto L2; else goto L1;
  L4:;
#line 251
    Narration_FormatMsg(Narration_placeMsg[id], 63, buf, 79);
#line 252
    HudLog_AddLogLine(buf, 79);
    return;
  L5:;
    return;
}

static void Narration_DispatchInside(int32_t id) {
    (void)id;
    char buf[79 + 1];
    if (id < 0) goto L2; else goto L3;
  L1:;
    if (Narration_insideMsg[id][0] == '\0') goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (id > 22) goto L2; else goto L1;
  L4:;
#line 260
    Narration_FormatMsg(Narration_insideMsg[id], 63, buf, 79);
#line 261
    HudLog_AddLogLine(buf, 79);
    return;
  L5:;
    return;
}

static void Narration_DispatchEvent(int32_t id) {
    (void)id;
    char buf[79 + 1];
    if (id < 0) goto L2; else goto L3;
  L1:;
    if (Narration_eventMsg[id][0] == '\0') goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (id > 38) goto L2; else goto L1;
  L4:;
#line 269
    Narration_FormatMsg(Narration_eventMsg[id], 63, buf, 79);
#line 270
    HudLog_AddLogLine(buf, 79);
    return;
  L5:;
    return;
}

static int32_t Narration_LookupPlace(int32_t sector, int32_t *tab, uint32_t tab_high, int32_t count) {
    (void)sector;
    (void)tab;
    (void)tab_high;
    (void)count;
    int32_t i;
    int32_t base;
    i = 0;
    goto L1;
  L1:;
    if (i <= (count - 1)) goto L2; else goto L4;
  L2:;
#line 281
    base = (i * 3);
    if (sector >= tab[base]) goto L7; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return 0;
  L5:;
    goto L3;
  L6:;
    return tab[(base + 2)];
  L7:;
    if (sector <= tab[(base + 1)]) goto L6; else goto L5;
}

static void Narration_InitPlace(int32_t heroX, int32_t heroY, int32_t region) {
    (void)heroX;
    (void)heroY;
    (void)region;
    int32_t sector;
    int32_t placeId;
    if (region < 0) goto L2; else goto L1;
  L1:;
#line 295
    sector = ((uint32_t)(((uint32_t)(Assets_GetMapSector(heroX, heroY)))) & (uint32_t)(255));
    if (region > 7) goto L4; else goto L5;
  L2:;
    return;
  L3:;
#line 302
    Narration_heroPlace = placeId;
    return;
  L4:;
#line 297
    placeId = Narration_LookupPlace(sector, Narration_inTab, 107, 36);
    if (placeId > 0) goto L7; else goto L6;
  L5:;
#line 300
    placeId = Narration_LookupPlace(sector, Narration_outTab, 86, 29);
    goto L3;
  L6:;
    goto L3;
  L7:;
#line 298
    (placeId += 256);
    goto L6;
}

static void Narration_UpdatePlace(int32_t heroX, int32_t heroY, int32_t region) {
    (void)heroX;
    (void)heroY;
    (void)region;
    int32_t sector;
    int32_t placeId;
    if (region < 0) goto L2; else goto L1;
  L1:;
#line 309
    sector = ((uint32_t)(((uint32_t)(Assets_GetMapSector(heroX, heroY)))) & (uint32_t)(255));
    if (region > 7) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    if (placeId != 0) goto L18; else goto L16;
  L4:;
#line 312
    placeId = Narration_LookupPlace(sector, Narration_inTab, 107, 36);
    if (placeId > 0) goto L7; else goto L6;
  L5:;
#line 316
    placeId = Narration_LookupPlace(sector, Narration_outTab, 86, 29);
    if (placeId == 4) goto L9; else goto L8;
  L6:;
    goto L3;
  L7:;
#line 314
    (placeId += 256);
    goto L6;
  L8:;
    goto L3;
  L9:;
    if (region > 7) goto L11; else goto L12;
  L10:;
    goto L8;
  L11:;
    goto L10;
  L12:;
    if (((uint32_t)(((uint32_t)(region))) & (uint32_t)(1)) != 0) goto L13; else goto L14;
  L13:;
#line 322
    placeId = 0;
    goto L10;
  L14:;
    if (region > 3) goto L15; else goto L10;
  L15:;
#line 324
    placeId = 5;
    goto L10;
  L16:;
    return;
  L17:;
#line 331
    Narration_heroPlace = placeId;
    if (placeId > 256) goto L20; else goto L21;
  L18:;
    if (placeId != Narration_heroPlace) goto L17; else goto L16;
  L19:;
    goto L16;
  L20:;
#line 333
    Narration_DispatchInside((placeId - 256));
    goto L19;
  L21:;
#line 335
    Narration_DispatchPlace(placeId);
    goto L19;
}

static void Narration_Event(int32_t n) {
    (void)n;
#line 342
    Narration_DispatchEvent(n);
    return;
}

static void Narration_init(void) {
#line 348
    Narration_heroPlace = 0;
#line 349
    Narration_InitOutdoorTriggers();
#line 350
    Narration_InitIndoorTriggers();
#line 351
    Narration_InitPlaceMessages();
#line 352
    Narration_InitInsideMessages();
#line 353
    Narration_InitEventMessages();
    return;
}

/* Imported Module HudFont */

typedef struct HudFont_GlyphRec HudFont_GlyphRec;
typedef struct HudFont_GlyphRec HudFont_GlyphRec;
struct HudFont_GlyphRec {
    int32_t locStart;
    int32_t bitLen;
    int32_t spacing;
};

static const int32_t HudFont_MaxAmberGlyphs = 96;
static const int32_t HudFont_AmberH = 9;
static const int32_t HudFont_TopazW = 8;
static const int32_t HudFont_TopazH = 8;
static const int32_t HudFont_TopazNum = 224;
static const int32_t HudFont_TopazLo = 32;
static const int32_t HudFont_HudW = 640;
static int32_t HudFont_MapX(int32_t hx);
static int32_t HudFont_MapY(int32_t hy);
static int32_t HudFont_MapW(int32_t w);
static int32_t HudFont_MapH(int32_t h);
static int HudFont_LoadAmber(void * ren);
static int HudFont_LoadTopaz(void * ren);
static int HudFont_LoadHudFont(void * ren);
static void HudFont_DrawHudStr(void * ren, char *s, uint32_t s_high, int32_t hx, int32_t hy);
static int32_t HudFont_HudStrWidth(char *s, uint32_t s_high);
static void HudFont_DrawScreenStr(void * ren, char *s, uint32_t s_high, int32_t sx, int32_t sy, int32_t sc);
static void HudFont_SetFontColor(int32_t r, int32_t g, int32_t b);
static void HudFont_ResetFontColor(void);
static int32_t HudFont_ScreenStrWidth(char *s, uint32_t s_high, int32_t sc);
static void HudFont_DrawMenuStr(void * ren, char *s, uint32_t s_high, int32_t hx, int32_t hy, int32_t nchars, int32_t textOff, int32_t fgR, int32_t fgG, int32_t fgB, int32_t bgR, int32_t bgG, int32_t bgB);

void * HudFont_amberTex;
int32_t HudFont_amberLo;
int32_t HudFont_amberNum;
int32_t HudFont_amberBase;
HudFont_GlyphRec HudFont_amberGlyphs[95 + 1];
void * HudFont_topazTex;
int32_t HudFont_scrW;
int32_t HudFont_hudY0;
static int32_t HudFont_MapX(int32_t hx) {
    (void)hx;
    return m2_div((hx * HudFont_scrW), 640);
}

static int32_t HudFont_MapY(int32_t hy) {
    (void)hy;
    return (HudFont_hudY0 + (hy * 3));
}

static int32_t HudFont_MapW(int32_t w) {
    (void)w;
    return m2_div((w * HudFont_scrW), 640);
}

static int32_t HudFont_MapH(int32_t h) {
    (void)h;
    return (h * 3);
}

static int HudFont_LoadAmber(void * ren) {
    (void)ren;
    uint32_t fh;
    int32_t i;
    uint32_t b;
    uint32_t lo;
    uint32_t hi;
    char p[127 + 1];
#line 68 "src/HudFont.mod"
    HudFont_amberTex = NULL;
#line 69
    HudFont_amberNum = 0;
#line 71
    Assets_AssetPath("amber_9.fnt", 11, p, 127);
#line 72
    m2_BinaryIO_OpenRead(p, &fh);
    if (BinaryIO_Done) goto L1; else goto L2;
  L1:;
#line 78
    m2_BinaryIO_ReadByte(fh, &b);
    HudFont_amberLo = ((int32_t)(b));
#line 79
    m2_BinaryIO_ReadByte(fh, &b);
    HudFont_amberNum = ((int32_t)(b));
#line 80
    m2_BinaryIO_ReadByte(fh, &b);
    HudFont_amberBase = ((int32_t)(b));
    if (HudFont_amberNum > 96) goto L4; else goto L3;
  L2:;
#line 74
    m2_WriteString("HudFont: cannot open amber_9.fnt");
    m2_WriteLn();
    return 0;
  L3:;
    i = 0;
    goto L5;
  L4:;
#line 82
    HudFont_amberNum = 96;
    goto L3;
  L5:;
    if (i <= (HudFont_amberNum - 1)) goto L6; else goto L8;
  L6:;
#line 85
    m2_BinaryIO_ReadByte(fh, &lo);
#line 86
    m2_BinaryIO_ReadByte(fh, &hi);
#line 87
    HudFont_amberGlyphs[i].locStart = (((int32_t)(lo)) + (((int32_t)(hi)) * 256));
#line 88
    m2_BinaryIO_ReadByte(fh, &b);
    HudFont_amberGlyphs[i].bitLen = ((int32_t)(b));
#line 89
    m2_BinaryIO_ReadByte(fh, &b);
    HudFont_amberGlyphs[i].spacing = ((int32_t)(b));
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 91
    m2_BinaryIO_Close(fh);
#line 93
    Assets_AssetPath("amber_9.bmp", 11, p, 127);
#line 94
    HudFont_amberTex = Texture_LoadBMPKeyed(ren, p, 127, 0, 0, 0);
    if (HudFont_amberTex == NULL) goto L10; else goto L9;
  L9:;
#line 99
    Texture_SetBlendMode(HudFont_amberTex, 1);
#line 101
    Texture_SetColorMod(HudFont_amberTex, 170, 85, 0);
#line 103
    m2_WriteString("HudFont: amber_9 loaded (");
#line 104
    m2_WriteInt(HudFont_amberNum, 1);
    m2_WriteString(" glyphs)");
    m2_WriteLn();
    return 1;
  L10:;
#line 96
    m2_WriteString("HudFont: cannot load amber_9.bmp");
    m2_WriteLn();
    return 0;
}

static int HudFont_LoadTopaz(void * ren) {
    (void)ren;
    char p[127 + 1];
#line 111
    Assets_AssetPath("topaz_8.bmp", 11, p, 127);
#line 112
    HudFont_topazTex = Texture_LoadBMPKeyed(ren, p, 127, 0, 0, 0);
    if (HudFont_topazTex == NULL) goto L2; else goto L1;
  L1:;
#line 117
    Texture_SetBlendMode(HudFont_topazTex, 1);
#line 118
    m2_WriteString("HudFont: topaz_8 loaded");
    m2_WriteLn();
    return 1;
  L2:;
#line 114
    m2_WriteString("HudFont: cannot load topaz_8.bmp");
    m2_WriteLn();
    return 0;
}

static int HudFont_LoadHudFont(void * ren) {
    (void)ren;
    int ok;
#line 125
    HudFont_scrW = (320 * 3);
#line 126
    HudFont_hudY0 = (143 * 3);
#line 127
    ok = HudFont_LoadAmber(ren);
    if (HudFont_LoadTopaz(ren)) goto L1; else goto L2;
  L1:;
    return ok;
  L2:;
#line 128
    ok = 0;
    goto L1;
}

static void HudFont_DrawHudStr(void * ren, char *s, uint32_t s_high, int32_t hx, int32_t hy) {
    (void)ren;
    (void)s;
    (void)s_high;
    (void)hx;
    (void)hy;
    int32_t i;
    int32_t cx;
    int32_t idx;
    int32_t dw;
    int32_t dh;
    if (HudFont_amberTex == NULL) goto L2; else goto L1;
  L1:;
#line 141
    Texture_SetColorMod(HudFont_amberTex, 170, 85, 0);
#line 142
    cx = hx;
#line 143
    dh = HudFont_MapH(9);
#line 144
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= s_high) goto L6; else goto L5;
  L4:;
#line 146
    idx = (((int32_t)((unsigned char)(s[i]))) - HudFont_amberLo);
    if (idx >= 0) goto L9; else goto L7;
  L5:;
    return;
  L6:;
    if (s[i] != '\0') goto L4; else goto L5;
  L7:;
#line 158
    (i++);
    goto L3;
  L8:;
    if (HudFont_amberGlyphs[idx].bitLen > 0) goto L11; else goto L10;
  L9:;
    if (idx < HudFont_amberNum) goto L8; else goto L7;
  L10:;
#line 156
    (cx += HudFont_amberGlyphs[idx].spacing);
    goto L7;
  L11:;
#line 149
    dw = HudFont_MapW(HudFont_amberGlyphs[idx].bitLen);
    if (dw < 1) goto L13; else goto L12;
  L12:;
#line 151
    Texture_DrawRegion(ren, HudFont_amberTex, HudFont_amberGlyphs[idx].locStart, 0, HudFont_amberGlyphs[idx].bitLen, 9, HudFont_MapX(cx), HudFont_MapY(hy), dw, dh);
    goto L10;
  L13:;
#line 150
    dw = 1;
    goto L12;
}

static int32_t HudFont_HudStrWidth(char *s, uint32_t s_high) {
    (void)s;
    (void)s_high;
    int32_t i;
    int32_t w;
    int32_t idx;
#line 165
    w = 0;
#line 166
    i = 0;
    goto L1;
  L1:;
    if (i <= s_high) goto L4; else goto L3;
  L2:;
#line 168
    idx = (((int32_t)((unsigned char)(s[i]))) - HudFont_amberLo);
    if (idx >= 0) goto L7; else goto L5;
  L3:;
    return w;
  L4:;
    if (s[i] != '\0') goto L2; else goto L3;
  L5:;
#line 172
    (i++);
    goto L1;
  L6:;
#line 170
    (w += HudFont_amberGlyphs[idx].spacing);
    goto L5;
  L7:;
    if (idx < HudFont_amberNum) goto L6; else goto L5;
}

static void HudFont_DrawScreenStr(void * ren, char *s, uint32_t s_high, int32_t sx, int32_t sy, int32_t sc) {
    (void)ren;
    (void)s;
    (void)s_high;
    (void)sx;
    (void)sy;
    (void)sc;
    int32_t i;
    int32_t cx;
    int32_t idx;
    int32_t dw;
    int32_t dh;
    if (HudFont_amberTex == NULL) goto L2; else goto L1;
  L1:;
#line 185
    cx = sx;
#line 186
    dh = (9 * sc);
#line 187
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= s_high) goto L6; else goto L5;
  L4:;
#line 189
    idx = (((int32_t)((unsigned char)(s[i]))) - HudFont_amberLo);
    if (idx >= 0) goto L9; else goto L7;
  L5:;
    return;
  L6:;
    if (s[i] != '\0') goto L4; else goto L5;
  L7:;
#line 200
    (i++);
    goto L3;
  L8:;
    if (HudFont_amberGlyphs[idx].bitLen > 0) goto L11; else goto L10;
  L9:;
    if (idx < HudFont_amberNum) goto L8; else goto L7;
  L10:;
#line 198
    (cx += (HudFont_amberGlyphs[idx].spacing * sc));
    goto L7;
  L11:;
#line 192
    dw = (HudFont_amberGlyphs[idx].bitLen * sc);
#line 193
    Texture_DrawRegion(ren, HudFont_amberTex, HudFont_amberGlyphs[idx].locStart, 0, HudFont_amberGlyphs[idx].bitLen, 9, cx, sy, dw, dh);
    goto L10;
}

static void HudFont_SetFontColor(int32_t r, int32_t g, int32_t b) {
    (void)r;
    (void)g;
    (void)b;
    if (HudFont_amberTex != NULL) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 206
    Texture_SetColorMod(HudFont_amberTex, r, g, b);
    goto L1;
}

static void HudFont_ResetFontColor(void) {
    if (HudFont_amberTex != NULL) goto L2; else goto L1;
  L1:;
    return;
  L2:;
#line 212
    Texture_SetColorMod(HudFont_amberTex, 170, 85, 0);
    goto L1;
}

static int32_t HudFont_ScreenStrWidth(char *s, uint32_t s_high, int32_t sc) {
    (void)s;
    (void)s_high;
    (void)sc;
    int32_t i;
    int32_t w;
    int32_t idx;
#line 218
    w = 0;
#line 219
    i = 0;
    goto L1;
  L1:;
    if (i <= s_high) goto L4; else goto L3;
  L2:;
#line 221
    idx = (((int32_t)((unsigned char)(s[i]))) - HudFont_amberLo);
    if (idx >= 0) goto L7; else goto L5;
  L3:;
    return w;
  L4:;
    if (s[i] != '\0') goto L2; else goto L3;
  L5:;
#line 225
    (i++);
    goto L1;
  L6:;
#line 223
    (w += (HudFont_amberGlyphs[idx].spacing * sc));
    goto L5;
  L7:;
    if (idx < HudFont_amberNum) goto L6; else goto L5;
}

static void HudFont_DrawMenuStr(void * ren, char *s, uint32_t s_high, int32_t hx, int32_t hy, int32_t nchars, int32_t textOff, int32_t fgR, int32_t fgG, int32_t fgB, int32_t bgR, int32_t bgG, int32_t bgB) {
    (void)ren;
    (void)s;
    (void)s_high;
    (void)hx;
    (void)hy;
    (void)nchars;
    (void)textOff;
    (void)fgR;
    (void)fgG;
    (void)fgB;
    (void)bgR;
    (void)bgG;
    (void)bgB;
    int32_t i;
    int32_t cx;
    int32_t idx;
    int32_t dw;
    int32_t dh;
    int32_t bx;
    int32_t bw;
    int32_t bh;
    if (HudFont_topazTex == NULL) goto L2; else goto L1;
  L1:;
#line 240
    bx = HudFont_MapX(hx);
#line 241
    bw = HudFont_MapW((nchars * 8));
#line 242
    dh = HudFont_MapH(8);
#line 243
    bh = dh;
#line 244
    Canvas_SetColor(ren, bgR, bgG, bgB, 255);
#line 245
    Canvas_FillRect(ren, bx, HudFont_MapY(hy), bw, bh);
#line 248
    Texture_SetColorMod(HudFont_topazTex, fgR, fgG, fgB);
#line 249
    cx = (hx + textOff);
#line 250
    dw = HudFont_MapW(8);
#line 251
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= s_high) goto L6; else goto L5;
  L4:;
#line 253
    idx = (((int32_t)((unsigned char)(s[i]))) - 32);
    if (idx >= 0) goto L9; else goto L7;
  L5:;
    return;
  L6:;
    if (s[i] != '\0') goto L4; else goto L5;
  L7:;
#line 259
    (cx += 8);
#line 260
    (i++);
    goto L3;
  L8:;
#line 255
    Texture_DrawRegion(ren, HudFont_topazTex, (idx * 8), 0, 8, 8, HudFont_MapX(cx), HudFont_MapY(hy), dw, dh);
    goto L7;
  L9:;
    if (idx < 224) goto L8; else goto L7;
}

/* Imported Module Quest */

typedef struct Quest_InputState Quest_InputState;
static void Quest_CheckRescue(int32_t heroX, int32_t heroY);
static int Quest_CheckWinCondition(void);
static void Quest_MakePath(int32_t slot, char *path, uint32_t path_high);
static void Quest_MakeLegacyPath(int32_t slot, char *path, uint32_t path_high);
static void Quest_WriteInt4(uint32_t fd, int32_t val);
static void Quest_ReadInt4(uint32_t fd, int32_t *val);
static void Quest_WriteBool4(uint32_t fd, int b);
static void Quest_ReadBool4(uint32_t fd, int *b);
static int Quest_SaveGame(int32_t slot, int32_t savedDayNight, int32_t savedFatigue, int32_t savedHunger, int32_t savedCycle, int32_t savedLightTimer, int32_t savedSecretTimer, int32_t savedFreezeTimer, int32_t savedWardTimer, int32_t savedSanctuaryTimer);
static int Quest_LoadGame(int32_t slot, int32_t *savedDayNight, int32_t *savedFatigue, int32_t *savedHunger, int32_t *savedCycle, int32_t *savedLightTimer, int32_t *savedSecretTimer, int32_t *savedFreezeTimer, int32_t *savedWardTimer, int32_t *savedSanctuaryTimer);
static void Quest_CenterText(char *s, uint32_t s_high, int32_t y, int32_t sc);
static void Quest_ShowWinScreen(void);

int Quest_princessRescued;
int Quest_gameWon;
static void Quest_CheckRescue(int32_t heroX, int32_t heroY) {
    (void)heroX;
    (void)heroY;
    int32_t i;
    if (Quest_princessRescued) goto L2; else goto L1;
  L1:;
    if (Assets_currentRegion != 8) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (heroX > 10820) goto L9; else goto L5;
  L4:;
    return;
  L5:;
    return;
  L6:;
#line 41 "src/Quest.mod"
    Quest_princessRescued = 1;
#line 42
    Brothers_brothers[Brothers_activeBrother].stuff[28] = 1;
#line 43
    Brothers_AddWealth(100);
    i = 16;
    goto L10;
  L7:;
    if (heroY < 35670) goto L6; else goto L5;
  L8:;
    if (heroY > 35646) goto L7; else goto L5;
  L9:;
    if (heroX < 10877) goto L8; else goto L5;
  L10:;
    if (i <= 21) goto L11; else goto L13;
  L11:;
#line 45
    (Brothers_brothers[Brothers_activeBrother].stuff[i] += 3);
    goto L12;
  L12:;
    i = (i + 1);
    goto L10;
  L13:;
#line 47
    HudLog_AddLogLine("You have rescued the princess!", 30);
#line 48
    HudLog_AddLogLine("The king gave you a writ and 100 gold.", 38);
#line 49
    m2_WriteString("Quest: princess rescued!");
    m2_WriteLn();
    goto L5;
}

static int Quest_CheckWinCondition(void) {
    if (Brothers_brothers[Brothers_activeBrother].stuff[22] > 0) goto L2; else goto L1;
  L1:;
    return 0;
  L2:;
    if (Quest_gameWon) goto L3; else goto L4;
  L3:;
    return 1;
  L4:;
#line 59
    Quest_gameWon = 1;
#line 60
    HudLog_AddLogLine("You have recovered the Talisman!", 32);
#line 61
    HudLog_AddLogLine("The quest is complete!", 22);
#line 62
    m2_WriteString("Quest: TALISMAN RECOVERED — YOU WIN!");
    m2_WriteLn();
    goto L3;
}

static void Quest_MakePath(int32_t slot, char *path, uint32_t path_high) {
    (void)slot;
    (void)path;
    (void)path_high;
    if ((slot == 0)) goto L3;
    if ((slot == 1)) goto L4;
    if ((slot == 2)) goto L5;
    if ((slot == 3)) goto L6;
    if ((slot == 4)) goto L7;
    if ((slot == 5)) goto L8;
    if ((slot == 6)) goto L9;
    if ((slot == 7)) goto L10;
    goto L2;
  L1:;
    return;
  L2:;
#line 83
    m2_Strings_Assign("X", path, path_high);
    goto L1;
  L3:;
#line 74
    m2_Strings_Assign("A", path, path_high);
    goto L1;
  L4:;
#line 75
    m2_Strings_Assign("B", path, path_high);
    goto L1;
  L5:;
#line 76
    m2_Strings_Assign("C", path, path_high);
    goto L1;
  L6:;
#line 77
    m2_Strings_Assign("D", path, path_high);
    goto L1;
  L7:;
#line 78
    m2_Strings_Assign("E", path, path_high);
    goto L1;
  L8:;
#line 79
    m2_Strings_Assign("F", path, path_high);
    goto L1;
  L9:;
#line 80
    m2_Strings_Assign("G", path, path_high);
    goto L1;
  L10:;
#line 81
    m2_Strings_Assign("H", path, path_high);
    goto L1;
}

static void Quest_MakeLegacyPath(int32_t slot, char *path, uint32_t path_high) {
    (void)slot;
    (void)path;
    (void)path_high;
#line 89
    m2_Strings_Assign("saves/save_", path, path_high);
    if ((slot == 0)) goto L3;
    if ((slot == 1)) goto L4;
    if ((slot == 2)) goto L5;
    if ((slot == 3)) goto L6;
    if ((slot == 4)) goto L7;
    if ((slot == 5)) goto L8;
    if ((slot == 6)) goto L9;
    if ((slot == 7)) goto L10;
    goto L2;
  L1:;
    return;
  L2:;
#line 100
    m2_Strings_Concat(path, "X.dat", path, path_high);
    goto L1;
  L3:;
#line 91
    m2_Strings_Concat(path, "A.dat", path, path_high);
    goto L1;
  L4:;
#line 92
    m2_Strings_Concat(path, "B.dat", path, path_high);
    goto L1;
  L5:;
#line 93
    m2_Strings_Concat(path, "C.dat", path, path_high);
    goto L1;
  L6:;
#line 94
    m2_Strings_Concat(path, "D.dat", path, path_high);
    goto L1;
  L7:;
#line 95
    m2_Strings_Concat(path, "E.dat", path, path_high);
    goto L1;
  L8:;
#line 96
    m2_Strings_Concat(path, "F.dat", path, path_high);
    goto L1;
  L9:;
#line 97
    m2_Strings_Concat(path, "G.dat", path, path_high);
    goto L1;
  L10:;
#line 98
    m2_Strings_Concat(path, "H.dat", path, path_high);
    goto L1;
}

static void Quest_WriteInt4(uint32_t fd, int32_t val) {
    (void)fd;
    (void)val;
    char buf[3 + 1];
#line 109
    buf[0] = ((char)(((uint32_t)(((uint32_t)(val))) & (uint32_t)(255))));
#line 110
    buf[1] = ((char)(((uint32_t)(((uint32_t)(m2_div(val, 256)))) & (uint32_t)(255))));
#line 111
    buf[2] = ((char)(((uint32_t)(((uint32_t)(m2_div(val, 65536)))) & (uint32_t)(255))));
#line 112
    buf[3] = ((char)(((uint32_t)(((uint32_t)(m2_div(val, 16777216)))) & (uint32_t)(255))));
#line 113
    m2_BinaryIO_WriteBytes(fd, buf, 4);
    return;
}

static void Quest_ReadInt4(uint32_t fd, int32_t *val) {
    (void)fd;
    (void)val;
    char buf[3 + 1];
    uint32_t n;
#line 120
    m2_BinaryIO_ReadBytes(fd, buf, 4, &n);
    if (n < 4) goto L2; else goto L1;
  L1:;
#line 122
    (*val) = (((((int32_t)((unsigned char)(buf[0]))) + (((int32_t)((unsigned char)(buf[1]))) * 256)) + (((int32_t)((unsigned char)(buf[2]))) * 65536)) + (((int32_t)((unsigned char)(buf[3]))) * 16777216));
    return;
  L2:;
#line 121
    (*val) = 0;
    return;
}

static void Quest_WriteBool4(uint32_t fd, int b) {
    (void)fd;
    (void)b;
    if (b) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 128
    Quest_WriteInt4(fd, 1);
    goto L1;
  L3:;
    Quest_WriteInt4(fd, 0);
    goto L1;
}

static void Quest_ReadBool4(uint32_t fd, int *b) {
    (void)fd;
    (void)b;
    int32_t v;
#line 134
    Quest_ReadInt4(fd, &v);
#line 135
    (*b) = v != 0;
    return;
}

static int Quest_SaveGame(int32_t slot, int32_t savedDayNight, int32_t savedFatigue, int32_t savedHunger, int32_t savedCycle, int32_t savedLightTimer, int32_t savedSecretTimer, int32_t savedFreezeTimer, int32_t savedWardTimer, int32_t savedSanctuaryTimer) {
    (void)slot;
    (void)savedDayNight;
    (void)savedFatigue;
    (void)savedHunger;
    (void)savedCycle;
    (void)savedLightTimer;
    (void)savedSecretTimer;
    (void)savedFreezeTimer;
    (void)savedWardTimer;
    (void)savedSanctuaryTimer;
    char path[63 + 1];
    uint32_t fd;
    int32_t i;
    int32_t k;
    int32_t px;
    int32_t py;
    int32_t region;
    char buf[3 + 1];
#line 147
    Quest_MakePath(slot, path, 63);
#line 148
    m2_WriteString("Save: writing to ");
    m2_WriteString(path);
    m2_WriteLn();
#line 149
    m2_BinaryIO_OpenWrite(path, &fd);
    if (fd == 0) goto L2; else goto L1;
  L1:;
#line 157
    buf[0] = 'F';
    buf[1] = 'T';
    buf[2] = 'A';
    buf[3] = '4';
#line 158
    m2_BinaryIO_WriteBytes(fd, buf, 4);
#line 161
    Quest_WriteInt4(fd, Brothers_activeBrother);
    i = 0;
    goto L3;
  L2:;
#line 151
    m2_WriteString("Save: FAILED to open ");
    m2_WriteString(path);
    m2_WriteLn();
#line 152
    HudLog_AddLogLine("Save failed.", 12);
    return 0;
  L3:;
    if (i <= 2) goto L4; else goto L6;
  L4:;
#line 165
    Quest_WriteInt4(fd, Brothers_brothers[i].vitality);
#line 166
    Quest_WriteInt4(fd, Brothers_brothers[i].weapon);
#line 167
    Quest_WriteInt4(fd, Brothers_brothers[i].brave);
#line 168
    Quest_WriteInt4(fd, Brothers_brothers[i].luck);
#line 169
    Quest_WriteInt4(fd, Brothers_brothers[i].kind);
#line 170
    Quest_WriteInt4(fd, Brothers_brothers[i].wealth);
    k = 0;
    goto L7;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
#line 176
    Quest_WriteInt4(fd, Actor_actors[0].absX);
#line 177
    Quest_WriteInt4(fd, Actor_actors[0].absY);
#line 178
    Quest_WriteInt4(fd, Actor_actors[0].weapon);
#line 179
    Quest_WriteInt4(fd, Actor_actors[0].facing);
#line 182
    Quest_WriteInt4(fd, Assets_currentRegion);
#line 185
    Quest_WriteBool4(fd, Quest_princessRescued);
#line 188
    Quest_WriteInt4(fd, Doors_GetUnlockedCount());
    i = 0;
    goto L11;
  L7:;
    if (k <= 44) goto L8; else goto L10;
  L8:;
#line 171
    Quest_WriteInt4(fd, Brothers_brothers[i].stuff[k]);
    goto L9;
  L9:;
    k = (k + 1);
    goto L7;
  L10:;
#line 172
    Quest_WriteBool4(fd, Brothers_brothers[i].alive);
    goto L5;
  L11:;
    if (i <= (Doors_GetUnlockedCount() - 1)) goto L12; else goto L14;
  L12:;
#line 190
    Doors_GetUnlockedDoor(i, &px, &py, &region);
#line 191
    Quest_WriteInt4(fd, px);
#line 192
    Quest_WriteInt4(fd, py);
#line 193
    Quest_WriteInt4(fd, region);
    goto L13;
  L13:;
    i = (i + 1);
    goto L11;
  L14:;
#line 197
    Quest_WriteInt4(fd, savedDayNight);
#line 198
    Quest_WriteInt4(fd, savedFatigue);
#line 199
    Quest_WriteInt4(fd, savedHunger);
#line 200
    Quest_WriteInt4(fd, savedCycle);
#line 201
    Quest_WriteInt4(fd, savedLightTimer);
#line 202
    Quest_WriteInt4(fd, savedSecretTimer);
#line 203
    Quest_WriteInt4(fd, savedFreezeTimer);
#line 204
    Quest_WriteBool4(fd, Quest_gameWon);
#line 207
    Quest_WriteInt4(fd, WorldObj_objCount);
    i = 0;
    goto L15;
  L15:;
    if (i <= (WorldObj_objCount - 1)) goto L16; else goto L18;
  L16:;
#line 209
    Quest_WriteInt4(fd, WorldObj_objects[i].x);
#line 210
    Quest_WriteInt4(fd, WorldObj_objects[i].y);
#line 211
    Quest_WriteInt4(fd, WorldObj_objects[i].objId);
#line 212
    Quest_WriteInt4(fd, WorldObj_objects[i].status);
#line 213
    Quest_WriteInt4(fd, WorldObj_objects[i].region);
    goto L17;
  L17:;
    i = (i + 1);
    goto L15;
  L18:;
    i = 0;
    goto L19;
  L19:;
    if (i <= 9) goto L20; else goto L22;
  L20:;
#line 215
    Quest_WriteBool4(fd, WorldObj_IsRegionDistributed(i));
    goto L21;
  L21:;
    i = (i + 1);
    goto L19;
  L22:;
#line 216
    Quest_WriteInt4(fd, savedWardTimer);
#line 217
    Quest_WriteInt4(fd, savedSanctuaryTimer);
#line 219
    m2_BinaryIO_Close(fd);
#line 220
    HudLog_AddLogLine("Game saved.", 11);
#line 221
    m2_WriteString("Quest: saved to ");
    m2_WriteString(path);
    m2_WriteLn();
    return 1;
}

static int Quest_LoadGame(int32_t slot, int32_t *savedDayNight, int32_t *savedFatigue, int32_t *savedHunger, int32_t *savedCycle, int32_t *savedLightTimer, int32_t *savedSecretTimer, int32_t *savedFreezeTimer, int32_t *savedWardTimer, int32_t *savedSanctuaryTimer) {
    (void)slot;
    (void)savedDayNight;
    (void)savedFatigue;
    (void)savedHunger;
    (void)savedCycle;
    (void)savedLightTimer;
    (void)savedSecretTimer;
    (void)savedFreezeTimer;
    (void)savedWardTimer;
    (void)savedSanctuaryTimer;
    char path[63 + 1];
    uint32_t fd;
    int32_t n;
    int32_t i;
    int32_t v;
    int32_t k;
    int32_t px;
    int32_t py;
    int32_t region;
    int32_t version;
    int distributed;
    char buf[3 + 1];
#line 236
    (*savedWardTimer) = 0;
#line 237
    (*savedSanctuaryTimer) = 0;
#line 238
    Quest_MakePath(slot, path, 63);
#line 239
    m2_BinaryIO_OpenRead(path, &fd);
    if (fd == 0) goto L2; else goto L1;
  L1:;
#line 250
    m2_BinaryIO_ReadBytes(fd, buf, 4, &n);
    if (n < 4) goto L6; else goto L10;
  L2:;
#line 241
    Quest_MakeLegacyPath(slot, path, 63);
#line 242
    m2_BinaryIO_OpenRead(path, &fd);
    if (fd == 0) goto L4; else goto L3;
  L3:;
    goto L1;
  L4:;
#line 244
    HudLog_AddLogLine("No save file found.", 19);
    return 0;
  L5:;
#line 259
    version = (((int32_t)((unsigned char)(buf[3]))) - ((int32_t)((unsigned char)('0'))));
#line 262
    Quest_ReadInt4(fd, &v);
#line 263
    Brothers_activeBrother = v;
    if (Brothers_activeBrother > 2) goto L15; else goto L14;
  L6:;
#line 255
    HudLog_AddLogLine("Invalid save file.", 18);
#line 256
    m2_BinaryIO_Close(fd);
    return 0;
  L7:;
    if (buf[3] != '1') goto L13; else goto L5;
  L8:;
    if (buf[2] != 'A') goto L6; else goto L7;
  L9:;
    if (buf[1] != 'T') goto L6; else goto L8;
  L10:;
    if (buf[0] != 'F') goto L6; else goto L9;
  L11:;
    if (buf[3] != '4') goto L6; else goto L5;
  L12:;
    if (buf[3] != '3') goto L11; else goto L5;
  L13:;
    if (buf[3] != '2') goto L12; else goto L5;
  L14:;
    i = 0;
    goto L16;
  L15:;
#line 264
    Brothers_activeBrother = 0;
    goto L14;
  L16:;
    if (i <= 2) goto L17; else goto L19;
  L17:;
#line 268
    Quest_ReadInt4(fd, &Brothers_brothers[i].vitality);
#line 269
    Quest_ReadInt4(fd, &Brothers_brothers[i].weapon);
#line 270
    Quest_ReadInt4(fd, &Brothers_brothers[i].brave);
#line 271
    Quest_ReadInt4(fd, &Brothers_brothers[i].luck);
#line 272
    Quest_ReadInt4(fd, &Brothers_brothers[i].kind);
#line 273
    Quest_ReadInt4(fd, &Brothers_brothers[i].wealth);
    if (version >= 4) goto L21; else goto L22;
  L18:;
    i = (i + 1);
    goto L16;
  L19:;
#line 289
    Quest_ReadInt4(fd, &Actor_actors[0].absX);
#line 290
    Quest_ReadInt4(fd, &Actor_actors[0].absY);
#line 291
    Quest_ReadInt4(fd, &Actor_actors[0].weapon);
#line 292
    Quest_ReadInt4(fd, &Actor_actors[0].facing);
#line 295
    Quest_ReadInt4(fd, &v);
    if (v >= 0) goto L48; else goto L46;
  L20:;
#line 285
    Quest_ReadBool4(fd, &Brothers_brothers[i].alive);
    goto L18;
  L21:;
    k = 0;
    goto L23;
  L22:;
    if (version >= 3) goto L28; else goto L29;
  L23:;
    if (k <= 44) goto L24; else goto L26;
  L24:;
#line 275
    Quest_ReadInt4(fd, &Brothers_brothers[i].stuff[k]);
    goto L25;
  L25:;
    k = (k + 1);
    goto L23;
  L26:;
    goto L20;
  L27:;
    goto L20;
  L28:;
    k = 0;
    goto L30;
  L29:;
    k = 0;
    goto L38;
  L30:;
    if (k <= 39) goto L31; else goto L33;
  L31:;
#line 278
    Quest_ReadInt4(fd, &Brothers_brothers[i].stuff[k]);
    goto L32;
  L32:;
    k = (k + 1);
    goto L30;
  L33:;
    k = 40;
    goto L34;
  L34:;
    if (k <= 44) goto L35; else goto L37;
  L35:;
#line 279
    Brothers_brothers[i].stuff[k] = 0;
    goto L36;
  L36:;
    k = (k + 1);
    goto L34;
  L37:;
    goto L27;
  L38:;
    if (k <= 34) goto L39; else goto L41;
  L39:;
#line 281
    Quest_ReadInt4(fd, &Brothers_brothers[i].stuff[k]);
    goto L40;
  L40:;
    k = (k + 1);
    goto L38;
  L41:;
    k = 35;
    goto L42;
  L42:;
    if (k <= 44) goto L43; else goto L45;
  L43:;
#line 282
    Brothers_brothers[i].stuff[k] = 0;
    goto L44;
  L44:;
    k = (k + 1);
    goto L42;
  L45:;
    goto L27;
  L46:;
#line 301
    Quest_ReadBool4(fd, &Quest_princessRescued);
#line 304
    Doors_ClearUnlockedDoors();
#line 305
    Quest_ReadInt4(fd, &v);
    if (v < 0) goto L50; else goto L49;
  L47:;
#line 297
    Assets_SwitchRegion(v);
    goto L46;
  L48:;
    if (v <= 9) goto L47; else goto L46;
  L49:;
    if (v > 128) goto L52; else goto L51;
  L50:;
#line 306
    v = 0;
    goto L49;
  L51:;
    i = 0;
    goto L53;
  L52:;
#line 307
    v = 128;
    goto L51;
  L53:;
    if (i <= (v - 1)) goto L54; else goto L56;
  L54:;
#line 309
    Quest_ReadInt4(fd, &px);
#line 310
    Quest_ReadInt4(fd, &py);
#line 311
    Quest_ReadInt4(fd, &region);
#line 312
    Doors_AddUnlockedDoor(px, py, region);
    goto L55;
  L55:;
    i = (i + 1);
    goto L53;
  L56:;
    if (version >= 2) goto L58; else goto L57;
  L57:;
#line 349
    m2_BinaryIO_Close(fd);
#line 351
    Actor_actors[0].state = 13;
#line 352
    Actor_actors[0].vitality = Brothers_brothers[Brothers_activeBrother].vitality;
#line 353
    Actor_actors[0].environ = 0;
#line 354
    Actor_actorCount = 1;
#line 357
    NPC_ResetMaterialized();
#line 360
    v = Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY);
    if (v == 1) goto L75; else goto L76;
  L58:;
#line 316
    Quest_ReadInt4(fd, &(*savedDayNight));
#line 317
    Quest_ReadInt4(fd, &(*savedFatigue));
#line 318
    Quest_ReadInt4(fd, &(*savedHunger));
#line 319
    Quest_ReadInt4(fd, &(*savedCycle));
#line 320
    Quest_ReadInt4(fd, &(*savedLightTimer));
#line 321
    Quest_ReadInt4(fd, &(*savedSecretTimer));
#line 322
    Quest_ReadInt4(fd, &(*savedFreezeTimer));
#line 323
    Quest_ReadBool4(fd, &Quest_gameWon);
#line 325
    Quest_ReadInt4(fd, &v);
    if (v < 0) goto L60; else goto L59;
  L59:;
    if (v > 1600) goto L62; else goto L61;
  L60:;
#line 326
    v = 0;
    goto L59;
  L61:;
#line 328
    WorldObj_objCount = v;
    i = 0;
    goto L63;
  L62:;
#line 327
    v = 1600;
    goto L61;
  L63:;
    if (i <= (WorldObj_objCount - 1)) goto L64; else goto L66;
  L64:;
#line 330
    Quest_ReadInt4(fd, &WorldObj_objects[i].x);
#line 331
    Quest_ReadInt4(fd, &WorldObj_objects[i].y);
#line 332
    Quest_ReadInt4(fd, &WorldObj_objects[i].objId);
#line 333
    Quest_ReadInt4(fd, &WorldObj_objects[i].status);
#line 334
    Quest_ReadInt4(fd, &WorldObj_objects[i].region);
    goto L65;
  L65:;
    i = (i + 1);
    goto L63;
  L66:;
    i = 0;
    goto L67;
  L67:;
    if (i <= 9) goto L68; else goto L70;
  L68:;
#line 337
    Quest_ReadBool4(fd, &distributed);
#line 338
    WorldObj_SetRegionDistributed(i, distributed);
    goto L69;
  L69:;
    i = (i + 1);
    goto L67;
  L70:;
    if (version >= 4) goto L72; else goto L73;
  L71:;
    goto L57;
  L72:;
#line 341
    Quest_ReadInt4(fd, &(*savedWardTimer));
#line 342
    Quest_ReadInt4(fd, &(*savedSanctuaryTimer));
    goto L71;
  L73:;
#line 344
    (*savedWardTimer) = 0;
#line 345
    (*savedSanctuaryTimer) = 0;
    goto L71;
  L74:;
#line 384
    Narration_InitPlace(Actor_actors[0].absX, Actor_actors[0].absY, Assets_currentRegion);
#line 386
    HudLog_AddLogLine("Game loaded.", 12);
#line 387
    m2_WriteString("Quest: loaded from ");
    m2_WriteString(path);
    m2_WriteLn();
    return 1;
  L75:;
    i = 1;
    goto L77;
  L76:;
    if (v >= 10) goto L75; else goto L74;
  L77:;
    if (i <= 16) goto L78; else goto L80;
  L78:;
#line 363
    v = Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY + i));
    if (v != 1) goto L84; else goto L83;
  L79:;
    i = (i + 1);
    goto L77;
  L80:;
    goto L74;
  L81:;
    goto L79;
  L82:;
#line 365
    (Actor_actors[0].absY += i);
    i = 16;
    goto L81;
  L83:;
#line 367
    v = Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY - i));
    if (v != 1) goto L88; else goto L87;
  L84:;
    if (v < 10) goto L82; else goto L83;
  L85:;
    goto L81;
  L86:;
#line 369
    (Actor_actors[0].absY -= i);
    i = 16;
    goto L85;
  L87:;
#line 371
    v = Assets_GetTerrainAt((Actor_actors[0].absX + i), Actor_actors[0].absY);
    if (v != 1) goto L92; else goto L91;
  L88:;
    if (v < 10) goto L86; else goto L87;
  L89:;
    goto L85;
  L90:;
#line 373
    (Actor_actors[0].absX += i);
    i = 16;
    goto L89;
  L91:;
#line 375
    v = Assets_GetTerrainAt((Actor_actors[0].absX - i), Actor_actors[0].absY);
    if (v != 1) goto L95; else goto L93;
  L92:;
    if (v < 10) goto L90; else goto L91;
  L93:;
    goto L89;
  L94:;
#line 377
    (Actor_actors[0].absX -= i);
    i = 16;
    goto L93;
  L95:;
    if (v < 10) goto L94; else goto L93;
}

static void Quest_CenterText(char *s, uint32_t s_high, int32_t y, int32_t sc) {
    (void)s;
    (void)s_high;
    (void)y;
    (void)sc;
    int32_t w;
    int32_t x;
    int32_t sw;
#line 394
    sw = (320 * 3);
#line 395
    w = HudFont_ScreenStrWidth(s, s_high, sc);
#line 396
    x = m2_div((sw - w), 2);
#line 397
    HudFont_DrawScreenStr(Platform_ren, s, s_high, x, y, sc);
    return;
}

static void Quest_ShowWinScreen(void) {
    void * winTex;
    char p[127 + 1];
    int32_t sw;
    int32_t sh;
    int32_t i;
    Platform_InputState inp;
#line 406
    sw = (320 * 3);
#line 407
    sh = ((143 + 57) * 3);
#line 409
    Music_StopMusic();
#line 412
    HudFont_SetFontColor(204, 0, 0);
    i = 1;
    goto L1;
  L1:;
    if (i <= 300) goto L2; else goto L4;
  L2:;
#line 414
    Platform_PollInput(&inp);
#line 415
    Platform_BeginFrame();
#line 416
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 417
    Canvas_Clear(Platform_ren);
#line 418
    Quest_CenterText("The Talisman has been recovered!", 32, m2_div(sh, 4), 2);
#line 419
    Quest_CenterText("The village of Tambry is saved!", 31, (m2_div(sh, 4) + 30), 2);
#line 420
    Quest_CenterText("Congratulations!", 16, m2_div((sh * 2), 4), 3);
#line 421
    Platform_EndFrame();
#line 422
    Platform_DelayMs(33);
    if (inp.attack) goto L6; else goto L7;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 425
    HudFont_ResetFontColor();
#line 428
    m2_Strings_Assign("assets/winpic.bmp", p, 127);
#line 429
    winTex = Platform_LoadBMPTexture(p, 127);
    if (winTex != NULL) goto L9; else goto L8;
  L5:;
    goto L3;
  L6:;
#line 423
    i = 300;
    goto L5;
  L7:;
    if (inp.dirKey != 8) goto L6; else goto L5;
  L8:;
    return;
  L9:;
    i = 1;
    goto L10;
  L10:;
    if (i <= 300) goto L11; else goto L13;
  L11:;
#line 432
    Platform_PollInput(&inp);
#line 433
    Platform_BeginFrame();
#line 434
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 435
    Canvas_Clear(Platform_ren);
#line 436
    Texture_DrawRegion(Platform_ren, winTex, 0, 0, 320, 200, 0, 0, sw, sh);
#line 437
    Platform_EndFrame();
#line 438
    Platform_DelayMs(33);
    if (inp.attack) goto L15; else goto L16;
  L12:;
    i = (i + 1);
    goto L10;
  L13:;
    goto L8;
  L14:;
    goto L12;
  L15:;
#line 439
    i = 300;
    goto L14;
  L16:;
    if (inp.dirKey != 8) goto L15; else goto L14;
}

static void Quest_init(void) {
#line 445
    Quest_princessRescued = 0;
#line 446
    Quest_gameWon = 0;
    return;
}

/* Imported Module GameState */

typedef struct GameState_InputState GameState_InputState;
static const int32_t GameState_ViewInventory = 4;
static const int32_t GameState_NumStoneCircles = 11;
static const int32_t GameState_MsgLen = 255;
static const int32_t GameState_ViewNormal = 0;
static const int32_t GameState_FrameTime = 33;
static const int32_t GameState_ViewBird = 5;
static const int32_t GameState_BuyItems = 7;
static void GameState_InitTreasureProbs(void);
static int32_t GameState_GoldValue(int32_t stuffIdx);
static int32_t GameState_TreasureGroup(int32_t race);
static void GameState_IntToStr(int32_t n, char *buf, uint32_t buf_high);
static void GameState_InitGame(void);
static void GameState_ShowMessage(char *msg, uint32_t msg_high);
static void GameState_ShowInventory(void);
static void GameState_HandleLook(void);
static void GameState_TogglePause(void);
static void GameState_ToggleMusic(void);
static void GameState_WeaponName(int32_t w, char *name, uint32_t name_high);
static void GameState_TreasureName(int32_t ti, char *name, uint32_t name_high);
static int GameState_TakeEnemyWeapon(int32_t enemyIdx, char *name, uint32_t name_high);
static int GameState_TakeEnemyTreasure(int32_t enemyIdx, char *name, uint32_t name_high);
static void GameState_SearchNearbyCorpses(void);
static int32_t GameState_RandContainer(int32_t limit);
static int32_t GameState_PickContainerItem(void);
static void GameState_ContainerLoot(void);
static void GameState_InitBuyTable(void);
static void GameState_HandleBuy(int32_t optIdx);
static void GameState_HandleSell(int32_t optIdx);
static int32_t GameState_HerbPrice(int32_t si);
static int32_t GameState_HerbStuff(int32_t optIdx);
static void GameState_HandleHerbBuy(int32_t optIdx);
static void GameState_HandleHerbSell(int32_t optIdx);
static int32_t GameState_ScrollPrice(int32_t si);
static void GameState_HandleScrollBuy(int32_t optIdx);
static void GameState_HandleAppleBuy(int32_t optIdx);
static void GameState_OpenBuyMenu(void);
static void GameState_OpenSellMenu(void);
static void GameState_HandleGive(int32_t optIdx);
static void GameState_InheritBrotherItems(void);
static void GameState_HandleKeys(int32_t optIdx);
static void GameState_InitStoneList(void);
static void GameState_GetStoneCircle(int32_t i, int32_t *x, int32_t *y);
static void GameState_HandleStoneTeleport(void);
static void GameState_KillWeakEnemies(void);
static void GameState_HandleMagic(int32_t optIdx);
static void GameState_DamageNearbyEnemies(int32_t amount, int32_t radius);
static void GameState_FrightenNearbyEnemies(int32_t radius);
static void GameState_HarvestNearby(void);
static void GameState_HandleSpell(int32_t optIdx);
static void GameState_HandleStudy(int32_t optIdx);
static void GameState_ShowHerbCount(char *name, uint32_t name_high, char *properties, uint32_t properties_high, int32_t stuffIdx);
static void GameState_HandleHerbs(int32_t optIdx);
static void GameState_HandleCamp(void);
static void GameState_HandleEat(void);
static void GameState_HandleMenuClick(int32_t mx, int32_t my);
static void GameState_HandleWorldPickup(void);
static void GameState_HandleTalk(void);
static void GameState_HandleYell(void);
static int32_t GameState_NewXDir(int32_t d);
static int32_t GameState_NewYDir(int32_t d);
static void GameState_UpdateWitch(void);
static void GameState_CheckEnvironment(void);
static void GameState_CheckBedTile(void);
static void GameState_UpdateSleep(void);
static void GameState_UpdateFatigue(void);
static void GameState_BattleAftermath(void);
static int32_t GameState_PointerDirection(int32_t mx, int32_t my);
static void GameState_UpdatePlayer(void);
static void GameState_CheckDoors(void);
static void GameState_UpdateGame(void);

int32_t GameState_witchDir;
int32_t GameState_witchS1;
int32_t GameState_viewStatus;
int32_t GameState_colorPlayTimer;
int32_t GameState_secretTimer;
int32_t GameState_cycle;
int32_t GameState_witchIndex;
int32_t GameState_dayNight;
int32_t GameState_witchS2;
int32_t GameState_msgTimer;
int32_t GameState_regionFade;
int32_t GameState_fairyX;
int GameState_mapToggled;
int GameState_fairyActive;
int GameState_running;
int GameState_witchFlag;
char GameState_msgText[255 + 1];
Platform_InputState GameState_input;
int32_t GameState_potionCooldown;
int32_t GameState_hungerTimer;
int32_t GameState_prevRegion;
int32_t GameState_deathTimer;
int32_t GameState_doorCooldown;
int GameState_battleFlag;
int GameState_prevBattle;
int32_t GameState_dayPeriod;
int GameState_aftermathDone;
int32_t GameState_fatigue;
int32_t GameState_hunger;
int32_t GameState_sleepWait;
int GameState_sleepInBed;
int32_t GameState_containerRng;
int32_t GameState_witchRng;
int GameState_saveMode;
int GameState_cheatGod;
int GameState_cheatSpeed;
int32_t GameState_lightTimer;
int32_t GameState_freezeTimer;
int32_t GameState_sanctuaryTimer;
char GameState_nameBuf[31 + 1];
char GameState_msgBuf[79 + 1];
int32_t GameState_treasureProbs[39 + 1];
int32_t GameState_buyStuff[6 + 1];
int32_t GameState_buyCost[6 + 1];
int32_t GameState_stoneX[10 + 1];
int32_t GameState_stoneY[10 + 1];
static void GameState_InitTreasureProbs(void) {
#line 101 "src/GameState.mod"
    GameState_treasureProbs[0] = 0;
    GameState_treasureProbs[1] = 0;
    GameState_treasureProbs[2] = 0;
#line 102
    GameState_treasureProbs[3] = 0;
    GameState_treasureProbs[4] = 0;
    GameState_treasureProbs[5] = 0;
#line 103
    GameState_treasureProbs[6] = 0;
    GameState_treasureProbs[7] = 0;
#line 104
    GameState_treasureProbs[8] = 9;
    GameState_treasureProbs[9] = 11;
    GameState_treasureProbs[10] = 13;
#line 105
    GameState_treasureProbs[11] = 31;
    GameState_treasureProbs[12] = 31;
    GameState_treasureProbs[13] = 17;
#line 106
    GameState_treasureProbs[14] = 17;
    GameState_treasureProbs[15] = 32;
#line 107
    GameState_treasureProbs[16] = 12;
    GameState_treasureProbs[17] = 14;
    GameState_treasureProbs[18] = 20;
#line 108
    GameState_treasureProbs[19] = 20;
    GameState_treasureProbs[20] = 20;
    GameState_treasureProbs[21] = 31;
#line 109
    GameState_treasureProbs[22] = 33;
    GameState_treasureProbs[23] = 31;
#line 110
    GameState_treasureProbs[24] = 10;
    GameState_treasureProbs[25] = 10;
    GameState_treasureProbs[26] = 16;
#line 111
    GameState_treasureProbs[27] = 16;
    GameState_treasureProbs[28] = 11;
    GameState_treasureProbs[29] = 17;
#line 112
    GameState_treasureProbs[30] = 18;
    GameState_treasureProbs[31] = 19;
#line 113
    GameState_treasureProbs[32] = 15;
    GameState_treasureProbs[33] = 21;
    GameState_treasureProbs[34] = 0;
#line 114
    GameState_treasureProbs[35] = 0;
    GameState_treasureProbs[36] = 0;
    GameState_treasureProbs[37] = 0;
#line 115
    GameState_treasureProbs[38] = 0;
    GameState_treasureProbs[39] = 0;
    return;
}

static int32_t GameState_GoldValue(int32_t stuffIdx) {
    (void)stuffIdx;
    if ((stuffIdx == 31)) goto L3;
    if ((stuffIdx == 32)) goto L4;
    if ((stuffIdx == 33)) goto L5;
    if ((stuffIdx == 34)) goto L6;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 2;
  L4:;
    return 5;
  L5:;
    return 10;
  L6:;
    return 100;
}

static int32_t GameState_TreasureGroup(int32_t race) {
    (void)race;
    if ((race == 0)) goto L3;
    if ((race == 1)) goto L4;
    if ((race == 2)) goto L5;
    if ((race == 3)) goto L6;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 2;
  L4:;
    return 1;
  L5:;
    return 4;
  L6:;
    return 3;
}

static void GameState_IntToStr(int32_t n, char *buf, uint32_t buf_high) {
    (void)n;
    (void)buf;
    (void)buf_high;
    int32_t i;
    int32_t len;
    char tmp[7 + 1];
    if (n < 0) goto L2; else goto L1;
  L1:;
#line 134
    len = 0;
    if (n == 0) goto L4; else goto L5;
  L2:;
    n = 0;
    goto L1;
  L3:;
    i = 0;
    goto L9;
  L4:;
#line 135
    tmp[0] = '0';
    len = 1;
    goto L3;
  L5:;
    goto L6;
  L6:;
    if (n > 0) goto L7; else goto L8;
  L7:;
#line 136
    tmp[len] = ((char)((((int32_t)((unsigned char)('0'))) + m2_mod(n, 10))));
    n = m2_div(n, 10);
    (len++);
    goto L6;
  L8:;
    goto L3;
  L9:;
    if (i <= (len - 1)) goto L10; else goto L12;
  L10:;
#line 138
    buf[i] = tmp[((len - 1) - i)];
    goto L11;
  L11:;
    i = (i + 1);
    goto L9;
  L12:;
#line 139
    buf[len] = '\0';
    return;
}

static void GameState_InitGame(void) {
#line 144
    GameState_running = 1;
#line 145
    GameState_cycle = 0;
#line 146
    GameState_dayNight = 12000;
#line 147
    GameState_msgTimer = 0;
#line 148
    GameState_msgText[0] = '\0';
#line 149
    GameState_potionCooldown = 0;
#line 150
    GameState_hungerTimer = 0;
#line 151
    GameState_deathTimer = 0;
#line 152
    GameState_doorCooldown = 0;
#line 153
    GameState_battleFlag = 0;
#line 154
    GameState_prevBattle = 0;
#line 155
    GameState_viewStatus = 0;
#line 156
    GameState_dayPeriod = 6;
#line 157
    GameState_aftermathDone = 0;
#line 158
    GameState_fatigue = 0;
#line 159
    GameState_hunger = 0;
#line 160
    GameState_sleepWait = 0;
#line 161
    GameState_sleepInBed = 0;
#line 162
    GameState_containerRng = 31337;
#line 163
    GameState_witchRng = 54321;
#line 164
    GameState_saveMode = 0;
#line 165
    GameState_cheatGod = 0;
#line 166
    GameState_cheatSpeed = 0;
#line 167
    Platform_cheatKeys = 0;
#line 168
    GameState_lightTimer = 0;
#line 169
    GameState_secretTimer = 0;
#line 170
    GameState_freezeTimer = 0;
#line 171
    Combat_wardTimer = 0;
#line 172
    GameState_sanctuaryTimer = 0;
#line 173
    GameState_fairyActive = 0;
#line 174
    GameState_fairyX = 0;
#line 175
    GameState_colorPlayTimer = 0;
#line 176
    GameState_witchFlag = 0;
#line 177
    GameState_witchIndex = 0;
#line 178
    GameState_witchDir = 1;
#line 179
    GameState_witchS1 = 0;
    GameState_witchS2 = 0;
#line 181
    World_InitWorld();
#line 182
    Actor_InitAll();
#line 183
    Items_InitItems();
#line 184
    DayNight_InitDayNight();
#line 185
    Brothers_InitBrothers();
#line 186
    Doors_InitDoors();
#line 187
    HudLog_InitHudLog();
#line 188
    NPC_InitNPCs();
#line 189
    Encounter_InitEncounters();
#line 190
    Carrier_InitCarriers();
#line 191
    Missile_InitMissiles();
#line 192
    GameState_InitTreasureProbs();
#line 193
    GameState_InitBuyTable();
#line 194
    GameState_InitStoneList();
#line 196
    Brothers_RestoreBrotherState();
#line 197
    Actor_actorCount = 1;
#line 199
    Assets_InitAssets();
    if (Assets_PreloadAll()) goto L2; else goto L3;
  L1:;
    return;
  L2:;
    if (Assets_LoadHUD((320 * 3), (57 * 3))) goto L4; else goto L5;
  L3:;
#line 213
    GameState_ShowMessage("Welcome! (placeholder mode)", 27);
    goto L1;
  L4:;
#line 204
    Assets_SwitchRegion(3);
#line 205
    WorldObj_DistributeRegion(3);
#line 206
    Actor_actors[0].absX = 19036;
#line 207
    Actor_actors[0].absY = 15755;
#line 208
    Narration_InitPlace(Actor_actors[0].absX, Actor_actors[0].absY, 3);
#line 209
    Narration_Event(9);
#line 210
    Narration_Event(30);
#line 211
    Menu_SetOptions();
    goto L1;
  L5:;
#line 202
    m2_WriteString("*** HUD LOAD FAILED ***");
    m2_WriteLn();
    goto L4;
}

static void GameState_ShowMessage(char *msg, uint32_t msg_high) {
    (void)msg;
    (void)msg_high;
    char buf[255 + 1];
    int32_t si;
    int32_t di;
    int32_t ni;
    char name[15 + 1];
#line 222
    Brothers_ActiveName(name, 15);
#line 223
    si = 0;
    di = 0;
    goto L1;
  L1:;
    if (si <= msg_high) goto L5; else goto L3;
  L2:;
    if (msg[si] == '%') goto L7; else goto L8;
  L3:;
#line 235
    buf[di] = '\0';
#line 236
    m2_Strings_Assign(buf, GameState_msgText, 255);
#line 237
    GameState_msgTimer = 180;
#line 238
    HudLog_AddLogLine(buf, 255);
    return;
  L4:;
    if (di < 255) goto L2; else goto L3;
  L5:;
    if (msg[si] != '\0') goto L4; else goto L3;
  L6:;
    goto L1;
  L7:;
#line 226
    ni = 0;
    goto L9;
  L8:;
#line 232
    buf[di] = msg[si];
    (di++);
    (si++);
    goto L6;
  L9:;
    if (ni <= 15) goto L13; else goto L11;
  L10:;
#line 228
    buf[di] = name[ni];
    (di++);
    (ni++);
    goto L9;
  L11:;
#line 230
    (si++);
    goto L6;
  L12:;
    if (di < 255) goto L10; else goto L11;
  L13:;
    if (name[ni] != '\0') goto L12; else goto L11;
}

static void GameState_ShowInventory(void) {
#line 242
    GameState_viewStatus = 4;
    return;
}

static void GameState_HandleLook(void) {
    int32_t i;
    int32_t dx;
    int32_t dy;
    int32_t found;
#line 247
    found = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= (WorldObj_objCount - 1)) goto L2; else goto L4;
  L2:;
    if (WorldObj_objects[i].status == 0) goto L7; else goto L8;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    if (found > 0) goto L16; else goto L17;
  L5:;
    goto L3;
  L6:;
#line 251
    dx = (Actor_actors[0].absX - WorldObj_objects[i].x);
#line 252
    dy = (Actor_actors[0].absY - WorldObj_objects[i].y);
    if (dx > (-40)) goto L14; else goto L10;
  L7:;
    if (WorldObj_objects[i].region == Assets_currentRegion) goto L6; else goto L9;
  L8:;
    if (WorldObj_objects[i].status == 5) goto L7; else goto L5;
  L9:;
    if (WorldObj_objects[i].region == (-1)) goto L6; else goto L5;
  L10:;
    goto L5;
  L11:;
#line 254
    WorldObj_objects[i].status = 1;
    found = 1;
    goto L10;
  L12:;
    if (dy < 40) goto L11; else goto L10;
  L13:;
    if (dy > (-40)) goto L12; else goto L10;
  L14:;
    if (dx < 40) goto L13; else goto L10;
  L15:;
    return;
  L16:;
#line 258
    Narration_Event(38);
    goto L15;
  L17:;
    if (NPC_LookAtNPC(Actor_actors[0].absX, Actor_actors[0].absY, GameState_nameBuf, 31)) goto L19; else goto L20;
  L18:;
    goto L15;
  L19:;
#line 261
    m2_Strings_Assign("% sees ", GameState_msgBuf, 79);
#line 262
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 263
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
#line 264
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L18;
  L20:;
#line 265
    Narration_Event(20);
    goto L18;
}

static void GameState_TogglePause(void) {
    if (((uint32_t)(((uint32_t)(Menu_menus[4].enabled[5]))) & (uint32_t)(1)) == 0) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 272
    Menu_menus[4].enabled[5] = ((uint32_t)(((int32_t)(((uint32_t)(Menu_menus[4].enabled[5]))))) | (uint32_t)(1));
#line 273
    GameState_ShowMessage("Game paused.", 12);
    goto L1;
  L3:;
#line 275
    Menu_menus[4].enabled[5] = ((uint32_t)(((uint32_t)(Menu_menus[4].enabled[5]))) & (uint32_t)(14));
#line 276
    GameState_ShowMessage("Game resumed.", 13);
    goto L1;
}

static void GameState_ToggleMusic(void) {
    if (((uint32_t)(((uint32_t)(Menu_menus[4].enabled[6]))) & (uint32_t)(1)) == 0) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 283
    Menu_menus[4].enabled[6] = ((uint32_t)(((int32_t)(((uint32_t)(Menu_menus[4].enabled[6]))))) | (uint32_t)(1));
#line 284
    Music_ResumeMusic();
    goto L1;
  L3:;
#line 286
    Menu_menus[4].enabled[6] = ((uint32_t)(((uint32_t)(Menu_menus[4].enabled[6]))) & (uint32_t)(14));
#line 287
    Music_StopMusic();
    goto L1;
}

static void GameState_WeaponName(int32_t w, char *name, uint32_t name_high) {
    (void)w;
    (void)name;
    (void)name_high;
    if ((w == 1)) goto L3;
    if ((w == 2)) goto L4;
    if ((w == 3)) goto L5;
    if ((w == 4)) goto L6;
    if ((w == 5)) goto L7;
    goto L2;
  L1:;
    return;
  L2:;
#line 297
    m2_Strings_Assign("a weapon", name, name_high);
    goto L1;
  L3:;
#line 294
    m2_Strings_Assign("a dagger", name, name_high);
    goto L1;
  L4:;
    m2_Strings_Assign("a mace", name, name_high);
    goto L1;
  L5:;
#line 295
    m2_Strings_Assign("a sword", name, name_high);
    goto L1;
  L6:;
    m2_Strings_Assign("a bow", name, name_high);
    goto L1;
  L7:;
#line 296
    m2_Strings_Assign("a wand", name, name_high);
    goto L1;
}

static void GameState_TreasureName(int32_t ti, char *name, uint32_t name_high) {
    (void)ti;
    (void)name;
    (void)name_high;
    if ((ti == 9)) goto L3;
    if ((ti == 10)) goto L4;
    if ((ti == 11)) goto L5;
    if ((ti == 12)) goto L6;
    if ((ti == 13)) goto L7;
    if ((ti == 14)) goto L8;
    if ((ti == 15)) goto L9;
    if ((ti == 16)) goto L10;
    if ((ti == 17)) goto L11;
    if ((ti == 18)) goto L12;
    if ((ti == 19)) goto L13;
    if ((ti == 20)) goto L14;
    if ((ti == 21)) goto L15;
    if ((ti == 31)) goto L16;
    if ((ti == 32)) goto L17;
    if ((ti == 33)) goto L18;
    if ((ti == 34)) goto L19;
    if ((ti == 35)) goto L20;
    if ((ti == 36)) goto L21;
    if ((ti == 37)) goto L22;
    if ((ti == 38)) goto L23;
    if ((ti == 39)) goto L24;
    if ((ti == 40)) goto L25;
    if ((ti == 41)) goto L26;
    if ((ti == 42)) goto L27;
    if ((ti == 43)) goto L28;
    if ((ti == 44)) goto L29;
    goto L2;
  L1:;
    return;
  L2:;
#line 324
    m2_Strings_Assign("a treasure", name, name_high);
    goto L1;
  L3:;
#line 303
    m2_Strings_Assign("a Blue Stone", name, name_high);
    goto L1;
  L4:;
    m2_Strings_Assign("a Green Jewel", name, name_high);
    goto L1;
  L5:;
#line 304
    m2_Strings_Assign("a Glass Vial", name, name_high);
    goto L1;
  L6:;
    m2_Strings_Assign("a Crystal Orb", name, name_high);
    goto L1;
  L7:;
#line 305
    m2_Strings_Assign("a Bird Totem", name, name_high);
    goto L1;
  L8:;
    m2_Strings_Assign("a Gold Ring", name, name_high);
    goto L1;
  L9:;
#line 306
    m2_Strings_Assign("a Jade Skull", name, name_high);
    goto L1;
  L10:;
    m2_Strings_Assign("a Gold Key", name, name_high);
    goto L1;
  L11:;
#line 307
    m2_Strings_Assign("a Green Key", name, name_high);
    goto L1;
  L12:;
    m2_Strings_Assign("a Blue Key", name, name_high);
    goto L1;
  L13:;
#line 308
    m2_Strings_Assign("a Red Key", name, name_high);
    goto L1;
  L14:;
    m2_Strings_Assign("a Grey Key", name, name_high);
    goto L1;
  L15:;
#line 309
    m2_Strings_Assign("a White Key", name, name_high);
    goto L1;
  L16:;
#line 310
    m2_Strings_Assign("Mandrake", name, name_high);
    goto L1;
  L17:;
#line 311
    m2_Strings_Assign("Wolfsbane", name, name_high);
    goto L1;
  L18:;
#line 312
    m2_Strings_Assign("Mugwort", name, name_high);
    goto L1;
  L19:;
#line 313
    m2_Strings_Assign("Yarrow", name, name_high);
    goto L1;
  L20:;
#line 314
    m2_Strings_Assign("Nightshade", name, name_high);
    goto L1;
  L21:;
#line 315
    m2_Strings_Assign("Bloodroot", name, name_high);
    goto L1;
  L22:;
#line 316
    m2_Strings_Assign("a Ward Scroll", name, name_high);
    goto L1;
  L23:;
#line 317
    m2_Strings_Assign("a Freeze Scroll", name, name_high);
    goto L1;
  L24:;
#line 318
    m2_Strings_Assign("a Fire Scroll", name, name_high);
    goto L1;
  L25:;
#line 319
    m2_Strings_Assign("a Fear Scroll", name, name_high);
    goto L1;
  L26:;
#line 320
    m2_Strings_Assign("a Light Scroll", name, name_high);
    goto L1;
  L27:;
#line 321
    m2_Strings_Assign("a Sanctuary Scroll", name, name_high);
    goto L1;
  L28:;
#line 322
    m2_Strings_Assign("a Harvest Scroll", name, name_high);
    goto L1;
  L29:;
#line 323
    m2_Strings_Assign("a Heal Scroll", name, name_high);
    goto L1;
}

static int GameState_TakeEnemyWeapon(int32_t enemyIdx, char *name, uint32_t name_high) {
    (void)enemyIdx;
    (void)name;
    (void)name_high;
    int32_t w;
#line 330
    w = Actor_actors[enemyIdx].weapon;
    if (w < 1) goto L2; else goto L3;
  L1:;
#line 332
    Brothers_GiveStuff((w - 1));
#line 333
    GameState_WeaponName(w, name, name_high);
    if (w > Actor_actors[0].weapon) goto L5; else goto L4;
  L2:;
    return 0;
  L3:;
    if (w > 5) goto L2; else goto L1;
  L4:;
    if (w == 4) goto L7; else goto L6;
  L5:;
#line 334
    Actor_actors[0].weapon = w;
    goto L4;
  L6:;
#line 338
    Actor_actors[enemyIdx].weapon = 0;
    return 1;
  L7:;
#line 336
    Brothers_AddStuffN(8, (m2_mod(GameState_cycle, 8) + 2));
    goto L6;
}

static int GameState_TakeEnemyTreasure(int32_t enemyIdx, char *name, uint32_t name_high) {
    (void)enemyIdx;
    (void)name;
    (void)name_high;
    int32_t race;
    int32_t tg;
    int32_t ti;
    int32_t gv;
    int32_t roll;
    if (Actor_actors[enemyIdx].looted) goto L2; else goto L1;
  L1:;
#line 346
    Actor_actors[enemyIdx].looted = 1;
#line 347
    race = Actor_actors[enemyIdx].race;
    ti = 0;
    if (race == 2) goto L4; else goto L5;
  L2:;
    return 0;
  L3:;
    if (ti <= 0) goto L15; else goto L14;
  L4:;
#line 351
    roll = m2_mod(((GameState_cycle + Actor_actors[enemyIdx].absX) + Actor_actors[enemyIdx].absY), 20);
    if (roll < 3) goto L7; else goto L8;
  L5:;
    if (race < 128) goto L9; else goto L3;
  L6:;
    goto L3;
  L7:;
#line 353
    ti = (37 + m2_mod((GameState_cycle + Actor_actors[enemyIdx].absX), 8));
    goto L6;
  L8:;
#line 355
    ti = (31 + m2_mod((GameState_cycle + Actor_actors[enemyIdx].absY), 6));
    goto L6;
  L9:;
#line 358
    tg = GameState_TreasureGroup(race);
#line 359
    ti = ((tg * 8) + m2_mod(GameState_cycle, 8));
    if (ti >= 0) goto L13; else goto L12;
  L10:;
    goto L3;
  L11:;
#line 360
    ti = GameState_treasureProbs[ti];
    goto L10;
  L12:;
    ti = 0;
    goto L10;
  L13:;
    if (ti <= 39) goto L11; else goto L12;
  L14:;
    if (race == 2) goto L17; else goto L18;
  L15:;
    return 0;
  L16:;
    return 1;
  L17:;
#line 364
    Brothers_GiveStuff(ti);
    GameState_TreasureName(ti, name, name_high);
    goto L16;
  L18:;
    if (ti >= 31) goto L19; else goto L20;
  L19:;
#line 366
    gv = GameState_GoldValue(ti);
    Brothers_AddWealth(gv);
#line 367
    GameState_IntToStr(gv, name, name_high);
    m2_Strings_Concat(name, " Gold Pieces", name, name_high);
    goto L16;
  L20:;
#line 369
    Brothers_GiveStuff(ti);
    GameState_TreasureName(ti, name, name_high);
    goto L16;
}

static void GameState_SearchNearbyCorpses(void) {
    int32_t i;
    int32_t dx;
    int32_t dy;
    char wname[31 + 1];
    char tname[31 + 1];
    int hasWeapon;
    int hasTreasure;
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Actor_actors[i].actorType == 2) goto L7; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 400
    GameState_ShowMessage("Nothing to take.", 16);
    return;
  L5:;
    goto L3;
  L6:;
#line 381
    dx = (Actor_actors[0].absX - Actor_actors[i].absX);
#line 382
    dy = (Actor_actors[0].absY - Actor_actors[i].absY);
    if (dx < 0) goto L9; else goto L8;
  L7:;
    if (Actor_actors[i].state == 15) goto L6; else goto L5;
  L8:;
    if (dy < 0) goto L11; else goto L10;
  L9:;
#line 383
    dx = (-dx);
    goto L8;
  L10:;
    if (dx < 20) goto L14; else goto L12;
  L11:;
#line 384
    dy = (-dy);
    goto L10;
  L12:;
    goto L5;
  L13:;
#line 386
    hasWeapon = GameState_TakeEnemyWeapon(i, wname, 31);
#line 387
    hasTreasure = GameState_TakeEnemyTreasure(i, tname, 31);
#line 388
    m2_Strings_Assign("% searched the body and found ", GameState_msgBuf, 79);
    if (hasWeapon) goto L16; else goto L17;
  L14:;
    if (dy < 20) goto L13; else goto L12;
  L15:;
#line 394
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
#line 395
    GameState_ShowMessage(GameState_msgBuf, 79);
#line 396
    Menu_SetOptions();
    return;
  L16:;
#line 390
    m2_Strings_Concat(GameState_msgBuf, wname, GameState_msgBuf, 79);
    if (hasTreasure) goto L19; else goto L18;
  L17:;
    if (hasTreasure) goto L20; else goto L21;
  L18:;
    goto L15;
  L19:;
#line 391
    m2_Strings_Concat(GameState_msgBuf, " and ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, tname, GameState_msgBuf, 79);
    goto L18;
  L20:;
#line 392
    m2_Strings_Concat(GameState_msgBuf, tname, GameState_msgBuf, 79);
    goto L15;
  L21:;
#line 393
    m2_Strings_Concat(GameState_msgBuf, "nothing", GameState_msgBuf, 79);
    goto L15;
}

static int32_t GameState_RandContainer(int32_t limit) {
    (void)limit;
#line 405
    GameState_containerRng = ((GameState_containerRng * 1103515245) + 12345);
    if (GameState_containerRng < 0) goto L2; else goto L1;
  L1:;
    if (limit <= 0) goto L4; else goto L3;
  L2:;
#line 406
    GameState_containerRng = (-GameState_containerRng);
    goto L1;
  L3:;
    return m2_mod(m2_div(GameState_containerRng, 65536), limit);
  L4:;
    return 0;
}

static int32_t GameState_PickContainerItem(void) {
    int32_t i;
#line 414
    i = (GameState_RandContainer(8) + 8);
    if (i == 8) goto L2; else goto L1;
  L1:;
    return i;
  L2:;
#line 415
    i = 9;
    goto L1;
}

static void GameState_ContainerLoot(void) {
    int32_t k;
    int32_t i;
    int32_t j;
    int32_t gv;
    char tname[31 + 1];
    char numStr[31 + 1];
#line 423
    k = GameState_RandContainer(4);
    if (k == 0) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 424
    GameState_ShowMessage("nothing.", 8);
    goto L1;
  L3:;
    if (k == 1) goto L4; else goto L5;
  L4:;
#line 426
    i = GameState_PickContainerItem();
#line 427
    Brothers_GiveStuff(i);
    GameState_TreasureName(i, tname, 31);
#line 428
    m2_Strings_Assign(tname, GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
#line 429
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L1;
  L5:;
    if (k == 2) goto L6; else goto L7;
  L6:;
#line 431
    i = GameState_PickContainerItem();
    if (i == 8) goto L9; else goto L10;
  L7:;
#line 445
    i = (GameState_RandContainer(8) + 8);
    if (i == 8) goto L15; else goto L16;
  L8:;
#line 437
    j = GameState_PickContainerItem();
    goto L11;
  L9:;
#line 433
    gv = 100;
    Brothers_AddWealth(gv);
#line 434
    GameState_IntToStr(gv, numStr, 31);
    m2_Strings_Assign(numStr, GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, " Gold Pieces", GameState_msgBuf, 79);
    goto L8;
  L10:;
#line 435
    Brothers_GiveStuff(i);
    GameState_TreasureName(i, tname, 31);
    m2_Strings_Assign(tname, GameState_msgBuf, 79);
    goto L8;
  L11:;
    if (j == i) goto L12; else goto L13;
  L12:;
#line 438
    j = GameState_PickContainerItem();
    goto L11;
  L13:;
#line 439
    Brothers_GiveStuff(j);
    GameState_TreasureName(j, tname, 31);
#line 440
    m2_Strings_Concat(GameState_msgBuf, " and ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, tname, GameState_msgBuf, 79);
#line 441
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L1;
  L14:;
    goto L1;
  L15:;
#line 447
    GameState_ShowMessage("3 keys.", 7);
    j = 0;
    goto L17;
  L16:;
#line 457
    Brothers_GiveStuff(i);
    Brothers_GiveStuff(i);
    Brothers_GiveStuff(i);
#line 458
    GameState_TreasureName(i, tname, 31);
#line 459
    m2_Strings_Assign("3 ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, tname, GameState_msgBuf, 79);
#line 460
    m2_Strings_Concat(GameState_msgBuf, "s.", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L14;
  L17:;
    if (j <= 2) goto L18; else goto L20;
  L18:;
#line 449
    k = (GameState_RandContainer(8) + 16);
    if (k == 22) goto L22; else goto L23;
  L19:;
    j = (j + 1);
    goto L17;
  L20:;
    goto L14;
  L21:;
#line 454
    Brothers_GiveStuff(k);
    goto L19;
  L22:;
#line 451
    k = 16;
    goto L21;
  L23:;
    if (k == 23) goto L24; else goto L21;
  L24:;
#line 452
    k = 20;
    goto L21;
}

static void GameState_InitBuyTable(void) {
#line 476
    GameState_buyStuff[0] = 0;
    GameState_buyCost[0] = 3;
#line 477
    GameState_buyStuff[1] = 8;
    GameState_buyCost[1] = 10;
#line 478
    GameState_buyStuff[2] = 11;
    GameState_buyCost[2] = 15;
#line 479
    GameState_buyStuff[3] = 1;
    GameState_buyCost[3] = 30;
#line 480
    GameState_buyStuff[4] = 2;
    GameState_buyCost[4] = 45;
#line 481
    GameState_buyStuff[5] = 3;
    GameState_buyCost[5] = 75;
#line 482
    GameState_buyStuff[6] = 13;
    GameState_buyCost[6] = 20;
    return;
}

static void GameState_HandleBuy(int32_t optIdx) {
    (void)optIdx;
    int32_t npc;
    int32_t slot;
    int32_t si;
    int32_t cost;
#line 488
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L1;
  L1:;
    if (Actor_actors[npc].race != 8) goto L4; else goto L3;
  L2:;
#line 489
    GameState_ShowMessage("Nobody to buy from.", 19);
    return;
  L3:;
    if (optIdx < 5) goto L6; else goto L7;
  L4:;
#line 491
    GameState_ShowMessage("Nobody to buy from.", 19);
    return;
  L5:;
#line 494
    slot = (optIdx - 5);
#line 495
    si = GameState_buyStuff[slot];
#line 496
    cost = GameState_buyCost[slot];
    if (Brothers_brothers[Brothers_activeBrother].wealth > cost) goto L9; else goto L10;
  L6:;
    return;
  L7:;
    if (optIdx > 11) goto L6; else goto L5;
  L8:;
    return;
  L9:;
#line 498
    Brothers_AddWealth((-cost));
    if (si == 0) goto L12; else goto L13;
  L10:;
#line 520
    GameState_ShowMessage("Not enough money!", 17);
    goto L8;
  L11:;
#line 518
    Menu_SetOptions();
    goto L8;
  L12:;
#line 501
    Narration_Event(22);
#line 502
    (GameState_hunger -= 50);
    if (GameState_hunger < 0) goto L15; else goto L16;
  L13:;
    if (si == 8) goto L17; else goto L18;
  L14:;
    goto L11;
  L15:;
#line 503
    GameState_hunger = 0;
    Narration_Event(13);
    goto L14;
  L16:;
#line 504
    GameState_ShowMessage("Yum!", 4);
    goto L14;
  L17:;
#line 508
    Brothers_AddStuffN(8, 10);
#line 509
    Narration_Event(23);
    goto L11;
  L18:;
#line 511
    Brothers_GiveStuff(si);
#line 512
    m2_Strings_Assign("% bought a ", GameState_msgBuf, 79);
#line 513
    GameState_TreasureName(si, GameState_nameBuf, 31);
#line 514
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 515
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
#line 516
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L11;
}

static void GameState_HandleSell(int32_t optIdx) {
    (void)optIdx;
    int32_t npc;
#line 527
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L3;
  L1:;
    if ((optIdx == 5)) goto L6;
    if ((optIdx == 6)) goto L7;
    goto L5;
  L2:;
#line 529
    GameState_ShowMessage("You can only sell items in a tavern.", 36);
#line 530
    Menu_GoMenu(0);
    return;
  L3:;
    if (Actor_actors[npc].race != 8) goto L2; else goto L1;
  L4:;
#line 549
    Menu_SetOptions();
    return;
  L5:;
    return;
  L6:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[24] > 0) goto L9; else goto L10;
  L7:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[20] > 0) goto L12; else goto L13;
  L8:;
    goto L4;
  L9:;
#line 536
    (Brothers_brothers[Brothers_activeBrother].stuff[24]--);
#line 537
    Brothers_AddWealth(5);
#line 538
    GameState_ShowMessage("% sold an apple for 5 gold.", 27);
    goto L8;
  L10:;
#line 539
    GameState_ShowMessage("% doesn't have an apple.", 24);
    goto L8;
  L11:;
    goto L4;
  L12:;
#line 542
    (Brothers_brothers[Brothers_activeBrother].stuff[20]--);
#line 543
    Brothers_AddWealth(50);
#line 544
    GameState_ShowMessage("% sold a grey key for 50 gold.", 30);
    goto L11;
  L13:;
#line 545
    GameState_ShowMessage("% doesn't have a grey key.", 26);
    goto L11;
}

static int32_t GameState_HerbPrice(int32_t si) {
    (void)si;
    if ((si == 31)) goto L3;
    if ((si == 32)) goto L4;
    if ((si == 33)) goto L5;
    if ((si == 34)) goto L6;
    if ((si == 35)) goto L7;
    if ((si == 36)) goto L8;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 30;
  L4:;
    return 40;
  L5:;
    return 25;
  L6:;
    return 20;
  L7:;
    return 45;
  L8:;
    return 50;
}

static int32_t GameState_HerbStuff(int32_t optIdx) {
    (void)optIdx;
    if (optIdx < 5) goto L2; else goto L3;
  L1:;
    return ((31 + optIdx) - 5);
  L2:;
    return (-1);
  L3:;
    if (optIdx > 10) goto L2; else goto L1;
}

static void GameState_HandleHerbBuy(int32_t optIdx) {
    (void)optIdx;
    int32_t npc;
    int32_t si;
    int32_t cost;
#line 575
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L3;
  L1:;
#line 579
    si = GameState_HerbStuff(optIdx);
    if (si < 0) goto L5; else goto L4;
  L2:;
#line 577
    GameState_ShowMessage("The herb wizard is not nearby.", 30);
    Menu_GoMenu(0);
    return;
  L3:;
    if (Actor_actors[npc].race != 16) goto L2; else goto L1;
  L4:;
#line 581
    cost = GameState_HerbPrice(si);
    if (Brothers_brothers[Brothers_activeBrother].wealth < cost) goto L7; else goto L6;
  L5:;
    return;
  L6:;
#line 585
    Brothers_AddWealth((-cost));
#line 586
    Brothers_GiveStuff(si);
#line 587
    GameState_TreasureName(si, GameState_nameBuf, 31);
#line 588
    m2_Strings_Assign("% bought ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 589
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
#line 590
    Menu_SetOptions();
    return;
  L7:;
#line 583
    GameState_ShowMessage("Not enough money!", 17);
    return;
}

static void GameState_HandleHerbSell(int32_t optIdx) {
    (void)optIdx;
    int32_t npc;
    int32_t si;
    int32_t price;
#line 596
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L3;
  L1:;
#line 600
    si = GameState_HerbStuff(optIdx);
    if (si < 0) goto L5; else goto L4;
  L2:;
#line 598
    GameState_ShowMessage("The herb wizard is not nearby.", 30);
    Menu_GoMenu(0);
    return;
  L3:;
    if (Actor_actors[npc].race != 16) goto L2; else goto L1;
  L4:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[si] <= 0) goto L7; else goto L6;
  L5:;
    return;
  L6:;
#line 605
    (Brothers_brothers[Brothers_activeBrother].stuff[si]--);
#line 606
    price = m2_div(GameState_HerbPrice(si), 2);
#line 607
    Brothers_AddWealth(price);
#line 608
    GameState_TreasureName(si, GameState_nameBuf, 31);
#line 609
    m2_Strings_Assign("% sold ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 610
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
#line 611
    Menu_SetOptions();
    return;
  L7:;
#line 603
    GameState_ShowMessage("% doesn't have that ingredient.", 31);
    return;
}

static int32_t GameState_ScrollPrice(int32_t si) {
    (void)si;
    if ((si == 37)) goto L3;
    if ((si == 38)) goto L4;
    if ((si == 39)) goto L5;
    if ((si == 40)) goto L6;
    if ((si == 41)) goto L7;
    if ((si == 42)) goto L8;
    if ((si == 43)) goto L9;
    if ((si == 44)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 30;
  L4:;
    return 50;
  L5:;
    return 60;
  L6:;
    return 40;
  L7:;
    return 20;
  L8:;
    return 100;
  L9:;
    return 70;
  L10:;
    return 40;
}

static void GameState_HandleScrollBuy(int32_t optIdx) {
    (void)optIdx;
    int32_t npc;
    int32_t si;
    int32_t cost;
#line 633
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L3;
  L1:;
    if (optIdx < 5) goto L5; else goto L6;
  L2:;
#line 635
    GameState_ShowMessage("The scroll priest is not nearby.", 32);
    Menu_GoMenu(0);
    return;
  L3:;
    if (Actor_actors[npc].race != 17) goto L2; else goto L1;
  L4:;
#line 638
    si = ((37 + optIdx) - 5);
#line 639
    cost = GameState_ScrollPrice(si);
    if (Brothers_brothers[Brothers_activeBrother].wealth < cost) goto L8; else goto L7;
  L5:;
    return;
  L6:;
    if (optIdx > 12) goto L5; else goto L4;
  L7:;
#line 643
    Brothers_AddWealth((-cost));
#line 644
    Brothers_GiveStuff(si);
#line 645
    GameState_TreasureName(si, GameState_nameBuf, 31);
#line 646
    m2_Strings_Assign("% bought ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 647
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
#line 648
    Menu_SetOptions();
    return;
  L8:;
#line 641
    GameState_ShowMessage("Not enough money!", 17);
    return;
}

static void GameState_HandleAppleBuy(int32_t optIdx) {
    (void)optIdx;
    static const int32_t AppleCost = 5;
    int32_t npc;
#line 655
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L3;
  L1:;
    if (optIdx != 5) goto L5; else goto L4;
  L2:;
#line 657
    GameState_ShowMessage("The apple ranger is not nearby.", 31);
    Menu_GoMenu(0);
    return;
  L3:;
    if (Actor_actors[npc].race != 18) goto L2; else goto L1;
  L4:;
    if (Brothers_brothers[Brothers_activeBrother].wealth < 5) goto L7; else goto L6;
  L5:;
    return;
  L6:;
#line 663
    Brothers_AddWealth((-5));
#line 664
    Brothers_GiveStuff(24);
#line 665
    GameState_ShowMessage("% bought an apple.", 18);
#line 666
    Menu_SetOptions();
    return;
  L7:;
#line 661
    GameState_ShowMessage("Not enough money!", 17);
    return;
}

static void GameState_OpenBuyMenu(void) {
    int32_t npc;
#line 672
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc >= 0) goto L4; else goto L3;
  L1:;
    return;
  L2:;
#line 674
    Menu_GoMenu(16);
    goto L1;
  L3:;
    if (npc >= 0) goto L7; else goto L6;
  L4:;
    if (Actor_actors[npc].race == 16) goto L2; else goto L3;
  L5:;
#line 676
    Menu_GoMenu(18);
    goto L1;
  L6:;
    if (npc >= 0) goto L10; else goto L9;
  L7:;
    if (Actor_actors[npc].race == 17) goto L5; else goto L6;
  L8:;
#line 678
    Menu_GoMenu(19);
    goto L1;
  L9:;
#line 680
    Menu_GoMenu(3);
    goto L1;
  L10:;
    if (Actor_actors[npc].race == 18) goto L8; else goto L9;
}

static void GameState_OpenSellMenu(void) {
    int32_t npc;
#line 687
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc >= 0) goto L4; else goto L3;
  L1:;
    return;
  L2:;
#line 689
    Menu_GoMenu(17);
    goto L1;
  L3:;
#line 691
    Menu_GoMenu(10);
    goto L1;
  L4:;
    if (Actor_actors[npc].race == 16) goto L2; else goto L3;
}

static void GameState_HandleGive(int32_t optIdx) {
    (void)optIdx;
    char resp[127 + 1];
    int32_t npc;
#line 699
    npc = NPC_FindNearestNPC(Actor_actors[0].absX, Actor_actors[0].absY);
    if (npc < 0) goto L2; else goto L1;
  L1:;
    if (NPC_GiveToNPC(Actor_actors[0].absX, Actor_actors[0].absY, (optIdx - 5), resp, 127)) goto L4; else goto L3;
  L2:;
#line 700
    GameState_ShowMessage("Nobody here.", 12);
    Menu_GoMenu(0);
    return;
  L3:;
#line 704
    Menu_SetOptions();
#line 705
    Menu_GoMenu(0);
    return;
  L4:;
    if (resp[0] != '\0') goto L6; else goto L5;
  L5:;
    goto L3;
  L6:;
#line 702
    GameState_ShowMessage(resp, 127);
    goto L5;
}

static void GameState_InheritBrotherItems(void) {
    int32_t prev;
    int32_t k;
#line 714
    prev = (Brothers_activeBrother - 1);
    if (prev >= 0) goto L4; else goto L1;
  L1:;
#line 722
    WorldObj_objects[0].status = 0;
#line 723
    WorldObj_objects[1].status = 0;
    return;
  L2:;
    k = 0;
    goto L5;
  L3:;
    if (Brothers_brothers[prev].alive) goto L1; else goto L2;
  L4:;
    if (prev <= 2) goto L3; else goto L1;
  L5:;
    if (k <= 44) goto L6; else goto L8;
  L6:;
#line 717
    (Brothers_brothers[Brothers_activeBrother].stuff[k] += Brothers_brothers[prev].stuff[k]);
#line 718
    Brothers_brothers[prev].stuff[k] = 0;
    goto L7;
  L7:;
    k = (k + 1);
    goto L5;
  L8:;
    goto L1;
}

static void GameState_HandleKeys(int32_t optIdx) {
    (void)optIdx;
    int32_t keyIdx;
#line 729
    keyIdx = (optIdx - 5);
    if (keyIdx < 0) goto L2; else goto L3;
  L1:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[(16 + keyIdx)] <= 0) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (keyIdx > 5) goto L2; else goto L1;
  L4:;
    if (Doors_UseKeyOnDoor(Actor_actors[0].absX, Actor_actors[0].absY, (keyIdx + 1))) goto L7; else goto L8;
  L5:;
#line 732
    GameState_ShowMessage("You don't have that key.", 24);
    Menu_GoMenu(0);
    return;
  L6:;
#line 744
    Menu_GoMenu(0);
    return;
  L7:;
#line 735
    (Brothers_brothers[Brothers_activeBrother].stuff[(16 + keyIdx)]--);
#line 736
    GameState_ShowMessage("It opened.", 10);
    goto L6;
  L8:;
#line 738
    GameState_TreasureName((16 + keyIdx), GameState_nameBuf, 31);
#line 739
    m2_Strings_Assign("% tried ", GameState_msgBuf, 79);
#line 740
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 741
    m2_Strings_Concat(GameState_msgBuf, " but it didn't fit.", GameState_msgBuf, 79);
#line 742
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L6;
}

static void GameState_InitStoneList(void) {
#line 754
    GameState_stoneX[0] = 54;
    GameState_stoneY[0] = 43;
#line 755
    GameState_stoneX[1] = 71;
    GameState_stoneY[1] = 77;
#line 756
    GameState_stoneX[2] = 78;
    GameState_stoneY[2] = 102;
#line 757
    GameState_stoneX[3] = 66;
    GameState_stoneY[3] = 121;
#line 758
    GameState_stoneX[4] = 12;
    GameState_stoneY[4] = 85;
#line 759
    GameState_stoneX[5] = 79;
    GameState_stoneY[5] = 40;
#line 760
    GameState_stoneX[6] = 107;
    GameState_stoneY[6] = 38;
#line 761
    GameState_stoneX[7] = 73;
    GameState_stoneY[7] = 21;
#line 762
    GameState_stoneX[8] = 12;
    GameState_stoneY[8] = 26;
#line 763
    GameState_stoneX[9] = 26;
    GameState_stoneY[9] = 53;
#line 764
    GameState_stoneX[10] = 84;
    GameState_stoneY[10] = 60;
    return;
}

static void GameState_GetStoneCircle(int32_t i, int32_t *x, int32_t *y) {
    (void)i;
    (void)x;
    (void)y;
    if (i < 0) goto L2; else goto L3;
  L1:;
#line 773
    (*x) = ((GameState_stoneX[i] * 256) + 128);
#line 774
    (*y) = ((GameState_stoneY[i] * 256) + 128);
    return;
  L2:;
#line 770
    (*x) = 0;
    (*y) = 0;
    return;
  L3:;
    if (i >= 11) goto L2; else goto L1;
}

static void GameState_HandleStoneTeleport(void) {
    int32_t sx;
    int32_t sy;
    int32_t i;
    int32_t dest;
    int32_t newX;
    int32_t newY;
#line 781
    sx = m2_div(Actor_actors[0].absX, 256);
#line 782
    sy = m2_div(Actor_actors[0].absY, 256);
    i = 0;
    goto L1;
  L1:;
    if (i <= 10) goto L2; else goto L4;
  L2:;
    if (GameState_stoneX[i] == sx) goto L7; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 798
    GameState_ShowMessage("The ring pulses but nothing happens.", 36);
    return;
  L5:;
    goto L3;
  L6:;
#line 786
    dest = m2_mod(((i + Actor_actors[0].facing) + 1), 11);
#line 787
    newX = ((GameState_stoneX[dest] * 256) + ((uint32_t)(((uint32_t)(Actor_actors[0].absX))) & (uint32_t)(255)));
#line 788
    newY = ((GameState_stoneY[dest] * 256) + ((uint32_t)(((uint32_t)(Actor_actors[0].absY))) & (uint32_t)(255)));
#line 789
    GameState_colorPlayTimer = 32;
#line 790
    Actor_actors[0].absX = newX;
#line 791
    Actor_actors[0].absY = newY;
#line 792
    (Brothers_brothers[Brothers_activeBrother].stuff[9]--);
#line 793
    GameState_ShowMessage("The stone transports you!", 25);
#line 794
    Menu_SetOptions();
    return;
  L7:;
    if (GameState_stoneY[i] == sy) goto L6; else goto L5;
}

static void GameState_KillWeakEnemies(void) {
    int32_t i;
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Actor_actors[i].vitality > 0) goto L8; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    if (GameState_battleFlag) goto L10; else goto L9;
  L5:;
    goto L3;
  L6:;
#line 808
    Actor_actors[i].vitality = 0;
#line 809
    Actor_actors[i].state = 14;
#line 810
    (Brothers_brothers[Brothers_activeBrother].brave--);
    goto L5;
  L7:;
    if (Actor_actors[i].race < 7) goto L6; else goto L5;
  L8:;
    if (Actor_actors[i].actorType == 2) goto L7; else goto L5;
  L9:;
#line 814
    GameState_ShowMessage("Dark energy destroys the enemies!", 33);
    return;
  L10:;
#line 813
    Narration_Event(34);
    goto L9;
}

static void GameState_HandleMagic(int32_t optIdx) {
    (void)optIdx;
    int32_t itemIdx;
    int32_t si;
    int32_t v;
    int used;
    if (optIdx == 12) goto L2; else goto L3;
  L1:;
#line 825
    itemIdx = (optIdx - 5);
    if (itemIdx < 0) goto L8; else goto L9;
  L2:;
#line 821
    Menu_GoMenu(11);
    return;
  L3:;
    if (optIdx == 13) goto L4; else goto L5;
  L4:;
#line 822
    Menu_GoMenu(12);
    return;
  L5:;
    if (optIdx == 14) goto L6; else goto L1;
  L6:;
#line 823
    Menu_GoMenu(13);
    return;
  L7:;
#line 827
    si = (9 + itemIdx);
    if (Brothers_brothers[Brothers_activeBrother].stuff[si] <= 0) goto L11; else goto L10;
  L8:;
    return;
  L9:;
    if (itemIdx > 6) goto L8; else goto L7;
  L10:;
#line 831
    used = 1;
    if ((itemIdx == 0)) goto L14;
    if ((itemIdx == 1)) goto L15;
    if ((itemIdx == 2)) goto L16;
    if ((itemIdx == 3)) goto L17;
    if ((itemIdx == 4)) goto L18;
    if ((itemIdx == 5)) goto L19;
    if ((itemIdx == 6)) goto L20;
    goto L13;
  L11:;
#line 829
    Narration_Event(21);
    Menu_GoMenu(0);
    return;
  L12:;
    if (used) goto L25; else goto L24;
  L13:;
#line 859
    Menu_GoMenu(0);
    return;
  L14:;
#line 834
    GameState_HandleStoneTeleport();
#line 835
    used = 0;
    goto L12;
  L15:;
#line 837
    (GameState_lightTimer += 760);
#line 838
    GameState_ShowMessage("Everything is bathed in green light!", 36);
    goto L12;
  L16:;
#line 840
    v = (m2_mod(GameState_cycle, 8) + 4);
#line 841
    (Actor_actors[0].vitality += v);
    if (Actor_actors[0].vitality > (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4))) goto L22; else goto L23;
  L17:;
#line 848
    (GameState_secretTimer += 360);
#line 849
    GameState_ShowMessage("Hidden things shimmer into view!", 32);
    goto L12;
  L18:;
#line 851
    GameState_viewStatus = 5;
#line 852
    GameState_ShowMessage("A bird's eye view!", 18);
    goto L12;
  L19:;
#line 854
    (GameState_freezeTimer += 250);
#line 855
    GameState_ShowMessage("Time stands still!", 18);
    goto L12;
  L20:;
#line 857
    GameState_KillWeakEnemies();
    goto L12;
  L21:;
    goto L12;
  L22:;
#line 843
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
    goto L21;
  L23:;
#line 845
    GameState_ShowMessage("That feels a lot better!", 24);
    goto L21;
  L24:;
#line 864
    Menu_SetOptions();
#line 865
    Menu_GoMenu(0);
    return;
  L25:;
#line 862
    (Brothers_brothers[Brothers_activeBrother].stuff[si]--);
    goto L24;
}

static void GameState_DamageNearbyEnemies(int32_t amount, int32_t radius) {
    (void)amount;
    (void)radius;
    int32_t i;
    int32_t dx;
    int32_t dy;
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
#line 872
    dx = (Actor_actors[i].absX - Actor_actors[0].absX);
#line 873
    dy = (Actor_actors[i].absY - Actor_actors[0].absY);
    if (dx < 0) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    if (dy < 0) goto L8; else goto L7;
  L6:;
#line 874
    dx = (-dx);
    goto L5;
  L7:;
    if (Actor_actors[i].actorType == 2) goto L13; else goto L9;
  L8:;
#line 875
    dy = (-dy);
    goto L7;
  L9:;
    goto L3;
  L10:;
#line 878
    (Actor_actors[i].vitality -= amount);
    if (Actor_actors[i].vitality <= 0) goto L15; else goto L14;
  L11:;
    if (dy < radius) goto L10; else goto L9;
  L12:;
    if (dx < radius) goto L11; else goto L9;
  L13:;
    if (Actor_actors[i].vitality > 0) goto L12; else goto L9;
  L14:;
    goto L9;
  L15:;
#line 880
    Actor_actors[i].vitality = 0;
#line 881
    Actor_actors[i].state = 14;
#line 882
    Actor_actors[i].tactic = 7;
    goto L14;
}

static void GameState_FrightenNearbyEnemies(int32_t radius) {
    (void)radius;
    int32_t i;
    int32_t dx;
    int32_t dy;
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
#line 892
    dx = (Actor_actors[i].absX - Actor_actors[0].absX);
#line 893
    dy = (Actor_actors[i].absY - Actor_actors[0].absY);
    if (dx < 0) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    if (dy < 0) goto L8; else goto L7;
  L6:;
#line 894
    dx = (-dx);
    goto L5;
  L7:;
    if (Actor_actors[i].actorType == 2) goto L14; else goto L9;
  L8:;
#line 895
    dy = (-dy);
    goto L7;
  L9:;
    goto L3;
  L10:;
#line 898
    Actor_actors[i].goal = 5;
    goto L9;
  L11:;
    if (dy < radius) goto L10; else goto L9;
  L12:;
    if (dx < radius) goto L11; else goto L9;
  L13:;
    if (Actor_actors[i].race < 7) goto L12; else goto L9;
  L14:;
    if (Actor_actors[i].vitality > 0) goto L13; else goto L9;
}

static void GameState_HarvestNearby(void) {
    int32_t i;
    int32_t id;
    int32_t dx;
    int32_t dy;
    int32_t count;
    int picked;
    char itemName[31 + 1];
#line 908
    count = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= (WorldObj_objCount - 1)) goto L2; else goto L4;
  L2:;
    if (WorldObj_objects[i].status == 1) goto L7; else goto L8;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    i = 1;
    goto L63;
  L5:;
    goto L3;
  L6:;
#line 912
    dx = (WorldObj_objects[i].x - Actor_actors[0].absX);
#line 913
    dy = (WorldObj_objects[i].y - Actor_actors[0].absY);
    if (dx < 0) goto L11; else goto L10;
  L7:;
    if (WorldObj_objects[i].region == Assets_currentRegion) goto L6; else goto L9;
  L8:;
    if (WorldObj_objects[i].status == 5) goto L7; else goto L5;
  L9:;
    if (WorldObj_objects[i].region == (-1)) goto L6; else goto L5;
  L10:;
    if (dy < 0) goto L13; else goto L12;
  L11:;
#line 914
    dx = (-dx);
    goto L10;
  L12:;
    if (dx < 100) goto L16; else goto L14;
  L13:;
#line 915
    dy = (-dy);
    goto L12;
  L14:;
    goto L5;
  L15:;
#line 917
    id = WorldObj_objects[i].objId;
    picked = 1;
    if ((id == 13)) goto L19;
    if ((id == 14) || (id == 15) || (id == 16)) goto L20;
    if ((id == 17)) goto L21;
    if ((id == 18)) goto L22;
    if ((id == 19)) goto L23;
    if ((id == 22)) goto L24;
    if ((id == 23)) goto L25;
    if ((id == 24)) goto L26;
    if ((id == 25)) goto L27;
    if ((id == 26)) goto L28;
    if ((id == 11)) goto L29;
    if ((id == 8)) goto L30;
    if ((id == 9)) goto L31;
    if ((id == 10)) goto L32;
    if ((id == 12)) goto L33;
    if ((id == 114)) goto L34;
    if ((id == 145)) goto L35;
    if ((id == 148)) goto L36;
    if ((id == 149)) goto L37;
    if ((id == 151)) goto L38;
    if ((id == 153)) goto L39;
    if ((id == 154)) goto L40;
    if ((id == 242)) goto L41;
    if ((id == 27)) goto L42;
    if ((id == 138)) goto L43;
    if ((id == 139)) goto L44;
    if ((id == 140)) goto L45;
    if ((id == 155)) goto L46;
    if ((id == 156)) goto L47;
    if ((id == 157)) goto L48;
    if ((id == 158)) goto L49;
    if ((id == 159)) goto L50;
    if ((id == 160)) goto L51;
    if ((id == 161)) goto L52;
    if ((id == 162)) goto L53;
    if ((id == 163)) goto L54;
    if ((id == 164)) goto L55;
    if ((id == 165)) goto L56;
    if ((id == 166)) goto L57;
    if ((id == 167)) goto L58;
    if ((id == 168)) goto L59;
    if ((id == 169)) goto L60;
    goto L18;
  L16:;
    if (dy < 100) goto L15; else goto L14;
  L17:;
    if (picked) goto L62; else goto L61;
  L18:;
#line 946
    picked = 0;
    goto L17;
  L19:;
#line 919
    Brothers_AddWealth(50);
    goto L17;
  L20:;
#line 920
    GameState_ContainerLoot();
    goto L17;
  L21:;
#line 921
    Brothers_GiveStuff(14);
    goto L17;
  L22:;
    Brothers_GiveStuff(9);
    goto L17;
  L23:;
    Brothers_GiveStuff(10);
    goto L17;
  L24:;
#line 922
    Brothers_GiveStuff(11);
    goto L17;
  L25:;
    Brothers_GiveStuff(13);
    goto L17;
  L26:;
    Brothers_GiveStuff(15);
    goto L17;
  L27:;
#line 923
    Brothers_GiveStuff(16);
    goto L17;
  L28:;
    Brothers_GiveStuff(20);
    goto L17;
  L29:;
#line 924
    Brothers_AddStuffN(8, 10);
    goto L17;
  L30:;
#line 925
    Brothers_GiveStuff(2);
    goto L17;
  L31:;
    Brothers_GiveStuff(1);
    goto L17;
  L32:;
    Brothers_GiveStuff(3);
    goto L17;
  L33:;
#line 926
    Brothers_GiveStuff(0);
    goto L17;
  L34:;
    Brothers_GiveStuff(18);
    goto L17;
  L35:;
    Brothers_GiveStuff(4);
    goto L17;
  L36:;
#line 927
    Brothers_GiveStuff(24);
    goto L17;
  L37:;
    Brothers_GiveStuff(25);
    goto L17;
  L38:;
    Brothers_GiveStuff(6);
    goto L17;
  L39:;
#line 928
    Brothers_GiveStuff(17);
    goto L17;
  L40:;
    Brothers_GiveStuff(21);
    goto L17;
  L41:;
    Brothers_GiveStuff(19);
    goto L17;
  L42:;
#line 929
    Brothers_SetStuff(5, 1);
    goto L17;
  L43:;
    Brothers_SetStuff(29, 1);
    goto L17;
  L44:;
#line 930
    Brothers_SetStuff(22, 1);
    goto L17;
  L45:;
    Brothers_SetStuff(30, 1);
    goto L17;
  L46:;
    Brothers_SetStuff(7, 1);
    goto L17;
  L47:;
#line 931
    Brothers_GiveStuff(31);
    goto L17;
  L48:;
#line 932
    Brothers_GiveStuff(32);
    goto L17;
  L49:;
#line 933
    Brothers_GiveStuff(33);
    goto L17;
  L50:;
#line 934
    Brothers_GiveStuff(34);
    goto L17;
  L51:;
#line 935
    Brothers_GiveStuff(35);
    goto L17;
  L52:;
#line 936
    Brothers_GiveStuff(36);
    goto L17;
  L53:;
#line 937
    Brothers_GiveStuff(37);
    goto L17;
  L54:;
#line 938
    Brothers_GiveStuff(38);
    goto L17;
  L55:;
#line 939
    Brothers_GiveStuff(39);
    goto L17;
  L56:;
#line 940
    Brothers_GiveStuff(40);
    goto L17;
  L57:;
#line 941
    Brothers_GiveStuff(41);
    goto L17;
  L58:;
#line 942
    Brothers_GiveStuff(42);
    goto L17;
  L59:;
#line 943
    Brothers_GiveStuff(43);
    goto L17;
  L60:;
#line 944
    Brothers_GiveStuff(44);
    goto L17;
  L61:;
    goto L14;
  L62:;
#line 948
    WorldObj_objects[i].status = 2;
    (count++);
    goto L61;
  L63:;
    if (i <= (Actor_actorCount - 1)) goto L64; else goto L66;
  L64:;
    if (Actor_actors[i].actorType == 2) goto L68; else goto L67;
  L65:;
    i = (i + 1);
    goto L63;
  L66:;
#line 964
    Menu_SetOptions();
#line 965
    GameState_IntToStr(count, GameState_nameBuf, 31);
#line 966
    m2_Strings_Assign("Harvest gathered ", GameState_msgBuf, 79);
#line 967
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 968
    m2_Strings_Concat(GameState_msgBuf, " nearby items.", GameState_msgBuf, 79);
#line 969
    GameState_ShowMessage(GameState_msgBuf, 79);
    return;
  L67:;
    goto L65;
  L68:;
#line 954
    dx = (Actor_actors[i].absX - Actor_actors[0].absX);
#line 955
    dy = (Actor_actors[i].absY - Actor_actors[0].absY);
    if (dx < 0) goto L70; else goto L69;
  L69:;
    if (dy < 0) goto L72; else goto L71;
  L70:;
#line 956
    dx = (-dx);
    goto L69;
  L71:;
    if (dx < 100) goto L75; else goto L73;
  L72:;
#line 957
    dy = (-dy);
    goto L71;
  L73:;
    goto L67;
  L74:;
    if (GameState_TakeEnemyWeapon(i, itemName, 31)) goto L77; else goto L76;
  L75:;
    if (dy < 100) goto L74; else goto L73;
  L76:;
    if (GameState_TakeEnemyTreasure(i, itemName, 31)) goto L79; else goto L78;
  L77:;
#line 959
    (count++);
    goto L76;
  L78:;
    goto L73;
  L79:;
#line 960
    (count++);
    goto L78;
}

static void GameState_HandleSpell(int32_t optIdx) {
    (void)optIdx;
    if (optIdx < 5) goto L2; else goto L3;
  L1:;
    if (Brothers_HasStuff(((37 + optIdx) - 5))) goto L4; else goto L5;
  L2:;
#line 974
    Menu_GoMenu(0);
    return;
  L3:;
    if (optIdx > 12) goto L2; else goto L1;
  L4:;
    if ((optIdx == 5)) goto L8;
    if ((optIdx == 6)) goto L9;
    if ((optIdx == 7)) goto L10;
    if ((optIdx == 8)) goto L11;
    if ((optIdx == 9)) goto L12;
    if ((optIdx == 10)) goto L13;
    if ((optIdx == 11)) goto L14;
    if ((optIdx == 12)) goto L15;
    goto L7;
  L5:;
#line 976
    GameState_ShowMessage("You do not have that spell scroll.", 34);
    Menu_GoMenu(0);
    return;
  L6:;
#line 1026
    Menu_SetOptions();
    Menu_GoMenu(0);
    return;
  L7:;
#line 1024
    Menu_GoMenu(0);
    return;
  L8:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[32] < 1) goto L17; else goto L19;
  L9:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[32] < 2) goto L21; else goto L23;
  L10:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[36] < 1) goto L25; else goto L27;
  L11:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[35] < 1) goto L29; else goto L31;
  L12:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[34] < 1) goto L33; else goto L34;
  L13:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[32] < 2) goto L36; else goto L38;
  L14:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[31] < 1) goto L40; else goto L42;
  L15:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[32] < 1) goto L44; else goto L46;
  L16:;
    goto L6;
  L17:;
#line 981
    GameState_ShowMessage("Ward requires Wolfsbane and Yarrow.", 35);
    goto L16;
  L18:;
#line 982
    (Brothers_brothers[Brothers_activeBrother].stuff[32]--);
#line 983
    (Brothers_brothers[Brothers_activeBrother].stuff[34]--);
    (Combat_wardTimer += 600);
#line 984
    GameState_ShowMessage("A protective ward surrounds you.", 32);
    goto L16;
  L19:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[34] < 1) goto L17; else goto L18;
  L20:;
    goto L6;
  L21:;
#line 987
    GameState_ShowMessage("Freeze requires 2 Wolfsbane and Mugwort.", 40);
    goto L20;
  L22:;
#line 988
    (Brothers_brothers[Brothers_activeBrother].stuff[32] -= 2);
#line 989
    (Brothers_brothers[Brothers_activeBrother].stuff[33]--);
    (GameState_freezeTimer += 250);
#line 990
    GameState_ShowMessage("The enemies are frozen in time.", 31);
    goto L20;
  L23:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[33] < 1) goto L21; else goto L22;
  L24:;
    goto L6;
  L25:;
#line 993
    GameState_ShowMessage("Fire requires Bloodroot and Nightshade.", 39);
    goto L24;
  L26:;
#line 994
    (Brothers_brothers[Brothers_activeBrother].stuff[36]--);
#line 995
    (Brothers_brothers[Brothers_activeBrother].stuff[35]--);
#line 996
    GameState_DamageNearbyEnemies(12, 120);
    GameState_ShowMessage("Fire strikes nearby enemies.", 28);
    goto L24;
  L27:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[35] < 1) goto L25; else goto L26;
  L28:;
    goto L6;
  L29:;
#line 999
    GameState_ShowMessage("Fear requires Nightshade and Bloodroot.", 39);
    goto L28;
  L30:;
#line 1000
    (Brothers_brothers[Brothers_activeBrother].stuff[35]--);
#line 1001
    (Brothers_brothers[Brothers_activeBrother].stuff[36]--);
#line 1002
    GameState_FrightenNearbyEnemies(160);
    GameState_ShowMessage("Weaker enemies flee in fear.", 28);
    goto L28;
  L31:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[36] < 1) goto L29; else goto L30;
  L32:;
    goto L6;
  L33:;
#line 1004
    GameState_ShowMessage("Light requires Yarrow.", 22);
    goto L32;
  L34:;
#line 1005
    (Brothers_brothers[Brothers_activeBrother].stuff[34]--);
    (GameState_lightTimer += 760);
#line 1006
    GameState_ShowMessage("Everything is bathed in light.", 30);
    goto L32;
  L35:;
    goto L6;
  L36:;
#line 1009
    GameState_ShowMessage("Sanctuary requires 2 Wolfsbane and 2 Yarrow.", 44);
    goto L35;
  L37:;
#line 1010
    (Brothers_brothers[Brothers_activeBrother].stuff[32] -= 2);
#line 1011
    (Brothers_brothers[Brothers_activeBrother].stuff[34] -= 2);
    (GameState_sanctuaryTimer += 900);
#line 1012
    GameState_ShowMessage("Sanctuary prevents new enemy encounters.", 40);
    goto L35;
  L38:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[34] < 2) goto L36; else goto L37;
  L39:;
    goto L6;
  L40:;
#line 1015
    GameState_ShowMessage("Harvest requires Mandrake and Mugwort.", 38);
    goto L39;
  L41:;
#line 1016
    (Brothers_brothers[Brothers_activeBrother].stuff[31]--);
#line 1017
    (Brothers_brothers[Brothers_activeBrother].stuff[33]--);
    GameState_HarvestNearby();
    goto L39;
  L42:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[33] < 1) goto L40; else goto L41;
  L43:;
    goto L6;
  L44:;
#line 1020
    GameState_ShowMessage("Heal requires Wolfsbane and Mandrake.", 37);
    goto L43;
  L45:;
#line 1021
    (Brothers_brothers[Brothers_activeBrother].stuff[32]--);
#line 1022
    (Brothers_brothers[Brothers_activeBrother].stuff[31]--);
#line 1023
    (Actor_actors[0].vitality += 15);
    GameState_ShowMessage("Heal restores 15 health.", 24);
    goto L43;
  L46:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[31] < 1) goto L44; else goto L45;
}

static void GameState_HandleStudy(int32_t optIdx) {
    (void)optIdx;
    if ((optIdx == 5)) goto L3;
    if ((optIdx == 6)) goto L4;
    if ((optIdx == 7)) goto L5;
    if ((optIdx == 8)) goto L6;
    if ((optIdx == 9)) goto L7;
    if ((optIdx == 10)) goto L8;
    if ((optIdx == 11)) goto L9;
    if ((optIdx == 12)) goto L10;
    goto L2;
  L1:;
    return;
  L2:;
    goto L1;
  L3:;
#line 1032
    GameState_ShowMessage("Ward: Wolfsbane + Yarrow. Reduces incoming damage.", 50);
    goto L1;
  L4:;
#line 1033
    GameState_ShowMessage("Freeze: 2 Wolfsbane + Mugwort. Briefly stops enemies.", 53);
    goto L1;
  L5:;
#line 1034
    GameState_ShowMessage("Fire: Bloodroot + Nightshade. Damages nearby enemies.", 53);
    goto L1;
  L6:;
#line 1035
    GameState_ShowMessage("Fear: Nightshade + Bloodroot. Makes weaker enemies flee.", 56);
    goto L1;
  L7:;
#line 1036
    GameState_ShowMessage("Light: Yarrow. Illuminates dark areas.", 38);
    goto L1;
  L8:;
#line 1037
    GameState_ShowMessage("Sanctuary: 2 Wolfsbane + 2 Yarrow. Prevents new spawns.", 55);
    goto L1;
  L9:;
#line 1038
    GameState_ShowMessage("Harvest: Mandrake + Mugwort. Takes nearby collectibles.", 55);
    goto L1;
  L10:;
#line 1039
    GameState_ShowMessage("Heal: Wolfsbane + Mandrake. Restores 15 health.", 47);
    goto L1;
}

static void GameState_ShowHerbCount(char *name, uint32_t name_high, char *properties, uint32_t properties_high, int32_t stuffIdx) {
    (void)name;
    (void)name_high;
    (void)properties;
    (void)properties_high;
    (void)stuffIdx;
    char numStr[15 + 1];
#line 1046
    m2_Strings_Assign(name, GameState_msgBuf, 79);
#line 1047
    m2_Strings_Concat(GameState_msgBuf, ": ", GameState_msgBuf, 79);
#line 1048
    GameState_IntToStr(Brothers_brothers[Brothers_activeBrother].stuff[stuffIdx], numStr, 15);
#line 1049
    m2_Strings_Concat(GameState_msgBuf, numStr, GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, ". ", GameState_msgBuf, 79);
#line 1050
    m2_Strings_Concat(GameState_msgBuf, properties, GameState_msgBuf, 79);
#line 1051
    GameState_ShowMessage(GameState_msgBuf, 79);
    return;
}

static void GameState_HandleHerbs(int32_t optIdx) {
    (void)optIdx;
    if ((optIdx == 5)) goto L3;
    if ((optIdx == 6)) goto L4;
    if ((optIdx == 7)) goto L5;
    if ((optIdx == 8)) goto L6;
    if ((optIdx == 9)) goto L7;
    if ((optIdx == 10)) goto L8;
    goto L2;
  L1:;
    return;
  L2:;
    goto L1;
  L3:;
#line 1057
    GameState_ShowHerbCount("Mandrake", 8, "Healing, growth, vitality.", 26, 31);
    goto L1;
  L4:;
#line 1058
    GameState_ShowHerbCount("Wolfsbane", 9, "Protection, suppression.", 24, 32);
    goto L1;
  L5:;
#line 1059
    GameState_ShowHerbCount("Mugwort", 7, "Perception, dreams, movement.", 29, 33);
    goto L1;
  L6:;
#line 1060
    GameState_ShowHerbCount("Yarrow", 6, "Light, safety, navigation.", 26, 34);
    goto L1;
  L7:;
#line 1061
    GameState_ShowHerbCount("Nightshade", 10, "Poison, fear, curses.", 21, 35);
    goto L1;
  L8:;
#line 1062
    GameState_ShowHerbCount("Bloodroot", 9, "Direct damage, aggressive magic.", 32, 36);
    goto L1;
}

static void GameState_HandleCamp(void) {
    if (Assets_currentRegion >= 8) goto L2; else goto L3;
  L1:;
#line 1084
    Menu_GoMenu(0);
    return;
  L2:;
#line 1070
    GameState_ShowMessage("You can only camp in the wild.", 30);
    goto L1;
  L3:;
    if (GameState_battleFlag) goto L4; else goto L5;
  L4:;
#line 1072
    GameState_ShowMessage("No time for that now!", 21);
    goto L1;
  L5:;
    if (GameState_fatigue < 50) goto L6; else goto L7;
  L6:;
#line 1074
    GameState_ShowMessage("% is not sleepy.", 16);
    goto L1;
  L7:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[24] < 2) goto L8; else goto L9;
  L8:;
#line 1076
    GameState_ShowMessage("You need two apples to camp.", 28);
    goto L1;
  L9:;
#line 1078
    (Brothers_brothers[Brothers_activeBrother].stuff[24] -= 2);
#line 1079
    GameState_sleepInBed = 0;
#line 1080
    Actor_actors[0].state = 23;
#line 1081
    GameState_ShowMessage("% makes camp and settles down to sleep.", 39);
#line 1082
    Menu_SetOptions();
    goto L1;
}

static void GameState_HandleEat(void) {
    if (GameState_potionCooldown != 0) goto L2; else goto L1;
  L1:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[24] > 0) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    return;
  L4:;
#line 1091
    (Brothers_brothers[Brothers_activeBrother].stuff[24]--);
    if (GameState_hunger > 30) goto L7; else goto L8;
  L5:;
    if (Items_UseItem(2)) goto L9; else goto L10;
  L6:;
#line 1093
    Narration_Event(37);
#line 1094
    Menu_SetOptions();
#line 1095
    GameState_potionCooldown = 30;
    goto L3;
  L7:;
#line 1092
    (GameState_hunger -= 30);
    goto L6;
  L8:;
    GameState_hunger = 0;
    goto L6;
  L9:;
#line 1097
    (Actor_actors[0].vitality += 10);
    if (Actor_actors[0].vitality > (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4))) goto L12; else goto L11;
  L10:;
#line 1103
    GameState_ShowMessage("No food!", 8);
    GameState_potionCooldown = 30;
    goto L3;
  L11:;
    if (GameState_hunger > 30) goto L14; else goto L15;
  L12:;
#line 1099
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
    goto L11;
  L13:;
#line 1101
    GameState_ShowMessage("You eat some food.", 18);
    GameState_potionCooldown = 30;
    goto L3;
  L14:;
#line 1100
    (GameState_hunger -= 30);
    goto L13;
  L15:;
    GameState_hunger = 0;
    goto L13;
}

static void GameState_HandleMenuClick(int32_t mx, int32_t my) {
    (void)mx;
    (void)my;
    static const int32_t HudW = 640;
    int32_t hx;
    int32_t hy;
    int32_t col;
    int32_t row;
    int32_t itemIdx;
    int32_t optIdx;
#line 1111
    hx = m2_div((mx * 640), (320 * 3));
#line 1112
    hy = m2_div((my - (143 * 3)), 3);
    if (hy < 2) goto L2; else goto L1;
  L1:;
    if (hx < 430) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (hx >= (430 + (52 * 2))) goto L6; else goto L5;
  L4:;
    return;
  L5:;
#line 1116
    col = m2_div((hx - 430), 52);
#line 1117
    row = m2_div((hy - 2), 9);
    if (row < 0) goto L8; else goto L7;
  L6:;
    return;
  L7:;
    if (row > 5) goto L10; else goto L9;
  L8:;
    return;
  L9:;
#line 1120
    itemIdx = ((row * 2) + col);
    if (itemIdx >= Menu_optionCount) goto L12; else goto L11;
  L10:;
    return;
  L11:;
#line 1122
    optIdx = Menu_realOptions[itemIdx];
    if (optIdx < 0) goto L14; else goto L13;
  L12:;
    return;
  L13:;
    if (optIdx < 5) goto L16; else goto L15;
  L14:;
    return;
  L15:;
    if ((Menu_cmode == 0)) goto L22;
    if ((Menu_cmode == 1)) goto L23;
    if ((Menu_cmode == 2)) goto L24;
    if ((Menu_cmode == 3)) goto L25;
    if ((Menu_cmode == 4)) goto L26;
    if ((Menu_cmode == 6)) goto L27;
    if ((Menu_cmode == 7)) goto L28;
    if ((Menu_cmode == 8)) goto L29;
    if ((Menu_cmode == 5)) goto L30;
    if ((Menu_cmode == 9)) goto L31;
    if ((Menu_cmode == 10)) goto L32;
    if ((Menu_cmode == 11)) goto L33;
    if ((Menu_cmode == 12)) goto L34;
    if ((Menu_cmode == 13)) goto L35;
    if ((Menu_cmode == 14)) goto L36;
    if ((Menu_cmode == 15)) goto L37;
    if ((Menu_cmode == 16)) goto L38;
    if ((Menu_cmode == 17)) goto L39;
    if ((Menu_cmode == 18)) goto L40;
    if ((Menu_cmode == 19)) goto L41;
    goto L21;
  L16:;
    if (optIdx == 3) goto L18; else goto L19;
  L17:;
    return;
  L18:;
#line 1125
    Menu_GoMenu(14);
    goto L17;
  L19:;
#line 1126
    Menu_GoMenu(optIdx);
    goto L17;
  L20:;
    return;
  L21:;
    goto L20;
  L22:;
    if ((optIdx == 5)) goto L44;
    if ((optIdx == 6)) goto L45;
    if ((optIdx == 7)) goto L46;
    if ((optIdx == 8)) goto L47;
    if ((optIdx == 9)) goto L48;
    goto L43;
  L23:;
#line 1134
    GameState_HandleMagic(optIdx);
    goto L20;
  L24:;
    if ((optIdx == 5)) goto L51;
    if ((optIdx == 6)) goto L52;
    if ((optIdx == 7)) goto L53;
    goto L50;
  L25:;
#line 1138
    GameState_HandleBuy(optIdx);
    goto L20;
  L26:;
    if ((optIdx == 5)) goto L56;
    if ((optIdx == 6)) goto L57;
    if ((optIdx == 7)) goto L58;
    if ((optIdx == 8)) goto L59;
    if ((optIdx == 9)) goto L60;
    if ((optIdx == 10)) goto L61;
    goto L55;
  L27:;
#line 1145
    GameState_HandleKeys(optIdx);
    goto L20;
  L28:;
#line 1146
    GameState_HandleGive(optIdx);
    goto L20;
  L29:;
    if ((optIdx == 5) || (optIdx == 6) || (optIdx == 7) || (optIdx == 8) || (optIdx == 9)) goto L64;
    if ((optIdx == 10)) goto L65;
    if ((optIdx == 11)) goto L66;
    if ((optIdx == 12)) goto L67;
    if ((optIdx == 13)) goto L68;
    if ((optIdx == 14)) goto L69;
    goto L63;
  L30:;
    if (optIdx == 5) goto L83; else goto L84;
  L31:;
    if (optIdx >= 5) goto L89; else goto L87;
  L32:;
#line 1208
    GameState_HandleSell(optIdx);
    goto L20;
  L33:;
#line 1209
    GameState_HandleSpell(optIdx);
    goto L20;
  L34:;
#line 1210
    GameState_HandleStudy(optIdx);
    goto L20;
  L35:;
#line 1211
    GameState_HandleHerbs(optIdx);
    goto L20;
  L36:;
    if ((optIdx == 5)) goto L99;
    if ((optIdx == 6)) goto L100;
    if ((optIdx == 7)) goto L101;
    goto L98;
  L37:;
    if ((optIdx == 5)) goto L104;
    if ((optIdx == 6)) goto L105;
    goto L103;
  L38:;
#line 1218
    GameState_HandleHerbBuy(optIdx);
    goto L20;
  L39:;
#line 1219
    GameState_HandleHerbSell(optIdx);
    goto L20;
  L40:;
#line 1220
    GameState_HandleScrollBuy(optIdx);
    goto L20;
  L41:;
#line 1221
    GameState_HandleAppleBuy(optIdx);
    goto L20;
  L42:;
    goto L20;
  L43:;
    goto L42;
  L44:;
#line 1131
    GameState_ShowInventory();
    goto L42;
  L45:;
    GameState_HandleWorldPickup();
    goto L42;
  L46:;
    GameState_HandleLook();
    goto L42;
  L47:;
#line 1132
    Menu_GoMenu(8);
    goto L42;
  L48:;
    Menu_GoMenu(15);
    goto L42;
  L49:;
    goto L20;
  L50:;
    goto L49;
  L51:;
#line 1136
    GameState_HandleYell();
    goto L49;
  L52:;
    GameState_HandleTalk();
    goto L49;
  L53:;
    GameState_HandleTalk();
    goto L49;
  L54:;
    goto L20;
  L55:;
    goto L54;
  L56:;
#line 1140
    GameState_TogglePause();
    goto L54;
  L57:;
    GameState_ToggleMusic();
    goto L54;
  L58:;
    goto L54;
  L59:;
#line 1141
    GameState_saveMode = 1;
    Menu_GoMenu(9);
    goto L54;
  L60:;
#line 1142
    GameState_saveMode = 0;
    Menu_GoMenu(9);
    goto L54;
  L61:;
#line 1143
    GameState_running = 0;
    goto L54;
  L62:;
    goto L20;
  L63:;
#line 1178
    Menu_GoMenu(0);
    goto L62;
  L64:;
    if (Brothers_HasWeapon((optIdx - 4))) goto L71; else goto L72;
  L65:;
#line 1157
    Menu_GoMenu(0);
    goto L62;
  L66:;
    if (Brothers_HasStuff(6)) goto L74; else goto L75;
  L67:;
#line 1172
    Menu_GoMenu(6);
    goto L62;
  L68:;
#line 1174
    Menu_GoMenu(0);
    goto L62;
  L69:;
#line 1176
    Menu_GoMenu(0);
    goto L62;
  L70:;
#line 1155
    Menu_GoMenu(0);
    goto L62;
  L71:;
#line 1150
    Actor_actors[0].weapon = (optIdx - 4);
#line 1151
    GameState_WeaponName((optIdx - 4), GameState_nameBuf, 31);
#line 1152
    m2_Strings_Assign("Equipped ", GameState_msgBuf, 79);
    m2_Strings_Concat(GameState_msgBuf, GameState_nameBuf, GameState_msgBuf, 79);
#line 1153
    m2_Strings_Concat(GameState_msgBuf, ".", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L70;
  L72:;
#line 1154
    GameState_ShowMessage("% doesn't have one.", 19);
    goto L70;
  L73:;
#line 1170
    Menu_GoMenu(0);
    goto L62;
  L74:;
    if (Actor_actors[0].absX > 11194) goto L81; else goto L78;
  L75:;
#line 1169
    GameState_ShowMessage("% doesn't have one.", 19);
    goto L73;
  L76:;
    goto L73;
  L77:;
#line 1163
    GameState_ShowMessage("Nothing happens here.", 21);
    goto L76;
  L78:;
#line 1165
    Carrier_SpawnTurtle();
#line 1166
    Encounter_MoveExtent(1, Actor_actors[3].absX, Actor_actors[3].absY);
#line 1167
    GameState_ShowMessage("The turtle hears your call!", 27);
    goto L76;
  L79:;
    if (Actor_actors[0].absY < 16208) goto L77; else goto L78;
  L80:;
    if (Actor_actors[0].absY > 10205) goto L79; else goto L78;
  L81:;
    if (Actor_actors[0].absX < 21373) goto L80; else goto L78;
  L82:;
    goto L20;
  L83:;
#line 1182
    GameState_saveMode = 1;
    Menu_GoMenu(9);
    goto L82;
  L84:;
    if (optIdx == 6) goto L85; else goto L86;
  L85:;
#line 1184
    GameState_running = 0;
    goto L82;
  L86:;
#line 1185
    Menu_GoMenu(0);
    goto L82;
  L87:;
#line 1207
    Menu_GoMenu(0);
    goto L20;
  L88:;
    if (GameState_saveMode) goto L91; else goto L92;
  L89:;
    if (optIdx <= 12) goto L88; else goto L87;
  L90:;
    goto L87;
  L91:;
#line 1189
    Brothers_SaveBrotherState();
    if (Quest_SaveGame((optIdx - 5), GameState_dayNight, GameState_fatigue, GameState_hunger, GameState_cycle, GameState_lightTimer, GameState_secretTimer, GameState_freezeTimer, Combat_wardTimer, GameState_sanctuaryTimer)) goto L94; else goto L93;
  L92:;
    if (Quest_LoadGame((optIdx - 5), &GameState_dayNight, &GameState_fatigue, &GameState_hunger, &GameState_cycle, &GameState_lightTimer, &GameState_secretTimer, &GameState_freezeTimer, &Combat_wardTimer, &GameState_sanctuaryTimer)) goto L96; else goto L95;
  L93:;
    goto L90;
  L94:;
    goto L93;
  L95:;
    goto L90;
  L96:;
#line 1197
    GameState_dayPeriod = m2_div(GameState_dayNight, 2000);
#line 1198
    GameState_viewStatus = 0;
#line 1199
    GameState_battleFlag = 0;
#line 1200
    GameState_prevBattle = 0;
#line 1201
    GameState_aftermathDone = 0;
#line 1202
    WorldObj_revealHidden = GameState_secretTimer > 0;
#line 1203
    Menu_SetOptions();
    goto L95;
  L97:;
    goto L20;
  L98:;
    goto L97;
  L99:;
#line 1213
    GameState_OpenBuyMenu();
    goto L97;
  L100:;
    GameState_OpenSellMenu();
    goto L97;
  L101:;
    Menu_GoMenu(7);
    goto L97;
  L102:;
    goto L20;
  L103:;
    goto L102;
  L104:;
#line 1216
    GameState_HandleCamp();
    goto L102;
  L105:;
    GameState_HandleEat();
    Menu_GoMenu(0);
    goto L102;
}

static void GameState_HandleWorldPickup(void) {
    int32_t id;
#line 1228
    id = WorldObj_CheckObjectPickup(Actor_actors[0].absX, Actor_actors[0].absY);
    if (id >= 0) goto L2; else goto L3;
  L1:;
    return;
  L2:;
    if ((id == 13)) goto L6;
    if ((id == 14)) goto L7;
    if ((id == 15)) goto L8;
    if ((id == 16)) goto L9;
    if ((id == 17)) goto L10;
    if ((id == 18)) goto L11;
    if ((id == 19)) goto L12;
    if ((id == 20)) goto L13;
    if ((id == 22)) goto L14;
    if ((id == 23)) goto L15;
    if ((id == 24)) goto L16;
    if ((id == 25)) goto L17;
    if ((id == 26)) goto L18;
    if ((id == 11)) goto L19;
    if ((id == 8)) goto L20;
    if ((id == 9)) goto L21;
    if ((id == 10)) goto L22;
    if ((id == 12)) goto L23;
    if ((id == 102)) goto L24;
    if ((id == 114)) goto L25;
    if ((id == 145)) goto L26;
    if ((id == 148)) goto L27;
    if ((id == 151)) goto L28;
    if ((id == 153)) goto L29;
    if ((id == 154)) goto L30;
    if ((id == 156)) goto L31;
    if ((id == 157)) goto L32;
    if ((id == 158)) goto L33;
    if ((id == 159)) goto L34;
    if ((id == 160)) goto L35;
    if ((id == 161)) goto L36;
    if ((id == 162)) goto L37;
    if ((id == 163)) goto L38;
    if ((id == 164)) goto L39;
    if ((id == 165)) goto L40;
    if ((id == 166)) goto L41;
    if ((id == 167)) goto L42;
    if ((id == 168)) goto L43;
    if ((id == 169)) goto L44;
    if ((id == 242)) goto L45;
    if ((id == 27)) goto L46;
    if ((id == 138)) goto L47;
    if ((id == 139)) goto L48;
    if ((id == 140)) goto L49;
    if ((id == 155)) goto L50;
    if ((id == 149)) goto L51;
    if ((id == 28)) goto L52;
    goto L5;
  L3:;
#line 1284
    GameState_SearchNearbyCorpses();
    goto L1;
  L4:;
#line 1283
    Menu_SetOptions();
    goto L1;
  L5:;
#line 1282
    GameState_ShowMessage("Found something!", 16);
    goto L4;
  L6:;
#line 1231
    GameState_ShowMessage("Found 50 gold pieces!", 21);
    Brothers_AddWealth(50);
    goto L4;
  L7:;
#line 1232
    GameState_ShowMessage("Opened a brass urn.", 19);
    GameState_ContainerLoot();
    goto L4;
  L8:;
#line 1233
    GameState_ShowMessage("Opened a chest.", 15);
    GameState_ContainerLoot();
    goto L4;
  L9:;
#line 1234
    GameState_ShowMessage("Opened some sacks.", 18);
    GameState_ContainerLoot();
    goto L4;
  L10:;
#line 1235
    GameState_ShowMessage("Found a gold ring!", 18);
    Brothers_GiveStuff(14);
    goto L4;
  L11:;
#line 1236
    GameState_ShowMessage("Found a blue stone!", 19);
    Brothers_GiveStuff(9);
    goto L4;
  L12:;
#line 1237
    GameState_ShowMessage("Found a green jewel!", 20);
    Brothers_GiveStuff(10);
    goto L4;
  L13:;
#line 1238
    Narration_Event(17);
    if (Assets_currentRegion > 7) goto L54; else goto L55;
  L14:;
#line 1241
    GameState_ShowMessage("Found a vial!", 13);
    Brothers_GiveStuff(11);
    goto L4;
  L15:;
#line 1242
    GameState_ShowMessage("Found a totem!", 14);
    Brothers_GiveStuff(13);
    goto L4;
  L16:;
#line 1243
    GameState_ShowMessage("Found a skull!", 14);
    Brothers_GiveStuff(15);
    goto L4;
  L17:;
#line 1244
    GameState_ShowMessage("Found a gold key!", 17);
    Brothers_GiveStuff(16);
    goto L4;
  L18:;
#line 1245
    GameState_ShowMessage("Found a grey key!", 17);
    Brothers_GiveStuff(20);
    goto L4;
  L19:;
#line 1246
    GameState_ShowMessage("Found a quiver of arrows!", 25);
    Brothers_AddStuffN(8, 10);
    goto L4;
  L20:;
#line 1247
    GameState_ShowMessage("Found a sword!", 14);
    Brothers_GiveStuff(2);
    goto L4;
  L21:;
#line 1248
    GameState_ShowMessage("Found a mace!", 13);
    Brothers_GiveStuff(1);
    goto L4;
  L22:;
#line 1249
    GameState_ShowMessage("Found a bow!", 12);
    Brothers_GiveStuff(3);
    goto L4;
  L23:;
#line 1250
    GameState_ShowMessage("Found a dirk!", 13);
    Brothers_GiveStuff(0);
    goto L4;
  L24:;
    goto L4;
  L25:;
#line 1252
    GameState_ShowMessage("Found a blue key!", 17);
    Brothers_GiveStuff(18);
    goto L4;
  L26:;
#line 1253
    GameState_ShowMessage("Found a magic wand!", 19);
    Brothers_GiveStuff(4);
    goto L4;
  L27:;
#line 1254
    GameState_ShowMessage("Found some fruit!", 17);
    Brothers_GiveStuff(24);
    goto L4;
  L28:;
#line 1255
    GameState_ShowMessage("Found a shell!", 14);
    Brothers_GiveStuff(6);
    goto L4;
  L29:;
#line 1256
    GameState_ShowMessage("Found a green key!", 18);
    Brothers_GiveStuff(17);
    goto L4;
  L30:;
#line 1257
    GameState_ShowMessage("Found a white key!", 18);
    Brothers_GiveStuff(21);
    goto L4;
  L31:;
#line 1258
    GameState_ShowMessage("Found Mandrake!", 15);
    Brothers_GiveStuff(31);
    goto L4;
  L32:;
#line 1259
    GameState_ShowMessage("Found Wolfsbane!", 16);
    Brothers_GiveStuff(32);
    goto L4;
  L33:;
#line 1260
    GameState_ShowMessage("Found Mugwort!", 14);
    Brothers_GiveStuff(33);
    goto L4;
  L34:;
#line 1261
    GameState_ShowMessage("Found Yarrow!", 13);
    Brothers_GiveStuff(34);
    goto L4;
  L35:;
#line 1262
    GameState_ShowMessage("Found Nightshade!", 17);
    Brothers_GiveStuff(35);
    goto L4;
  L36:;
#line 1263
    GameState_ShowMessage("Found Bloodroot!", 16);
    Brothers_GiveStuff(36);
    goto L4;
  L37:;
#line 1264
    GameState_ShowMessage("Found the Ward scroll!", 22);
    Brothers_GiveStuff(37);
    goto L4;
  L38:;
#line 1265
    GameState_ShowMessage("Found the Freeze scroll!", 24);
    Brothers_GiveStuff(38);
    goto L4;
  L39:;
#line 1266
    GameState_ShowMessage("Found the Fire scroll!", 22);
    Brothers_GiveStuff(39);
    goto L4;
  L40:;
#line 1267
    GameState_ShowMessage("Found the Fear scroll!", 22);
    Brothers_GiveStuff(40);
    goto L4;
  L41:;
#line 1268
    GameState_ShowMessage("Found the Light scroll!", 23);
    Brothers_GiveStuff(41);
    goto L4;
  L42:;
#line 1269
    GameState_ShowMessage("Found the Sanctuary scroll!", 27);
    Brothers_GiveStuff(42);
    goto L4;
  L43:;
#line 1270
    GameState_ShowMessage("Found the Harvest scroll!", 25);
    Brothers_GiveStuff(43);
    goto L4;
  L44:;
#line 1271
    GameState_ShowMessage("Found the Heal scroll!", 22);
    Brothers_GiveStuff(44);
    goto L4;
  L45:;
#line 1272
    GameState_ShowMessage("Found a red key!", 16);
    Brothers_GiveStuff(19);
    goto L4;
  L46:;
#line 1273
    GameState_ShowMessage("% found the Golden Lasso!", 25);
    Brothers_SetStuff(5, 1);
    goto L4;
  L47:;
#line 1274
    GameState_ShowMessage("% found the King's Bone!", 24);
    Brothers_SetStuff(29, 1);
    goto L4;
  L48:;
#line 1275
    GameState_ShowMessage("% found the Talisman!", 21);
    Brothers_SetStuff(22, 1);
#line 1276
    GameState_ShowMessage("The quest is complete!", 22);
    goto L4;
  L49:;
#line 1277
    GameState_ShowMessage("% found a Shard!", 16);
    Brothers_SetStuff(30, 1);
    goto L4;
  L50:;
#line 1278
    GameState_ShowMessage("% found the Sun Stone!", 22);
    Brothers_SetStuff(7, 1);
    goto L4;
  L51:;
#line 1279
    GameState_ShowMessage("Found a gold statue!", 20);
    Brothers_GiveStuff(25);
    goto L4;
  L52:;
#line 1280
    GameState_ShowMessage("% found his brother's bones.", 28);
#line 1281
    GameState_InheritBrotherItems();
    goto L4;
  L53:;
    goto L4;
  L54:;
#line 1239
    Narration_Event(19);
    goto L53;
  L55:;
#line 1240
    Narration_Event(18);
    goto L53;
}

static void GameState_HandleTalk(void) {
    char speech[127 + 1];
    if (Carrier_TalkToCarrier(speech, 127)) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 1290
    GameState_ShowMessage(speech, 127);
    goto L1;
  L3:;
    if (NPC_TalkToNPC(Actor_actors[0].absX, Actor_actors[0].absY, speech, 127)) goto L4; else goto L5;
  L4:;
    if (speech[0] != '\0') goto L7; else goto L6;
  L5:;
#line 1293
    GameState_ShowMessage("Nobody to talk to here.", 23);
    goto L1;
  L6:;
    goto L1;
  L7:;
#line 1292
    GameState_ShowMessage(speech, 127);
    goto L6;
}

static void GameState_HandleYell(void) {
#line 1300
    GameState_ShowMessage("Nobody seems to hear.", 21);
    return;
}

static int32_t GameState_NewXDir(int32_t d) {
    (void)d;
    if ((d == 0)) goto L3;
    if ((d == 1)) goto L4;
    if ((d == 2)) goto L5;
    if ((d == 3)) goto L6;
    if ((d == 4)) goto L7;
    if ((d == 5)) goto L8;
    if ((d == 6)) goto L9;
    if ((d == 7)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 0;
  L4:;
    return 2;
  L5:;
    return 2;
  L6:;
    return 2;
  L7:;
    return 0;
  L8:;
    return (-2);
  L9:;
    return (-2);
  L10:;
    return (-2);
}

static int32_t GameState_NewYDir(int32_t d) {
    (void)d;
    if ((d == 0)) goto L3;
    if ((d == 1)) goto L4;
    if ((d == 2)) goto L5;
    if ((d == 3)) goto L6;
    if ((d == 4)) goto L7;
    if ((d == 5)) goto L8;
    if ((d == 6)) goto L9;
    if ((d == 7)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return (-2);
  L4:;
    return (-2);
  L5:;
    return 0;
  L6:;
    return 2;
  L7:;
    return 2;
  L8:;
    return 2;
  L9:;
    return 0;
  L10:;
    return (-2);
}

static void GameState_UpdateWitch(void) {
    int32_t i;
    int32_t dx;
    int32_t dy;
    int32_t dist;
    int32_t rng;
    int32_t pAngle;
    int32_t aDiff;
#line 1330
    GameState_witchFlag = 0;
    i = 1;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
    if (Actor_actors[i].actorType == 4) goto L9; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
#line 1336
    GameState_witchFlag = 1;
#line 1337
    dx = (Actor_actors[0].absX - Actor_actors[i].absX);
#line 1338
    dy = (Actor_actors[0].absY - Actor_actors[i].absY);
    if (dx < 0) goto L11; else goto L12;
  L7:;
    if (Actor_actors[i].state != 14) goto L6; else goto L5;
  L8:;
    if (Actor_actors[i].state != 15) goto L7; else goto L5;
  L9:;
    if (Actor_actors[i].race == 9) goto L8; else goto L5;
  L10:;
    if (dy < 0) goto L14; else goto L15;
  L11:;
#line 1339
    dist = (-dx);
    goto L10;
  L12:;
    dist = dx;
    goto L10;
  L13:;
    if (dx > 3) goto L23; else goto L22;
  L14:;
    if ((-dy) > dist) goto L17; else goto L16;
  L15:;
    if (dy > dist) goto L19; else goto L18;
  L16:;
    goto L13;
  L17:;
#line 1341
    dist = (-dy);
    goto L16;
  L18:;
    goto L13;
  L19:;
#line 1343
    dist = dy;
    goto L18;
  L20:;
#line 1359
    GameState_witchRng = ((GameState_witchRng * 1103515245) + 12345);
    if (GameState_witchRng < 0) goto L41; else goto L40;
  L21:;
#line 1347
    Actor_actors[i].facing = 1;
    goto L20;
  L22:;
    if (dx > 3) goto L26; else goto L25;
  L23:;
    if (dy < (-3)) goto L21; else goto L22;
  L24:;
#line 1348
    Actor_actors[i].facing = 3;
    goto L20;
  L25:;
    if (dx < (-3)) goto L29; else goto L28;
  L26:;
    if (dy > 3) goto L24; else goto L25;
  L27:;
#line 1349
    Actor_actors[i].facing = 5;
    goto L20;
  L28:;
    if (dx < (-3)) goto L32; else goto L31;
  L29:;
    if (dy > 3) goto L27; else goto L28;
  L30:;
#line 1350
    Actor_actors[i].facing = 7;
    goto L20;
  L31:;
    if (dx > 3) goto L33; else goto L34;
  L32:;
    if (dy < (-3)) goto L30; else goto L31;
  L33:;
#line 1351
    Actor_actors[i].facing = 2;
    goto L20;
  L34:;
    if (dx < (-3)) goto L35; else goto L36;
  L35:;
#line 1352
    Actor_actors[i].facing = 6;
    goto L20;
  L36:;
    if (dy < (-3)) goto L37; else goto L38;
  L37:;
#line 1353
    Actor_actors[i].facing = 0;
    goto L20;
  L38:;
    if (dy > 3) goto L39; else goto L20;
  L39:;
#line 1354
    Actor_actors[i].facing = 4;
    goto L20;
  L40:;
    if (m2_mod(m2_div(GameState_witchRng, 65536), 8) == 0) goto L43; else goto L42;
  L41:;
#line 1360
    GameState_witchRng = (-GameState_witchRng);
    goto L40;
  L42:;
    if (((uint32_t)(((uint32_t)(GameState_cycle))) & (uint32_t)(1)) == 0) goto L48; else goto L47;
  L43:;
    if (GameState_witchS1 > 0) goto L45; else goto L46;
  L44:;
    goto L42;
  L45:;
#line 1362
    GameState_witchDir = (-1);
    goto L44;
  L46:;
#line 1363
    GameState_witchDir = 1;
    goto L44;
  L47:;
    if (dist < 100) goto L54; else goto L53;
  L48:;
#line 1367
    (GameState_witchIndex += GameState_witchDir);
    if (GameState_witchIndex > 63) goto L50; else goto L51;
  L49:;
    goto L47;
  L50:;
#line 1368
    GameState_witchIndex = 0;
    goto L49;
  L51:;
    if (GameState_witchIndex < 0) goto L52; else goto L49;
  L52:;
#line 1369
    GameState_witchIndex = 63;
    goto L49;
  L53:;
    return;
  L54:;
    if (dx == 0) goto L58; else goto L57;
  L55:;
#line 1393
    aDiff = m2_mod(((GameState_witchIndex - pAngle) + 64), 64);
    if (aDiff > 32) goto L78; else goto L77;
  L56:;
#line 1382
    pAngle = GameState_witchIndex;
    goto L55;
  L57:;
    if (dx > 3) goto L61; else goto L60;
  L58:;
    if (dy == 0) goto L56; else goto L57;
  L59:;
#line 1383
    pAngle = 8;
    goto L55;
  L60:;
    if (dx > 3) goto L64; else goto L63;
  L61:;
    if (dy > 3) goto L59; else goto L60;
  L62:;
#line 1384
    pAngle = 24;
    goto L55;
  L63:;
    if (dx < (-3)) goto L67; else goto L66;
  L64:;
    if (dy < (-3)) goto L62; else goto L63;
  L65:;
#line 1385
    pAngle = 40;
    goto L55;
  L66:;
    if (dx < (-3)) goto L70; else goto L69;
  L67:;
    if (dy < (-3)) goto L65; else goto L66;
  L68:;
#line 1386
    pAngle = 56;
    goto L55;
  L69:;
    if (dx > 3) goto L71; else goto L72;
  L70:;
    if (dy > 3) goto L68; else goto L69;
  L71:;
#line 1387
    pAngle = 16;
    goto L55;
  L72:;
    if (dx < (-3)) goto L73; else goto L74;
  L73:;
#line 1388
    pAngle = 48;
    goto L55;
  L74:;
    if (dy > 3) goto L75; else goto L76;
  L75:;
#line 1389
    pAngle = 0;
    goto L55;
  L76:;
#line 1390
    pAngle = 32;
    goto L55;
  L77:;
    if (aDiff < 5) goto L80; else goto L79;
  L78:;
#line 1394
    aDiff = (64 - aDiff);
    goto L77;
  L79:;
    goto L53;
  L80:;
    if (((uint32_t)(((uint32_t)(GameState_cycle))) & (uint32_t)(7)) == 0) goto L82; else goto L81;
  L81:;
    goto L79;
  L82:;
#line 1397
    GameState_witchRng = ((GameState_witchRng * 1103515245) + 12345);
    if (GameState_witchRng < 0) goto L84; else goto L83;
  L83:;
#line 1399
    rng = (m2_mod(m2_div(GameState_witchRng, 65536), 2) + 1);
    if (Combat_wardTimer > 0) goto L86; else goto L85;
  L84:;
#line 1398
    GameState_witchRng = (-GameState_witchRng);
    goto L83;
  L85:;
#line 1401
    (Actor_actors[0].vitality -= rng);
    if (Actor_actors[0].vitality <= 0) goto L88; else goto L87;
  L86:;
#line 1400
    rng = m2_div((rng + 1), 2);
    goto L85;
  L87:;
    goto L81;
  L88:;
#line 1403
    Actor_actors[0].vitality = 0;
#line 1404
    Actor_actors[0].state = 14;
#line 1405
    Actor_actors[0].tactic = 7;
    goto L87;
}

static void GameState_CheckEnvironment(void) {
    int32_t sec;
    int32_t terrain;
    if (Actor_actors[0].state == 14) goto L2; else goto L3;
  L1:;
    if (Actor_actors[0].state == 22) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (Actor_actors[0].state == 15) goto L2; else goto L1;
  L4:;
    if (Encounter_xtype == 52) goto L30; else goto L28;
  L5:;
    if (Actor_actors[0].tactic < 30) goto L7; else goto L8;
  L6:;
    return;
  L7:;
#line 1427
    (Actor_actors[0].tactic++);
#line 1428
    Actor_actors[0].velX = m2_div((Actor_actors[0].velX * 3), 4);
#line 1429
    Actor_actors[0].velY = m2_div((Actor_actors[0].velY * 3), 4);
#line 1430
    Actor_actors[0].absX = (Actor_actors[0].absX + m2_div(Actor_actors[0].velX, 4));
#line 1431
    Actor_actors[0].absY = (Actor_actors[0].absY + m2_div(Actor_actors[0].velY, 4));
    goto L6;
  L8:;
#line 1436
    Actor_actors[0].state = 13;
#line 1437
    Actor_actors[0].velX = 0;
#line 1438
    Actor_actors[0].velY = 0;
    sec = 1;
    goto L9;
  L9:;
    if (sec <= 64) goto L10; else goto L12;
  L10:;
#line 1440
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY - sec));
    if (terrain >= 6) goto L16; else goto L15;
  L11:;
    sec = (sec + 1);
    goto L9;
  L12:;
#line 1460
    GameState_ShowMessage("The good fairy catches you!", 27);
    goto L6;
  L13:;
    goto L11;
  L14:;
#line 1442
    (Actor_actors[0].absY -= sec);
    sec = 64;
    goto L13;
  L15:;
#line 1444
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY + sec));
    if (terrain >= 6) goto L20; else goto L19;
  L16:;
    if (terrain <= 8) goto L14; else goto L15;
  L17:;
    goto L13;
  L18:;
#line 1446
    (Actor_actors[0].absY += sec);
    sec = 64;
    goto L17;
  L19:;
#line 1448
    terrain = Assets_GetTerrainAt((Actor_actors[0].absX + sec), Actor_actors[0].absY);
    if (terrain >= 6) goto L24; else goto L23;
  L20:;
    if (terrain <= 8) goto L18; else goto L19;
  L21:;
    goto L17;
  L22:;
#line 1450
    (Actor_actors[0].absX += sec);
    sec = 64;
    goto L21;
  L23:;
#line 1452
    terrain = Assets_GetTerrainAt((Actor_actors[0].absX - sec), Actor_actors[0].absY);
    if (terrain >= 6) goto L27; else goto L25;
  L24:;
    if (terrain <= 8) goto L22; else goto L23;
  L25:;
    goto L21;
  L26:;
#line 1454
    (Actor_actors[0].absX -= sec);
    sec = 64;
    goto L25;
  L27:;
    if (terrain <= 8) goto L26; else goto L25;
  L28:;
    if (Actor_actors[0].environ == 30) goto L36; else goto L35;
  L29:;
#line 1468
    terrain = Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY);
    if (terrain == 9) goto L32; else goto L31;
  L30:;
    if (Assets_currentRegion >= 8) goto L29; else goto L28;
  L31:;
    goto L28;
  L32:;
    if (Actor_actors[0].state != 22) goto L34; else goto L33;
  L33:;
    goto L31;
  L34:;
#line 1471
    Actor_actors[0].state = 22;
#line 1472
    Actor_actors[0].tactic = 0;
#line 1473
    Brothers_DecLuck(2);
    goto L33;
  L35:;
    if (World_camX > 8802) goto L44; else goto L39;
  L36:;
#line 1485
    sec = Assets_GetMapSector(Actor_actors[0].absX, Actor_actors[0].absY);
    if (sec == 181) goto L38; else goto L37;
  L37:;
    goto L35;
  L38:;
#line 1487
    Actor_actors[0].environ = 0;
#line 1488
    Actor_actors[0].absX = 4224;
#line 1489
    Actor_actors[0].absY = 34950;
#line 1490
    Assets_SwitchRegion(9);
#line 1491
    Actor_actorCount = 1;
#line 1492
    GameState_ShowMessage("The ground swallows you up!", 27);
    return;
  L39:;
    if (Actor_actors[0].environ == 30) goto L56; else goto L53;
  L40:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[23] > 0) goto L46; else goto L47;
  L41:;
    if (GameState_cheatGod) goto L39; else goto L40;
  L42:;
    if (World_camY < 29544) goto L41; else goto L39;
  L43:;
    if (World_camY > 24744) goto L42; else goto L39;
  L44:;
    if (World_camX < 13562) goto L43; else goto L39;
  L45:;
    goto L39;
  L46:;
#line 1506
    Actor_actors[0].environ = 0;
    goto L45;
  L47:;
    if (Actor_actors[0].environ > 15) goto L48; else goto L49;
  L48:;
#line 1508
    Actor_actors[0].vitality = 0;
#line 1509
    Actor_actors[0].state = 14;
#line 1510
    Actor_actors[0].tactic = 7;
#line 1511
    Narration_Event(27);
    goto L45;
  L49:;
    if (Actor_actors[0].environ > 2) goto L50; else goto L45;
  L50:;
#line 1513
    (Actor_actors[0].vitality--);
    if (Actor_actors[0].vitality <= 0) goto L52; else goto L51;
  L51:;
    goto L45;
  L52:;
#line 1515
    Actor_actors[0].vitality = 0;
#line 1516
    Actor_actors[0].state = 14;
#line 1517
    Actor_actors[0].tactic = 7;
#line 1518
    Narration_Event(27);
    goto L51;
  L53:;
    return;
  L54:;
#line 1527
    (Actor_actors[0].vitality--);
    if (Actor_actors[0].vitality <= 0) goto L58; else goto L59;
  L55:;
    if (GameState_cheatGod) goto L53; else goto L54;
  L56:;
    if (((uint32_t)(((uint32_t)(GameState_cycle))) & (uint32_t)(7)) == 0) goto L55; else goto L53;
  L57:;
    goto L53;
  L58:;
#line 1529
    Actor_actors[0].vitality = 0;
#line 1530
    Actor_actors[0].state = 14;
#line 1531
    Actor_actors[0].tactic = 7;
#line 1532
    Narration_Event(6);
    goto L57;
  L59:;
    if (((uint32_t)(((uint32_t)(GameState_cycle))) & (uint32_t)(31)) == 0) goto L60; else goto L57;
  L60:;
#line 1534
    GameState_ShowMessage("% is drowning!", 14);
    goto L57;
}

static void GameState_CheckBedTile(void) {
    int32_t sec;
    if (Assets_currentRegion != 8) goto L2; else goto L1;
  L1:;
#line 1545
    sec = Assets_GetSectorByte(Actor_actors[0].absX, Actor_actors[0].absY);
    if (sec == 161) goto L4; else goto L8;
  L2:;
#line 1544
    GameState_sleepWait = 0;
    return;
  L3:;
    return;
  L4:;
#line 1547
    (GameState_sleepWait++);
    if (GameState_sleepWait == 30) goto L10; else goto L9;
  L5:;
#line 1557
    GameState_sleepWait = 0;
    goto L3;
  L6:;
    if (sec == 53) goto L4; else goto L5;
  L7:;
    if (sec == 162) goto L4; else goto L6;
  L8:;
    if (sec == 52) goto L4; else goto L7;
  L9:;
    goto L3;
  L10:;
    if (GameState_fatigue < 50) goto L12; else goto L13;
  L11:;
    goto L9;
  L12:;
#line 1549
    Narration_Event(25);
    goto L11;
  L13:;
#line 1551
    Narration_Event(26);
#line 1552
    Actor_actors[0].absY = ((uint32_t)(((int32_t)(((uint32_t)(Actor_actors[0].absY))))) | (uint32_t)(31));
#line 1553
    GameState_sleepInBed = 1;
#line 1554
    Actor_actors[0].state = 23;
    goto L11;
}

static void GameState_UpdateSleep(void) {
    if (Actor_actors[0].state != 23) goto L2; else goto L1;
  L1:;
#line 1563
    (GameState_dayNight += 63);
    if (GameState_dayNight >= 24000) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (GameState_fatigue > 0) goto L6; else goto L5;
  L4:;
#line 1564
    (GameState_dayNight -= 24000);
    goto L3;
  L5:;
    if (GameState_fatigue == 0) goto L8; else goto L10;
  L6:;
#line 1565
    (GameState_fatigue--);
    goto L5;
  L7:;
    return;
  L8:;
#line 1569
    Actor_actors[0].state = 13;
    if (GameState_sleepInBed) goto L15; else goto L14;
  L9:;
    if (GameState_battleFlag) goto L13; else goto L7;
  L10:;
    if (GameState_fatigue < 30) goto L12; else goto L9;
  L11:;
    if (GameState_dayNight < 10000) goto L8; else goto L9;
  L12:;
    if (GameState_dayNight > 9000) goto L11; else goto L9;
  L13:;
    if (m2_mod(GameState_cycle, 64) == 0) goto L8; else goto L7;
  L14:;
#line 1573
    GameState_sleepInBed = 0;
#line 1574
    Narration_Event(14);
    goto L7;
  L15:;
#line 1571
    Actor_actors[0].absY = ((uint32_t)(((uint32_t)(Actor_actors[0].absY))) & (uint32_t)(65504));
    goto L14;
}

static void GameState_UpdateFatigue(void) {
    if (Actor_actors[0].state == 23) goto L2; else goto L1;
  L1:;
    if (Actor_actors[0].vitality < 1) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (((uint32_t)(((uint32_t)(GameState_dayNight))) & (uint32_t)(127)) != 0) goto L6; else goto L5;
  L4:;
    return;
  L5:;
#line 1583
    (GameState_hunger++);
    (GameState_fatigue++);
    if (GameState_hunger == 35) goto L8; else goto L9;
  L6:;
    return;
  L7:;
    if (GameState_fatigue == 70) goto L25; else goto L26;
  L8:;
#line 1584
    Narration_Event(0);
    goto L7;
  L9:;
    if (GameState_hunger == 60) goto L10; else goto L11;
  L10:;
#line 1585
    Narration_Event(1);
    goto L7;
  L11:;
    if (((uint32_t)(((uint32_t)(GameState_hunger))) & (uint32_t)(7)) == 0) goto L12; else goto L7;
  L12:;
    if (Actor_actors[0].vitality > 5) goto L14; else goto L15;
  L13:;
    goto L7;
  L14:;
    if (GameState_hunger > 100) goto L17; else goto L18;
  L15:;
    if (GameState_fatigue > 170) goto L21; else goto L22;
  L16:;
    if (GameState_hunger > 90) goto L20; else goto L19;
  L17:;
#line 1588
    (Actor_actors[0].vitality -= 2);
    goto L16;
  L18:;
    if (GameState_fatigue > 160) goto L17; else goto L16;
  L19:;
    goto L13;
  L20:;
#line 1589
    Narration_Event(2);
    goto L19;
  L21:;
#line 1591
    Narration_Event(12);
    GameState_sleepInBed = 0;
    Actor_actors[0].state = 23;
    goto L13;
  L22:;
    if (GameState_hunger > 140) goto L23; else goto L13;
  L23:;
#line 1593
    Narration_Event(24);
    GameState_hunger = 130;
    GameState_sleepInBed = 0;
    Actor_actors[0].state = 23;
    goto L13;
  L24:;
    return;
  L25:;
#line 1596
    Narration_Event(3);
    goto L24;
  L26:;
    if (GameState_fatigue == 90) goto L27; else goto L24;
  L27:;
#line 1597
    Narration_Event(4);
    goto L24;
}

static void GameState_BattleAftermath(void) {
    int32_t i;
    int32_t dead;
    int32_t flee;
    char numStr[7 + 1];
    if (Actor_actors[0].vitality < 1) goto L2; else goto L1;
  L1:;
#line 1607
    dead = 0;
    flee = 0;
    i = 4;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= (Actor_actorCount - 1)) goto L4; else goto L6;
  L4:;
    if (Actor_actors[i].actorType == 2) goto L8; else goto L7;
  L5:;
    i = (i + 1);
    goto L3;
  L6:;
    if (Actor_actors[0].vitality < 5) goto L17; else goto L16;
  L7:;
    goto L5;
  L8:;
    if (Actor_actors[i].state == 15) goto L10; else goto L12;
  L9:;
    goto L7;
  L10:;
#line 1611
    (dead++);
    goto L9;
  L11:;
    if (Actor_actors[i].goal == 5) goto L13; else goto L9;
  L12:;
    if (Actor_actors[i].state == 14) goto L10; else goto L11;
  L13:;
#line 1612
    (flee++);
    goto L9;
  L14:;
    if (Carrier_turtleEggs) goto L23; else goto L22;
  L15:;
#line 1615
    GameState_ShowMessage("Bravely done!", 13);
    goto L14;
  L16:;
    if (dead > 0) goto L19; else goto L18;
  L17:;
    if (dead > 0) goto L15; else goto L16;
  L18:;
    if (flee > 0) goto L21; else goto L20;
  L19:;
#line 1618
    GameState_IntToStr(dead, numStr, 7);
    m2_Strings_Assign(numStr, GameState_msgBuf, 79);
#line 1619
    m2_Strings_Concat(GameState_msgBuf, " foes were defeated in battle.", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L18;
  L20:;
    goto L14;
  L21:;
#line 1622
    GameState_IntToStr(flee, numStr, 7);
    m2_Strings_Assign(numStr, GameState_msgBuf, 79);
#line 1623
    m2_Strings_Concat(GameState_msgBuf, " foes fled in retreat.", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
    goto L20;
  L22:;
    return;
  L23:;
#line 1628
    Carrier_SpawnTurtle();
#line 1629
    Encounter_MoveExtent(1, Actor_actors[3].absX, Actor_actors[3].absY);
#line 1630
    Carrier_turtleEggs = 0;
#line 1631
    Carrier_turtleEggsDone = 1;
#line 1632
    GameState_ShowMessage("The turtle appears, grateful you saved its eggs!", 48);
    goto L22;
}

static int32_t GameState_PointerDirection(int32_t mx, int32_t my) {
    (void)mx;
    (void)my;
    int32_t dx;
    int32_t dy;
    int32_t ax;
    int32_t ay;
#line 1639
    dx = (mx - ((Actor_actors[0].absX - World_camX) * 3));
#line 1640
    dy = (my - ((Actor_actors[0].absY - World_camY) * 3));
#line 1641
    ax = dx;
    ay = dy;
    if (ax < 0) goto L2; else goto L1;
  L1:;
    if (ay < 0) goto L4; else goto L3;
  L2:;
#line 1642
    ax = (-ax);
    goto L1;
  L3:;
    if (ax <= (4 * 3)) goto L7; else goto L5;
  L4:;
#line 1643
    ay = (-ay);
    goto L3;
  L5:;
    if (ax > (ay * 2)) goto L9; else goto L10;
  L6:;
    return 8;
  L7:;
    if (ay <= (4 * 3)) goto L6; else goto L5;
  L8:;
    return 0;
  L9:;
    if (dx < 0) goto L12; else goto L13;
  L10:;
    if (ay > (ax * 2)) goto L14; else goto L15;
  L11:;
    return 0;
  L12:;
    return 6;
  L13:;
    return 2;
  L14:;
    if (dy < 0) goto L17; else goto L18;
  L15:;
    if (dx < 0) goto L19; else goto L20;
  L16:;
    return 0;
  L17:;
    return 0;
  L18:;
    return 4;
  L19:;
    if (dy < 0) goto L22; else goto L23;
  L20:;
    if (dy < 0) goto L25; else goto L26;
  L21:;
    return 0;
  L22:;
    return 7;
  L23:;
    return 5;
  L24:;
    return 0;
  L25:;
    return 1;
  L26:;
    return 3;
}

static void GameState_UpdatePlayer(void) {
    int32_t newX;
    int32_t newY;
    if (GameState_input.quit) goto L2; else goto L1;
  L1:;
    if (Actor_actors[0].state == 23) goto L4; else goto L3;
  L2:;
#line 1659
    GameState_running = 0;
    return;
  L3:;
    if (Actor_actors[0].state == 22) goto L6; else goto L5;
  L4:;
    return;
  L5:;
    if (Actor_actors[0].state == 14) goto L8; else goto L7;
  L6:;
    return;
  L7:;
    if (Actor_actors[0].state == 15) goto L14; else goto L13;
  L8:;
    if (Actor_actors[0].tactic == 7) goto L10; else goto L9;
  L9:;
#line 1667
    (Actor_actors[0].tactic--);
    if (Actor_actors[0].tactic <= 0) goto L12; else goto L11;
  L10:;
#line 1665
    Music_SetMood(24);
    goto L9;
  L11:;
    return;
  L12:;
#line 1669
    Actor_actors[0].state = 15;
#line 1670
    GameState_deathTimer = 0;
#line 1671
    Brothers_ActiveName(GameState_nameBuf, 31);
    m2_Strings_Assign(GameState_nameBuf, GameState_msgBuf, 79);
#line 1672
    m2_Strings_Concat(GameState_msgBuf, " has fallen!", GameState_msgBuf, 79);
    GameState_ShowMessage(GameState_msgBuf, 79);
#line 1673
    Brothers_DecLuck(5);
    goto L11;
  L13:;
    if (GameState_potionCooldown > 0) goto L38; else goto L37;
  L14:;
#line 1678
    (GameState_deathTimer++);
    if (GameState_deathTimer > 150) goto L18; else goto L17;
  L15:;
    if (GameState_deathTimer > 9000) goto L22; else goto L23;
  L16:;
#line 1685
    GameState_fairyActive = 1;
#line 1686
    GameState_fairyX = ((Actor_actors[0].absX + 16) + (400 - GameState_deathTimer));
    goto L15;
  L17:;
    if (GameState_deathTimer > 400) goto L19; else goto L20;
  L18:;
    if (GameState_deathTimer <= 400) goto L16; else goto L17;
  L19:;
#line 1689
    GameState_fairyActive = 1;
#line 1690
    GameState_fairyX = (Actor_actors[0].absX + 16);
    goto L15;
  L20:;
#line 1692
    GameState_fairyActive = 0;
    goto L15;
  L21:;
    return;
  L22:;
#line 1697
    Music_StopMusic();
#line 1698
    GameState_fairyActive = 0;
    if (Brothers_brothers[Brothers_activeBrother].luck >= 1) goto L26; else goto L27;
  L23:;
    if (GameState_deathTimer > 400) goto L24; else goto L21;
  L24:;
    if (Music_IsPlaying()) goto L21; else goto L22;
  L25:;
    goto L21;
  L26:;
#line 1704
    Actor_actors[0].state = 13;
#line 1705
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
#line 1706
    Actor_actors[0].environ = 0;
#line 1707
    GameState_hunger = 0;
    GameState_fatigue = 0;
#line 1708
    GameState_dayNight = 8000;
#line 1709
    GameState_dayPeriod = 4;
#line 1710
    GameState_lightTimer = 0;
    GameState_secretTimer = 0;
    GameState_freezeTimer = 0;
#line 1711
    Combat_wardTimer = 0;
    GameState_sanctuaryTimer = 0;
#line 1712
    Actor_actorCount = 1;
#line 1713
    GameState_deathTimer = 0;
#line 1714
    GameState_ShowMessage("The good fairy has revived you!", 31);
    goto L25;
  L27:;
#line 1719
    WorldObj_AddObj(Actor_actors[0].absX, Actor_actors[0].absY, 28, 1, (-1));
    if (Brothers_activeBrother <= 1) goto L29; else goto L28;
  L28:;
    if (Brothers_SwitchToNext()) goto L31; else goto L32;
  L29:;
#line 1723
    WorldObj_objects[Brothers_activeBrother].status = 3;
    goto L28;
  L30:;
    goto L25;
  L31:;
#line 1726
    Actor_actors[0].absX = 19036;
    Actor_actors[0].absY = 15755;
#line 1727
    Actor_actors[0].state = 13;
#line 1728
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
#line 1729
    Actor_actors[0].weapon = 1;
#line 1730
    Actor_actors[0].environ = 0;
#line 1731
    Actor_actors[0].facing = 4;
#line 1732
    GameState_hunger = 0;
    GameState_fatigue = 0;
#line 1733
    GameState_dayNight = 8000;
#line 1734
    GameState_dayPeriod = 4;
#line 1735
    GameState_lightTimer = 0;
    GameState_secretTimer = 0;
    GameState_freezeTimer = 0;
#line 1736
    Combat_wardTimer = 0;
    GameState_sanctuaryTimer = 0;
#line 1737
    Doors_RestoreDoorTiles();
#line 1738
    Assets_SwitchRegion(3);
#line 1739
    Narration_InitPlace(Actor_actors[0].absX, Actor_actors[0].absY, 3);
#line 1740
    Actor_actorCount = 1;
#line 1741
    Narration_Event(9);
    if (Brothers_activeBrother == 1) goto L34; else goto L35;
  L32:;
#line 1747
    GameState_ShowMessage("All brothers have fallen... Game Over.", 38);
#line 1748
    GameState_deathTimer = (-1);
    goto L30;
  L33:;
#line 1744
    GameState_deathTimer = 0;
#line 1745
    Menu_SetOptions();
    goto L30;
  L34:;
#line 1742
    Narration_Event(10);
    goto L33;
  L35:;
    if (Brothers_activeBrother == 2) goto L36; else goto L33;
  L36:;
#line 1743
    Narration_Event(11);
    goto L33;
  L37:;
    if (GameState_input.usePotion) goto L41; else goto L39;
  L38:;
#line 1754
    (GameState_potionCooldown--);
    goto L37;
  L39:;
    if (GameState_input.useFood) goto L48; else goto L47;
  L40:;
    if (Items_UseItem(6)) goto L43; else goto L44;
  L41:;
    if (GameState_potionCooldown == 0) goto L40; else goto L39;
  L42:;
    goto L39;
  L43:;
#line 1757
    (Actor_actors[0].vitality += 30);
    if (Actor_actors[0].vitality > (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4))) goto L46; else goto L45;
  L44:;
#line 1761
    GameState_ShowMessage("No potions!", 11);
    GameState_potionCooldown = 30;
    goto L42;
  L45:;
#line 1760
    GameState_ShowMessage("Potion restores your health!", 28);
    GameState_potionCooldown = 30;
    goto L42;
  L46:;
#line 1759
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
    goto L45;
  L47:;
    if (GameState_input.talk) goto L51; else goto L49;
  L48:;
#line 1763
    GameState_HandleEat();
    goto L47;
  L49:;
    if (Actor_actors[0].environ == (-2)) goto L55; else goto L54;
  L50:;
#line 1764
    GameState_HandleTalk();
    GameState_potionCooldown = 30;
    goto L49;
  L51:;
    if (GameState_potionCooldown == 0) goto L50; else goto L49;
  L52:;
    if (Carrier_riding == 11) goto L70; else goto L69;
  L53:;
    if (GameState_input.dirKey != 8) goto L57; else goto L56;
  L54:;
#line 1792
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
    goto L52;
  L55:;
    if (Carrier_riding != 11) goto L53; else goto L54;
  L56:;
#line 1780
    newX = (Actor_actors[0].absX + m2_div(Actor_actors[0].velX, 4));
#line 1781
    newY = (Actor_actors[0].absY + m2_div(Actor_actors[0].velY, 4));
    if (Movement_ProxCheck(newX, newY, 0) == 0) goto L67; else goto L68;
  L57:;
#line 1770
    Actor_actors[0].facing = GameState_input.dirKey;
#line 1771
    (Actor_actors[0].velX += GameState_NewXDir(GameState_input.dirKey));
#line 1772
    (Actor_actors[0].velY += GameState_NewYDir(GameState_input.dirKey));
    if (Actor_actors[0].velX > 34) goto L59; else goto L60;
  L58:;
    if (Actor_actors[0].velY > 42) goto L63; else goto L64;
  L59:;
#line 1773
    Actor_actors[0].velX = 34;
    goto L58;
  L60:;
    if (Actor_actors[0].velX < (-34)) goto L61; else goto L58;
  L61:;
#line 1774
    Actor_actors[0].velX = (-34);
    goto L58;
  L62:;
#line 1777
    Actor_actors[0].state = 12;
    goto L56;
  L63:;
#line 1775
    Actor_actors[0].velY = 42;
    goto L62;
  L64:;
    if (Actor_actors[0].velY < (-42)) goto L65; else goto L62;
  L65:;
#line 1776
    Actor_actors[0].velY = (-42);
    goto L62;
  L66:;
    return;
  L67:;
#line 1783
    Actor_actors[0].absX = newX;
#line 1784
    Actor_actors[0].absY = newY;
    goto L66;
  L68:;
#line 1786
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
#line 1787
    Actor_actors[0].environ = 0;
    goto L66;
  L69:;
    if (GameState_input.attack) goto L94; else goto L93;
  L70:;
#line 1798
    Actor_actors[0].environ = (-2);
    if (GameState_input.attack) goto L72; else goto L73;
  L71:;
#line 1815
    Actor_actors[0].absX = (Actor_actors[0].absX + m2_div(Actor_actors[0].velX, 4));
#line 1816
    Actor_actors[0].absY = (Actor_actors[0].absY + m2_div(Actor_actors[0].velY, 4));
    if (Actor_actors[0].absX < 0) goto L84; else goto L83;
  L72:;
#line 1800
    Carrier_swanDismount = 1;
    goto L71;
  L73:;
    if (GameState_input.dirKey != 8) goto L74; else goto L71;
  L74:;
#line 1805
    Actor_actors[0].facing = GameState_input.dirKey;
#line 1806
    (Actor_actors[0].velX += GameState_NewXDir(GameState_input.dirKey));
#line 1807
    (Actor_actors[0].velY += GameState_NewYDir(GameState_input.dirKey));
    if (Actor_actors[0].velX > 32) goto L76; else goto L77;
  L75:;
    if (Actor_actors[0].velY > 40) goto L80; else goto L81;
  L76:;
#line 1808
    Actor_actors[0].velX = 32;
    goto L75;
  L77:;
    if (Actor_actors[0].velX < (-32)) goto L78; else goto L75;
  L78:;
#line 1809
    Actor_actors[0].velX = (-32);
    goto L75;
  L79:;
#line 1812
    Actor_actors[0].state = 12;
    goto L71;
  L80:;
#line 1810
    Actor_actors[0].velY = 40;
    goto L79;
  L81:;
    if (Actor_actors[0].velY < (-40)) goto L82; else goto L79;
  L82:;
#line 1811
    Actor_actors[0].velY = (-40);
    goto L79;
  L83:;
    if (Actor_actors[0].absY < 0) goto L86; else goto L85;
  L84:;
#line 1818
    Actor_actors[0].absX = 0;
    Actor_actors[0].velX = 0;
    goto L83;
  L85:;
    if (Actor_actors[0].absX > 32767) goto L88; else goto L87;
  L86:;
#line 1819
    Actor_actors[0].absY = 0;
    Actor_actors[0].velY = 0;
    goto L85;
  L87:;
    if (Actor_actors[0].absY > 40959) goto L90; else goto L89;
  L88:;
#line 1820
    Actor_actors[0].absX = 32767;
    Actor_actors[0].velX = 0;
    goto L87;
  L89:;
    return;
  L90:;
#line 1821
    Actor_actors[0].absY = 40959;
    Actor_actors[0].velY = 0;
    goto L89;
  L91:;
    return;
  L92:;
    if (Actor_actors[0].weapon >= 4) goto L98; else goto L97;
  L93:;
    if (Actor_actors[0].state == 0) goto L107; else goto L109;
  L94:;
    if (Carrier_swanCooldown == 0) goto L92; else goto L93;
  L95:;
    goto L91;
  L96:;
    if (Actor_actors[0].weapon == 4) goto L102; else goto L101;
  L97:;
    if (Actor_actors[0].weapon >= 4) goto L105; else goto L106;
  L98:;
    if (Actor_actors[0].state != 24) goto L96; else goto L97;
  L99:;
#line 1836
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
    goto L95;
  L100:;
#line 1828
    GameState_ShowMessage("No Arrows!", 10);
    goto L99;
  L101:;
#line 1830
    Actor_actors[0].state = 24;
    Missile_FireMissile(0);
    if (Actor_actors[0].weapon == 4) goto L104; else goto L103;
  L102:;
    if (Brothers_brothers[Brothers_activeBrother].stuff[8] <= 0) goto L100; else goto L101;
  L103:;
    goto L99;
  L104:;
#line 1833
    (Brothers_brothers[Brothers_activeBrother].stuff[8]--);
    goto L103;
  L105:;
#line 1838
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
    goto L95;
  L106:;
#line 1840
    Actor_actors[0].state = 0;
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
    goto L95;
  L107:;
#line 1843
    Actor_actors[0].state = 13;
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
    goto L91;
  L108:;
    if (GameState_input.dirKey != 8) goto L110; else goto L111;
  L109:;
    if (Actor_actors[0].state == 24) goto L107; else goto L108;
  L110:;
    if (Actor_actors[0].environ == (-3)) goto L113; else goto L114;
  L111:;
#line 1856
    Actor_actors[0].state = 13;
    Actor_actors[0].velX = 0;
    Actor_actors[0].velY = 0;
    goto L91;
  L112:;
    goto L91;
  L113:;
#line 1847
    Actor_actors[0].facing = ((uint32_t)(((uint32_t)((GameState_input.dirKey + 4)))) & (uint32_t)(7));
    if (Movement_MoveActor(0, Actor_actors[0].facing, 2)) goto L116; else goto L117;
  L114:;
#line 1851
    Actor_actors[0].facing = GameState_input.dirKey;
    if (Movement_MoveActor(0, GameState_input.dirKey, 1)) goto L119; else goto L120;
  L115:;
    goto L112;
  L116:;
#line 1848
    Actor_actors[0].state = 12;
    goto L115;
  L117:;
#line 1849
    Actor_actors[0].state = 13;
    goto L115;
  L118:;
    goto L112;
  L119:;
#line 1852
    Actor_actors[0].state = 12;
    goto L118;
  L120:;
#line 1853
    Actor_actors[0].state = 13;
    goto L118;
}

static void GameState_CheckDoors(void) {
    int32_t newX;
    int32_t newY;
    int32_t newReg;
    int onDoor;
    if (GameState_doorCooldown > 0) goto L2; else goto L1;
  L1:;
#line 1865
    onDoor = 0;
    if (Assets_currentRegion < 8) goto L4; else goto L5;
  L2:;
#line 1864
    (GameState_doorCooldown--);
    return;
  L3:;
#line 1875
    Doors_CheckCloseDoors(Actor_actors[0].absX, Actor_actors[0].absY);
    if (Doors_CheckDoor(Actor_actors[0].absX, Actor_actors[0].absY, Assets_currentRegion, &newX, &newY, &newReg)) goto L13; else goto L12;
  L4:;
    if (Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY) == 15) goto L7; else goto L11;
  L5:;
#line 1874
    onDoor = 1;
    goto L3;
  L6:;
    goto L3;
  L7:;
#line 1872
    onDoor = 1;
    goto L6;
  L8:;
    if (Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY - 4)) == 15) goto L7; else goto L6;
  L9:;
    if (Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY + 4)) == 15) goto L7; else goto L8;
  L10:;
    if (Assets_GetTerrainAt((Actor_actors[0].absX - 4), Actor_actors[0].absY) == 15) goto L7; else goto L9;
  L11:;
    if (Assets_GetTerrainAt((Actor_actors[0].absX + 4), Actor_actors[0].absY) == 15) goto L7; else goto L10;
  L12:;
    return;
  L13:;
#line 1878
    Actor_actors[0].absX = newX;
    Actor_actors[0].absY = newY;
    if (newReg >= 0) goto L15; else goto L16;
  L14:;
#line 1883
    newReg = Assets_GetTerrainAt(Actor_actors[0].absX, Actor_actors[0].absY);
    if (newReg == 1) goto L18; else goto L19;
  L15:;
#line 1879
    Doors_RestoreDoorTiles();
    Assets_SwitchRegion(newReg);
    goto L14;
  L16:;
#line 1880
    Doors_RestoreDoorTiles();
    Assets_SwitchRegion(Assets_DetectRegion(newX, newY));
    goto L14;
  L17:;
#line 1907
    Narration_InitPlace(Actor_actors[0].absX, Actor_actors[0].absY, Assets_currentRegion);
#line 1908
    GameState_doorCooldown = 20;
    goto L12;
  L18:;
    newX = 1;
    goto L20;
  L19:;
    if (newReg >= 10) goto L18; else goto L17;
  L20:;
    if (newX <= 16) goto L21; else goto L23;
  L21:;
#line 1886
    newReg = Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY - newX));
    if (newReg != 1) goto L27; else goto L26;
  L22:;
    newX = (newX + 1);
    goto L20;
  L23:;
    goto L17;
  L24:;
    goto L22;
  L25:;
#line 1888
    (Actor_actors[0].absY -= newX);
    newX = 16;
    goto L24;
  L26:;
#line 1890
    newReg = Assets_GetTerrainAt(Actor_actors[0].absX, (Actor_actors[0].absY + newX));
    if (newReg != 1) goto L31; else goto L30;
  L27:;
    if (newReg < 10) goto L25; else goto L26;
  L28:;
    goto L24;
  L29:;
#line 1892
    (Actor_actors[0].absY += newX);
    newX = 16;
    goto L28;
  L30:;
#line 1894
    newReg = Assets_GetTerrainAt((Actor_actors[0].absX + newX), Actor_actors[0].absY);
    if (newReg != 1) goto L35; else goto L34;
  L31:;
    if (newReg < 10) goto L29; else goto L30;
  L32:;
    goto L28;
  L33:;
#line 1896
    (Actor_actors[0].absX += newX);
    newX = 16;
    goto L32;
  L34:;
#line 1898
    newReg = Assets_GetTerrainAt((Actor_actors[0].absX - newX), Actor_actors[0].absY);
    if (newReg != 1) goto L38; else goto L36;
  L35:;
    if (newReg < 10) goto L33; else goto L34;
  L36:;
    goto L32;
  L37:;
#line 1900
    (Actor_actors[0].absX -= newX);
    newX = 16;
    goto L36;
  L38:;
    if (newReg < 10) goto L37; else goto L36;
}

static void GameState_UpdateGame(void) {
#line 1914
    GameState_input.quit = 0;
    GameState_input.dirKey = 8;
#line 1915
    GameState_input.attack = 0;
    GameState_input.usePotion = 0;
#line 1916
    GameState_input.useFood = 0;
    GameState_input.talk = 0;
    GameState_input.toggleMap = 0;
#line 1917
    Platform_PollInput(&GameState_input);
    if (GameState_input.dirKey == 8) goto L3; else goto L1;
  L1:;
    if (GameState_viewStatus != 0) goto L5; else goto L4;
  L2:;
#line 1919
    GameState_input.dirKey = GameState_PointerDirection(GameState_input.mouseX, GameState_input.mouseY);
    goto L1;
  L3:;
    if (GameState_input.mouseMove) goto L2; else goto L1;
  L4:;
    if (Actor_actors[0].state == 15) goto L14; else goto L16;
  L5:;
    if (GameState_input.quit) goto L7; else goto L6;
  L6:;
    if (GameState_input.attack) goto L9; else goto L12;
  L7:;
#line 1922
    GameState_running = 0;
    return;
  L8:;
    return;
  L9:;
#line 1924
    GameState_viewStatus = 0;
    goto L8;
  L10:;
    if (GameState_input.dirKey != 8) goto L9; else goto L8;
  L11:;
    if (GameState_input.mouseClick) goto L9; else goto L10;
  L12:;
    if (GameState_input.menuKey != '\0') goto L9; else goto L11;
  L13:;
    if (GameState_input.quit) goto L45; else goto L44;
  L14:;
    if (GameState_input.attack) goto L18; else goto L20;
  L15:;
#line 1936
    GameState_mapToggled = GameState_input.toggleMap;
    if (GameState_input.menuKey == '0') goto L24; else goto L25;
  L16:;
    if (Actor_actors[0].state == 14) goto L14; else goto L15;
  L17:;
    goto L13;
  L18:;
    if (Actor_actors[0].state == 15) goto L22; else goto L21;
  L19:;
    if (GameState_input.dirKey != 8) goto L18; else goto L17;
  L20:;
    if (GameState_input.menuKey != '\0') goto L18; else goto L19;
  L21:;
    goto L17;
  L22:;
#line 1932
    GameState_deathTimer = 9999;
    goto L21;
  L23:;
    if (GameState_input.mouseClick) goto L43; else goto L42;
  L24:;
#line 1938
    GameState_cheatGod = (!GameState_cheatGod);
    if (GameState_cheatGod) goto L27; else goto L28;
  L25:;
    if (GameState_input.menuKey == '9') goto L29; else goto L30;
  L26:;
    goto L23;
  L27:;
#line 1939
    GameState_ShowMessage("GOD MODE ON", 11);
    goto L26;
  L28:;
#line 1940
    GameState_ShowMessage("GOD MODE OFF", 12);
    goto L26;
  L29:;
#line 1942
    GameState_cheatSpeed = (!GameState_cheatSpeed);
    if (GameState_cheatSpeed) goto L32; else goto L33;
  L30:;
    if (GameState_input.menuKey == '8') goto L34; else goto L35;
  L31:;
    goto L23;
  L32:;
#line 1943
    GameState_ShowMessage("SPEED MODE ON", 13);
    goto L31;
  L33:;
#line 1944
    GameState_ShowMessage("SPEED MODE OFF", 14);
    goto L31;
  L34:;
#line 1946
    Platform_cheatKeys = (!Platform_cheatKeys);
    if (Platform_cheatKeys) goto L37; else goto L38;
  L35:;
    if (GameState_input.menuKey == 'E') goto L39; else goto L40;
  L36:;
    goto L23;
  L37:;
#line 1947
    GameState_ShowMessage("NO KEYS MODE ON", 15);
    goto L36;
  L38:;
#line 1948
    GameState_ShowMessage("NO KEYS MODE OFF", 16);
    goto L36;
  L39:;
#line 1950
    GameState_HandleWorldPickup();
    goto L23;
  L40:;
    if (GameState_input.menuKey != '\0') goto L41; else goto L23;
  L41:;
#line 1951
    Menu_HandleMenuKey(GameState_input.menuKey);
    goto L23;
  L42:;
    goto L13;
  L43:;
#line 1952
    GameState_HandleMenuClick(GameState_input.mouseX, GameState_input.mouseY);
    goto L42;
  L44:;
    if (((uint32_t)(((uint32_t)(Menu_menus[4].enabled[5]))) & (uint32_t)(1)) != 0) goto L47; else goto L46;
  L45:;
#line 1954
    GameState_running = 0;
    return;
  L46:;
#line 1958
    GameState_UpdatePlayer();
    if (GameState_cheatGod) goto L53; else goto L50;
  L47:;
    if (GameState_msgTimer > 0) goto L49; else goto L48;
  L48:;
    return;
  L49:;
#line 1956
    (GameState_msgTimer--);
    goto L48;
  L50:;
    if (GameState_cheatSpeed) goto L56; else goto L54;
  L51:;
#line 1961
    Actor_actors[0].vitality = (15 + m2_div(Brothers_brothers[Brothers_activeBrother].brave, 4));
    goto L50;
  L52:;
    if (Actor_actors[0].state != 14) goto L51; else goto L50;
  L53:;
    if (Actor_actors[0].state != 15) goto L52; else goto L50;
  L54:;
#line 1970
    GameState_CheckEnvironment();
    if (GameState_freezeTimer == 0) goto L66; else goto L65;
  L55:;
    if (Movement_MoveActor(0, Actor_actors[0].facing, 1)) goto L58; else goto L57;
  L56:;
    if (Actor_actors[0].state == 12) goto L55; else goto L54;
  L57:;
    if (Movement_MoveActor(0, Actor_actors[0].facing, 1)) goto L60; else goto L59;
  L58:;
    goto L57;
  L59:;
    if (Movement_MoveActor(0, Actor_actors[0].facing, 1)) goto L62; else goto L61;
  L60:;
    goto L59;
  L61:;
    if (Movement_MoveActor(0, Actor_actors[0].facing, 1)) goto L64; else goto L63;
  L62:;
    goto L61;
  L63:;
    goto L54;
  L64:;
    goto L63;
  L65:;
#line 1981
    Carrier_UpdateCarriers();
    if (Carrier_dismountResult == 1) goto L73; else goto L74;
  L66:;
#line 1972
    EnemyAI_UpdateEnemies();
    if (Actor_actors[0].state != 15) goto L69; else goto L67;
  L67:;
#line 1979
    Missile_UpdateMissiles();
    goto L65;
  L68:;
#line 1974
    Combat_UpdateCombat();
    if (GameState_sanctuaryTimer == 0) goto L71; else goto L70;
  L69:;
    if (Actor_actors[0].state != 14) goto L68; else goto L67;
  L70:;
    goto L67;
  L71:;
#line 1976
    Encounter_UpdateEncounters(Actor_actors[0].absX, Actor_actors[0].absY, Assets_currentRegion);
    goto L70;
  L72:;
#line 1986
    Carrier_dismountResult = 0;
#line 1987
    Carrier_UpdateDragon();
    if (Carrier_dragonFire) goto L77; else goto L76;
  L73:;
#line 1983
    Narration_Event(33);
    goto L72;
  L74:;
    if (Carrier_dismountResult == 2) goto L75; else goto L72;
  L75:;
#line 1984
    Narration_Event(32);
    goto L72;
  L76:;
#line 1992
    Quest_CheckRescue(Actor_actors[0].absX, Actor_actors[0].absY);
    if (Quest_CheckWinCondition()) goto L79; else goto L78;
  L77:;
#line 1989
    Missile_FireMissile(3);
#line 1990
    Carrier_dragonFire = 0;
    goto L76;
  L78:;
#line 1994
    GameState_CheckDoors();
#line 1995
    World_UpdateCamera(Actor_actors[0].absX, Actor_actors[0].absY);
#line 1996
    GameState_prevRegion = Assets_currentRegion;
#line 1997
    Assets_CheckRegionSwitch(World_camX, World_camY);
    if (Assets_currentRegion != GameState_prevRegion) goto L81; else goto L80;
  L79:;
#line 1993
    Quest_ShowWinScreen();
    GameState_running = 0;
    goto L78;
  L80:;
#line 2001
    Narration_UpdatePlace(Actor_actors[0].absX, Actor_actors[0].absY, Assets_currentRegion);
#line 2002
    NPC_MaterializeNPCs(Actor_actors[0].absX, Actor_actors[0].absY, Assets_currentRegion);
#line 2003
    GameState_UpdateWitch();
#line 2004
    DayNight_UpdateDayNight();
#line 2005
    GameState_UpdateSleep();
#line 2006
    GameState_CheckBedTile();
#line 2007
    GameState_UpdateFatigue();
    if (DayNight_lightlevel < 40) goto L83; else goto L84;
  L81:;
#line 1999
    WorldObj_DistributeRegion(Assets_currentRegion);
    goto L80;
  L82:;
    if (m2_div(GameState_dayNight, 2000) != GameState_dayPeriod) goto L92; else goto L91;
  L83:;
    if (WorldObj_objCount > 2) goto L87; else goto L85;
  L84:;
    if (WorldObj_objCount > 2) goto L90; else goto L88;
  L85:;
    goto L82;
  L86:;
#line 2013
    WorldObj_objects[2].status = 3;
    goto L85;
  L87:;
    if (WorldObj_objects[2].objId == 10) goto L86; else goto L85;
  L88:;
    goto L82;
  L89:;
#line 2017
    WorldObj_objects[2].status = 2;
    goto L88;
  L90:;
    if (WorldObj_objects[2].objId == 10) goto L89; else goto L88;
  L91:;
    if (Actor_actors[0].state != 15) goto L101; else goto L99;
  L92:;
#line 2022
    GameState_dayPeriod = m2_div(GameState_dayNight, 2000);
    if ((GameState_dayPeriod == 0)) goto L95;
    if ((GameState_dayPeriod == 4)) goto L96;
    if ((GameState_dayPeriod == 6)) goto L97;
    if ((GameState_dayPeriod == 9)) goto L98;
    goto L94;
  L93:;
    goto L91;
  L94:;
    goto L93;
  L95:;
#line 2024
    Narration_Event(28);
    goto L93;
  L96:;
    Narration_Event(29);
    goto L93;
  L97:;
    Narration_Event(30);
    goto L93;
  L98:;
    Narration_Event(31);
    goto L93;
  L99:;
    if (DayNight_MusicTickDue()) goto L120; else goto L117;
  L100:;
#line 2028
    GameState_prevBattle = GameState_battleFlag;
#line 2029
    GameState_battleFlag = Encounter_EnemiesNearby(Actor_actors[0].absX, Actor_actors[0].absY);
    if (GameState_battleFlag) goto L104; else goto L102;
  L101:;
    if (Actor_actors[0].state != 14) goto L100; else goto L99;
  L102:;
    if (GameState_battleFlag) goto L107; else goto L105;
  L103:;
#line 2030
    GameState_aftermathDone = 0;
    goto L102;
  L104:;
    if (GameState_prevBattle) goto L102; else goto L103;
  L105:;
    if (GameState_battleFlag) goto L113; else goto L116;
  L106:;
    if (Actor_actors[0].absX > 9216) goto L112; else goto L109;
  L107:;
    if (GameState_prevBattle) goto L105; else goto L106;
  L108:;
    goto L105;
  L109:;
#line 2036
    Music_SetMood(4);
    goto L108;
  L110:;
    if (Actor_actors[0].absY < 35328) goto L108; else goto L109;
  L111:;
    if (Actor_actors[0].absY > 33280) goto L110; else goto L109;
  L112:;
    if (Actor_actors[0].absX < 12544) goto L111; else goto L109;
  L113:;
    goto L99;
  L114:;
#line 2041
    GameState_BattleAftermath();
#line 2042
    GameState_aftermathDone = 1;
    goto L113;
  L115:;
    if (GameState_aftermathDone) goto L113; else goto L114;
  L116:;
    if (GameState_prevBattle) goto L115; else goto L113;
  L117:;
    if (Assets_currentRegion >= 8) goto L134; else goto L135;
  L118:;
    if (Actor_actors[0].absX > 9216) goto L126; else goto L123;
  L119:;
    if (Actor_actors[0].state != 14) goto L118; else goto L117;
  L120:;
    if (Actor_actors[0].state != 15) goto L119; else goto L117;
  L121:;
    goto L117;
  L122:;
#line 2056
    Music_SetMood(16);
    goto L121;
  L123:;
    if (GameState_battleFlag) goto L127; else goto L128;
  L124:;
    if (Actor_actors[0].absY < 35328) goto L122; else goto L123;
  L125:;
    if (Actor_actors[0].absY > 33280) goto L124; else goto L123;
  L126:;
    if (Actor_actors[0].absX < 12544) goto L125; else goto L123;
  L127:;
#line 2058
    Music_SetMood(4);
    goto L121;
  L128:;
    if (Assets_currentRegion >= 8) goto L129; else goto L130;
  L129:;
#line 2060
    Music_SetCaveWave(Assets_currentRegion == 9);
#line 2061
    Music_SetMood(20);
    goto L121;
  L130:;
    if (DayNight_lightlevel > 120) goto L131; else goto L132;
  L131:;
#line 2062
    Music_SetMood(0);
    goto L121;
  L132:;
#line 2063
    Music_SetMood(8);
    goto L121;
  L133:;
#line 2069
    Brothers_SaveBrotherState();
#line 2070
    HudLog_SetStats(Brothers_brothers[Brothers_activeBrother].brave, Brothers_brothers[Brothers_activeBrother].luck, Brothers_brothers[Brothers_activeBrother].kind, Brothers_brothers[Brothers_activeBrother].wealth, Actor_actors[0].vitality);
    if (GameState_lightTimer > 0) goto L138; else goto L137;
  L134:;
#line 2066
    DayNight_brightness = 100;
    DayNight_isNight = 0;
    goto L133;
  L135:;
    if (GameState_lightTimer > 0) goto L136; else goto L133;
  L136:;
#line 2067
    DayNight_brightness = 100;
    DayNight_isNight = 0;
    goto L133;
  L137:;
    if (GameState_secretTimer > 0) goto L140; else goto L139;
  L138:;
#line 2076
    (GameState_lightTimer--);
    goto L137;
  L139:;
    if (GameState_freezeTimer > 0) goto L142; else goto L141;
  L140:;
#line 2077
    (GameState_secretTimer--);
    goto L139;
  L141:;
    if (Combat_wardTimer > 0) goto L144; else goto L143;
  L142:;
#line 2078
    (GameState_freezeTimer--);
    goto L141;
  L143:;
    if (GameState_sanctuaryTimer > 0) goto L146; else goto L145;
  L144:;
#line 2079
    (Combat_wardTimer--);
    goto L143;
  L145:;
#line 2081
    WorldObj_revealHidden = GameState_secretTimer > 0;
    if (GameState_msgTimer > 0) goto L148; else goto L147;
  L146:;
#line 2080
    (GameState_sanctuaryTimer--);
    goto L145;
  L147:;
#line 2084
    (GameState_cycle++);
    if (GameState_freezeTimer == 0) goto L150; else goto L149;
  L148:;
#line 2083
    (GameState_msgTimer--);
    goto L147;
  L149:;
    return;
  L150:;
#line 2085
    (GameState_dayNight++);
    goto L149;
}

/* Imported Module Intro */

typedef struct Intro_InputState Intro_InputState;
static const int32_t Intro_MoodIntro = 12;
static const int32_t Intro_PageW = 320;
static const int32_t Intro_PageH = 200;
static void * Intro_LoadPage(char *name, uint32_t name_high);
static int Intro_PumpAndCheck(void);
static void Intro_DrawTex(void * tex);
static void Intro_Wait(int32_t n);
static void Intro_ZoomIn(void * tex);
static void Intro_ZoomOut(void * tex);
static int32_t Intro_PageDet(int32_t v);
static void Intro_FlipScan(void * oldTex, void * newTex);
static void Intro_CenterStr(char *s, uint32_t s_high, int32_t y, int32_t sc);
static void Intro_ShowCredits(void);
static void Intro_DrawGreekKey(void);
static void Intro_ShowPlacard(void);
static void Intro_RunIntro(void);

void * Intro_cover;
void * Intro_spread1;
void * Intro_spread2;
void * Intro_spread3;
int Intro_skipped;
int32_t Intro_introTick;
static void * Intro_LoadPage(char *name, uint32_t name_high) {
    (void)name;
    (void)name_high;
    char p[127 + 1];
#line 32 "src/Intro.mod"
    Assets_AssetPath(name, name_high, p, 127);
    return Platform_LoadBMPTexture(p, 127);
}

static int Intro_PumpAndCheck(void) {
    Platform_InputState inp;
    int result;
#line 40
    inp.attack = 0;
    inp.quit = 0;
    inp.mouseClick = 0;
#line 41
    inp.dirKey = 8;
    inp.menuKey = '\0';
#line 42
    Platform_PollInput(&inp);
#line 43
    Music_UpdateMusic();
    if (Intro_introTick < 60) goto L2; else goto L1;
  L1:;
#line 45
    result = ((((inp.attack || inp.menuKey != '\0') || inp.dirKey != 8) || inp.quit) || inp.mouseClick);
    return result;
  L2:;
    return 0;
}

static void Intro_DrawTex(void * tex) {
    (void)tex;
    int32_t sw;
    int32_t sh;
    if (tex == NULL) goto L2; else goto L1;
  L1:;
#line 55
    sw = (320 * 3);
#line 56
    sh = ((143 + 57) * 3);
#line 57
    Platform_BeginFrame();
#line 58
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 59
    Canvas_Clear(Platform_ren);
#line 60
    Texture_DrawRegion(Platform_ren, tex, 0, 0, 320, 200, 0, 0, sw, sh);
#line 61
    Platform_EndFrame();
    return;
  L2:;
    return;
}

static void Intro_Wait(int32_t n) {
    (void)n;
    int32_t i;
    i = 1;
    goto L1;
  L1:;
    if (i <= n) goto L2; else goto L4;
  L2:;
    if (Intro_skipped) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
    if (Intro_PumpAndCheck()) goto L8; else goto L7;
  L6:;
    return;
  L7:;
#line 70
    (Intro_introTick++);
#line 71
    Platform_DelayMs(33);
    goto L3;
  L8:;
#line 69
    Intro_skipped = 1;
    return;
}

static void Intro_ZoomIn(void * tex) {
    (void)tex;
    int32_t i;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t sw;
    int32_t sh;
    if (tex == NULL) goto L2; else goto L1;
  L1:;
#line 80
    sw = (320 * 3);
#line 81
    sh = ((143 + 57) * 3);
#line 82
    i = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (i <= 160) goto L6; else goto L5;
  L4:;
    if (Intro_PumpAndCheck()) goto L8; else goto L7;
  L5:;
    return;
  L6:;
    if (Intro_skipped) goto L5; else goto L4;
  L7:;
#line 85
    (Intro_introTick++);
#line 86
    w = m2_div((sw * i), 160);
#line 87
    h = m2_div((sh * i), 160);
#line 88
    x = m2_div((sw - w), 2);
#line 89
    y = m2_div((sh - h), 2);
#line 90
    Platform_BeginFrame();
#line 91
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 92
    Canvas_Clear(Platform_ren);
    if (w > 0) goto L11; else goto L9;
  L8:;
#line 84
    Intro_skipped = 1;
    return;
  L9:;
#line 96
    Platform_EndFrame();
#line 97
    Music_UpdateMusic();
#line 98
    Platform_DelayMs(33);
#line 99
    (i += 4);
    goto L3;
  L10:;
#line 94
    Texture_DrawRegion(Platform_ren, tex, 0, 0, 320, 200, x, y, w, h);
    goto L9;
  L11:;
    if (h > 0) goto L10; else goto L9;
}

static void Intro_ZoomOut(void * tex) {
    (void)tex;
    int32_t i;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t sw;
    int32_t sh;
    int32_t fade;
    if (tex == NULL) goto L2; else goto L1;
  L1:;
#line 108
    sw = (320 * 3);
#line 109
    sh = ((143 + 57) * 3);
#line 110
    i = 156;
    goto L3;
  L2:;
    return;
  L3:;
    if (i >= 0) goto L6; else goto L5;
  L4:;
    if (Intro_PumpAndCheck()) goto L8; else goto L7;
  L5:;
    return;
  L6:;
    if (Intro_skipped) goto L5; else goto L4;
  L7:;
#line 113
    (Intro_introTick++);
#line 114
    w = m2_div((sw * i), 160);
#line 115
    h = m2_div((sh * i), 160);
#line 116
    x = m2_div((sw - w), 2);
#line 117
    y = m2_div((sh - h), 2);
#line 119
    fade = m2_div((i * 255), 156);
    if (fade > 255) goto L10; else goto L9;
  L8:;
#line 112
    Intro_skipped = 1;
    return;
  L9:;
    if (fade < 0) goto L12; else goto L11;
  L10:;
#line 120
    fade = 255;
    goto L9;
  L11:;
#line 122
    Platform_BeginFrame();
#line 123
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 124
    Canvas_Clear(Platform_ren);
    if (w > 0) goto L15; else goto L13;
  L12:;
#line 121
    fade = 0;
    goto L11;
  L13:;
#line 130
    Platform_EndFrame();
#line 131
    Music_UpdateMusic();
#line 132
    Platform_DelayMs(33);
#line 133
    (i -= 4);
    goto L3;
  L14:;
#line 126
    Texture_SetColorMod(tex, fade, fade, fade);
#line 127
    Texture_DrawRegion(Platform_ren, tex, 0, 0, 320, 200, x, y, w, h);
#line 128
    Texture_SetColorMod(tex, 255, 255, 255);
    goto L13;
  L15:;
    if (h > 0) goto L14; else goto L13;
}

static int32_t Intro_PageDet(int32_t v) {
    (void)v;
    if (v < 0) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return 10;
  L3:;
    if (v == 0) goto L4; else goto L5;
  L4:;
    return 9;
  L5:;
    if (v == 1) goto L6; else goto L7;
  L6:;
    return 9;
  L7:;
    if (v == 2) goto L8; else goto L9;
  L8:;
    return 8;
  L9:;
    if (v == 3) goto L10; else goto L11;
  L10:;
    return 7;
  L11:;
    if (v == 4) goto L12; else goto L13;
  L12:;
    return 6;
  L13:;
    if (v == 5) goto L14; else goto L15;
  L14:;
    return 5;
  L15:;
    if (v == 6) goto L16; else goto L17;
  L16:;
    return 5;
  L17:;
    if (v == 7) goto L18; else goto L19;
  L18:;
    return 5;
  L19:;
    if (v == 8) goto L20; else goto L21;
  L20:;
    return 4;
  L21:;
    if (v == 9) goto L22; else goto L23;
  L22:;
    return 4;
  L23:;
    if (v == 10) goto L24; else goto L25;
  L24:;
    return 4;
  L25:;
    if (v > 135) goto L26; else goto L27;
  L26:;
    return 10;
  L27:;
    if (v > 123) goto L28; else goto L29;
  L28:;
    return 6;
  L29:;
    if (v > 98) goto L30; else goto L31;
  L30:;
    return 5;
  L31:;
    if (v > 71) goto L32; else goto L33;
  L32:;
    return 4;
  L33:;
    return 3;
}

static void Intro_FlipScan(void * oldTex, void * newTex) {
    (void)oldTex;
    (void)newTex;
    int32_t i;
    int32_t d;
    int32_t sw;
    int32_t sh;
    int32_t scol;
    int32_t dcol;
    int32_t h;
    int32_t rate;
    int32_t wide;
    int32_t sx;
    int32_t sy;
    int32_t sw2;
    int32_t sh2;
    int32_t dx;
    int32_t dy;
    int32_t flip1[21 + 1];
    int32_t flip2[21 + 1];
    int32_t flip3[21 + 1];
    if (oldTex == NULL) goto L2; else goto L3;
  L1:;
#line 171
    sw = (320 * 3);
#line 172
    sh = ((143 + 57) * 3);
#line 175
    flip1[0] = 8;
    flip1[1] = 6;
    flip1[2] = 5;
    flip1[3] = 4;
    flip1[4] = 3;
#line 176
    flip1[5] = 2;
    flip1[6] = 3;
    flip1[7] = 5;
    flip1[8] = 13;
    flip1[9] = 0;
#line 177
    flip1[10] = 0;
    flip1[11] = 13;
    flip1[12] = 5;
    flip1[13] = 3;
    flip1[14] = 2;
#line 178
    flip1[15] = 3;
    flip1[16] = 4;
    flip1[17] = 5;
    flip1[18] = 6;
    flip1[19] = 8;
#line 179
    flip1[20] = 0;
    flip1[21] = 0;
#line 181
    flip2[0] = 7;
    flip2[1] = 5;
    flip2[2] = 4;
    flip2[3] = 3;
    flip2[4] = 2;
#line 182
    flip2[5] = 1;
    flip2[6] = 1;
    flip2[7] = 1;
    flip2[8] = 1;
    flip2[9] = 0;
#line 183
    flip2[10] = 0;
    flip2[11] = 1;
    flip2[12] = 1;
    flip2[13] = 1;
    flip2[14] = 1;
#line 184
    flip2[15] = 2;
    flip2[16] = 3;
    flip2[17] = 4;
    flip2[18] = 5;
    flip2[19] = 7;
#line 185
    flip2[20] = 0;
    flip2[21] = 0;
#line 187
    flip3[0] = 12;
    flip3[1] = 9;
    flip3[2] = 6;
    flip3[3] = 3;
    flip3[4] = 0;
#line 188
    flip3[5] = 0;
    flip3[6] = 0;
    flip3[7] = 0;
    flip3[8] = 0;
    flip3[9] = 0;
#line 189
    flip3[10] = 0;
    flip3[11] = 0;
    flip3[12] = 0;
    flip3[13] = 0;
    flip3[14] = 0;
#line 190
    flip3[15] = 0;
    flip3[16] = 3;
    flip3[17] = 6;
    flip3[18] = 9;
    flip3[19] = 0;
#line 191
    flip3[20] = 0;
    flip3[21] = 0;
    i = 0;
    goto L4;
  L2:;
    return;
  L3:;
    if (newTex == NULL) goto L2; else goto L1;
  L4:;
    if (i <= 21) goto L5; else goto L7;
  L5:;
    if (Intro_skipped) goto L9; else goto L8;
  L6:;
    i = (i + 1);
    goto L4;
  L7:;
    return;
  L8:;
    if (Intro_PumpAndCheck()) goto L11; else goto L10;
  L9:;
    return;
  L10:;
#line 196
    (Intro_introTick++);
#line 198
    rate = flip1[i];
#line 199
    wide = flip2[i];
#line 201
    Platform_BeginFrame();
#line 202
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 203
    Canvas_Clear(Platform_ren);
    if (i < 11) goto L13; else goto L14;
  L11:;
#line 195
    Intro_skipped = 1;
    return;
  L12:;
#line 268
    Platform_EndFrame();
    if (flip3[i] > 0) goto L27; else goto L28;
  L13:;
#line 208
    Texture_DrawRegion(Platform_ren, newTex, m2_div(320, 2), 0, m2_div(320, 2), 200, m2_div(sw, 2), 0, m2_div(sw, 2), sh);
#line 212
    Texture_DrawRegion(Platform_ren, oldTex, 0, 0, m2_div(320, 2), 200, 0, 0, m2_div(sw, 2), sh);
    if (rate > 0) goto L16; else goto L15;
  L14:;
#line 236
    Texture_DrawRegion(Platform_ren, newTex, m2_div(320, 2), 0, m2_div(320, 2), 200, m2_div(sw, 2), 0, m2_div(sw, 2), sh);
    if (rate > 0) goto L21; else goto L22;
  L15:;
    goto L12;
  L16:;
#line 218
    dcol = 0;
#line 219
    scol = wide;
    goto L17;
  L17:;
    if (scol < 136) goto L18; else goto L19;
  L18:;
#line 221
    h = Intro_PageDet(scol);
#line 223
    Texture_DrawRegion(Platform_ren, oldTex, (m2_div(320, 2) + scol), h, wide, ((200 - h) - h), (m2_div(sw, 2) + m2_div((dcol * sw), 320)), m2_div((h * sh), 200), m2_div((wide * sw), 320), m2_div((((200 - h) - h) * sh), 200));
#line 228
    (dcol += wide);
#line 229
    (scol += rate);
    goto L17;
  L19:;
    goto L15;
  L20:;
    goto L12;
  L21:;
#line 242
    Texture_DrawRegion(Platform_ren, oldTex, 24, 0, 135, 200, m2_div((24 * sw), 320), 0, m2_div((135 * sw), 320), sh);
#line 246
    dcol = 0;
#line 247
    scol = wide;
    goto L23;
  L22:;
#line 262
    Texture_DrawRegion(Platform_ren, newTex, 0, 0, m2_div(320, 2), 200, 0, 0, m2_div(sw, 2), sh);
    goto L20;
  L23:;
    if (scol < 136) goto L24; else goto L25;
  L24:;
#line 249
    h = Intro_PageDet(scol);
#line 250
    Texture_DrawRegion(Platform_ren, newTex, ((m2_div(320, 2) - scol) - wide), h, wide, ((200 - h) - h), m2_div((((m2_div(320, 2) - dcol) - wide) * sw), 320), m2_div((h * sh), 200), m2_div((wide * sw), 320), m2_div((((200 - h) - h) * sh), 200));
#line 257
    (dcol += wide);
#line 258
    (scol += rate);
    goto L23;
  L25:;
    goto L20;
  L26:;
    goto L29;
  L27:;
#line 272
    d = m2_div((flip3[i] * 33), 3);
    goto L26;
  L28:;
#line 274
    d = 33;
    goto L26;
  L29:;
    if (d > 0) goto L30; else goto L31;
  L30:;
    if (d > 20) goto L33; else goto L34;
  L31:;
    goto L6;
  L32:;
#line 278
    Music_UpdateMusic();
#line 279
    (d -= 20);
    goto L29;
  L33:;
#line 277
    Platform_DelayMs(20);
    goto L32;
  L34:;
    Platform_DelayMs(d);
    goto L32;
}

static void Intro_CenterStr(char *s, uint32_t s_high, int32_t y, int32_t sc) {
    (void)s;
    (void)s_high;
    (void)y;
    (void)sc;
    int32_t w;
    int32_t x;
    int32_t sw;
#line 287
    sw = (320 * 3);
#line 288
    w = HudFont_ScreenStrWidth(s, s_high, sc);
#line 289
    x = m2_div((sw - w), 2);
#line 290
    HudFont_DrawScreenStr(Platform_ren, s, s_high, x, y, sc);
    return;
}

static void Intro_ShowCredits(void) {
    int32_t i;
    int32_t sw;
    int32_t sh;
    int32_t sc;
#line 296
    sw = (320 * 3);
#line 297
    sh = ((143 + 57) * 3);
#line 298
    sc = 2;
#line 301
    HudFont_SetFontColor(255, 255, 255);
    i = 0;
    goto L1;
  L1:;
    if (i <= 15) goto L2; else goto L4;
  L2:;
    if (Intro_PumpAndCheck()) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 322
    Intro_Wait(120);
    if (Intro_skipped) goto L8; else goto L7;
  L5:;
#line 304
    (Intro_introTick++);
#line 305
    Platform_BeginFrame();
#line 306
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 307
    Canvas_Clear(Platform_ren);
#line 308
    Intro_CenterStr("\"The Faery Tale Adventure\"", 26, m2_div(sh, 6), sc);
#line 309
    Intro_CenterStr("Animation, Programming and Music", 32, m2_div((sh * 2), 6), sc);
#line 310
    Intro_CenterStr("by", 2, (m2_div((sh * 2), 6) + (20 * sc)), sc);
#line 311
    Intro_CenterStr("David Joiner", 12, m2_div((sh * 3), 6), sc);
#line 312
    Intro_CenterStr("Copyright (C) 1986 MicroIllusions", 33, m2_div((sh * 4), 6), sc);
#line 313
    Intro_CenterStr("Modula-2 port by Matt Fitzgerald", 32, (m2_div((sh * 4), 6) + (20 * sc)), sc);
#line 314
    Platform_EndFrame();
#line 315
    Music_UpdateMusic();
    Platform_DelayMs(20);
#line 316
    Music_UpdateMusic();
    Platform_DelayMs(20);
#line 317
    Music_UpdateMusic();
    Platform_DelayMs(20);
#line 318
    Music_UpdateMusic();
    Platform_DelayMs(33);
    goto L3;
  L6:;
#line 303
    Intro_skipped = 1;
    return;
  L7:;
    i = 15;
    goto L9;
  L8:;
    return;
  L9:;
    if (i >= 0) goto L10; else goto L12;
  L10:;
    if (Intro_PumpAndCheck()) goto L14; else goto L13;
  L11:;
    i = (i + (-1));
    goto L9;
  L12:;
#line 338
    HudFont_ResetFontColor();
    return;
  L13:;
#line 328
    (Intro_introTick++);
#line 329
    Platform_BeginFrame();
#line 330
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 331
    Canvas_Clear(Platform_ren);
#line 332
    Platform_EndFrame();
#line 333
    Music_UpdateMusic();
    Platform_DelayMs(20);
#line 334
    Music_UpdateMusic();
    Platform_DelayMs(20);
#line 335
    Music_UpdateMusic();
    Platform_DelayMs(20);
#line 336
    Music_UpdateMusic();
    Platform_DelayMs(33);
    goto L11;
  L14:;
#line 327
    Intro_skipped = 1;
    HudFont_ResetFontColor();
    return;
}

static void Intro_DrawGreekKey(void) {
    int32_t sw;
    int32_t sh;
    int32_t bx;
    int32_t by;
    int32_t bw;
    int32_t bh;
    int32_t sz;
    int32_t i;
    int32_t x;
    int32_t y;
#line 344
    sw = (320 * 3);
#line 345
    sh = ((143 + 57) * 3);
#line 346
    sz = 24;
#line 348
    Canvas_SetColor(Platform_ren, 180, 0, 0, 255);
#line 351
    by = m2_div(sh, 10);
    i = 0;
    goto L1;
  L1:;
    if (i <= m2_div(sw, sz)) goto L2; else goto L4;
  L2:;
#line 353
    x = (i * sz);
#line 354
    Canvas_FillRect(Platform_ren, x, by, sz, 3);
#line 355
    Canvas_FillRect(Platform_ren, x, by, 3, sz);
#line 356
    Canvas_FillRect(Platform_ren, (x + 6), (by + 6), (sz - 6), 3);
#line 357
    Canvas_FillRect(Platform_ren, ((x + sz) - 3), (by + 6), 3, m2_div(sz, 2));
#line 358
    Canvas_FillRect(Platform_ren, (x + 6), ((by + m2_div(sz, 2)) + 3), (sz - 6), 3);
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 362
    by = ((sh - m2_div(sh, 10)) - sz);
    i = 0;
    goto L5;
  L5:;
    if (i <= m2_div(sw, sz)) goto L6; else goto L8;
  L6:;
#line 364
    x = (i * sz);
#line 365
    Canvas_FillRect(Platform_ren, x, ((by + sz) - 3), sz, 3);
#line 366
    Canvas_FillRect(Platform_ren, ((x + sz) - 3), by, 3, sz);
#line 367
    Canvas_FillRect(Platform_ren, x, ((by + sz) - 9), (sz - 6), 3);
#line 368
    Canvas_FillRect(Platform_ren, x, ((by + m2_div(sz, 2)) - 3), 3, m2_div(sz, 2));
#line 369
    Canvas_FillRect(Platform_ren, x, ((by + m2_div(sz, 2)) - 3), (sz - 6), 3);
    goto L7;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
#line 373
    bx = m2_div(sw, 20);
    i = 0;
    goto L9;
  L9:;
    if (i <= m2_div(sh, sz)) goto L10; else goto L12;
  L10:;
#line 375
    y = (i * sz);
#line 376
    Canvas_FillRect(Platform_ren, bx, y, 3, sz);
#line 377
    Canvas_FillRect(Platform_ren, bx, y, sz, 3);
#line 378
    Canvas_FillRect(Platform_ren, (bx + 6), (y + 6), 3, (sz - 6));
#line 379
    Canvas_FillRect(Platform_ren, (bx + 6), ((y + sz) - 3), m2_div(sz, 2), 3);
#line 380
    Canvas_FillRect(Platform_ren, ((bx + m2_div(sz, 2)) + 3), y, 3, (sz - 6));
    goto L11;
  L11:;
    i = (i + 1);
    goto L9;
  L12:;
#line 384
    bx = ((sw - m2_div(sw, 20)) - sz);
    i = 0;
    goto L13;
  L13:;
    if (i <= m2_div(sh, sz)) goto L14; else goto L16;
  L14:;
#line 386
    y = (i * sz);
#line 387
    Canvas_FillRect(Platform_ren, ((bx + sz) - 3), y, 3, sz);
#line 388
    Canvas_FillRect(Platform_ren, bx, ((y + sz) - 3), sz, 3);
#line 389
    Canvas_FillRect(Platform_ren, ((bx + sz) - 9), y, 3, (sz - 6));
#line 390
    Canvas_FillRect(Platform_ren, ((bx + m2_div(sz, 2)) - 3), y, m2_div(sz, 2), 3);
#line 391
    Canvas_FillRect(Platform_ren, ((bx + m2_div(sz, 2)) - 3), (y + 6), 3, (sz - 6));
    goto L15;
  L15:;
    i = (i + 1);
    goto L13;
  L16:;
    return;
}

static void Intro_ShowPlacard(void) {
    int32_t sh;
    int32_t sc;
    int32_t lh;
    if (Intro_skipped) goto L2; else goto L1;
  L1:;
#line 400
    sh = ((143 + 57) * 3);
#line 401
    sc = 2;
#line 402
    lh = (14 * sc);
#line 404
    HudFont_SetFontColor(180, 0, 0);
#line 406
    Platform_BeginFrame();
#line 407
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 408
    Canvas_Clear(Platform_ren);
#line 409
    Intro_DrawGreekKey();
#line 410
    Intro_CenterStr("\"Rescue the Talisman!\"", 22, m2_div(sh, 6), sc);
#line 411
    Intro_CenterStr("was the Mayor's plea.", 21, (m2_div(sh, 6) + lh), sc);
#line 412
    Intro_CenterStr("\"Only the Talisman can", 22, (m2_div(sh, 6) + (lh * 3)), sc);
#line 413
    Intro_CenterStr("protect our village from", 24, (m2_div(sh, 6) + (lh * 4)), sc);
#line 414
    Intro_CenterStr("the evil forces of the", 22, (m2_div(sh, 6) + (lh * 5)), sc);
#line 415
    Intro_CenterStr("night.\" And so Julian", 21, (m2_div(sh, 6) + (lh * 6)), sc);
#line 416
    Intro_CenterStr("set out on his quest to", 23, (m2_div(sh, 6) + (lh * 7)), sc);
#line 417
    Intro_CenterStr("recover it.", 11, (m2_div(sh, 6) + (lh * 8)), sc);
#line 418
    Platform_EndFrame();
#line 420
    Intro_Wait(250);
#line 422
    HudFont_ResetFontColor();
    return;
  L2:;
    return;
}

static void Intro_RunIntro(void) {
#line 427
    Intro_skipped = 0;
#line 428
    Intro_introTick = 0;
#line 430
    Intro_cover = Intro_LoadPage("page0.bmp", 9);
#line 431
    Intro_spread1 = Intro_LoadPage("spread_1.bmp", 12);
#line 432
    Intro_spread2 = Intro_LoadPage("spread_2.bmp", 12);
#line 433
    Intro_spread3 = Intro_LoadPage("spread_3.bmp", 12);
    if (Intro_cover == NULL) goto L2; else goto L1;
  L1:;
#line 437
    m2_WriteString("Intro: starting");
    m2_WriteLn();
#line 438
    Platform_DelayMs(500);
#line 440
    Music_SetMood(12);
#line 443
    Intro_ShowCredits();
    if (Intro_skipped) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 447
    Platform_BeginFrame();
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
    Canvas_Clear(Platform_ren);
    Platform_EndFrame();
#line 448
    Intro_Wait(30);
    if (Intro_skipped) goto L6; else goto L5;
  L4:;
    return;
  L5:;
#line 452
    Intro_ZoomIn(Intro_cover);
    if (Intro_skipped) goto L8; else goto L7;
  L6:;
    return;
  L7:;
#line 454
    Intro_DrawTex(Intro_cover);
#line 455
    Intro_Wait(60);
    if (Intro_skipped) goto L10; else goto L9;
  L8:;
    return;
  L9:;
#line 459
    Intro_FlipScan(Intro_cover, Intro_spread1);
    if (Intro_skipped) goto L12; else goto L11;
  L10:;
    return;
  L11:;
#line 461
    Intro_DrawTex(Intro_spread1);
#line 462
    Intro_Wait(200);
    if (Intro_skipped) goto L14; else goto L13;
  L12:;
    return;
  L13:;
#line 466
    Intro_FlipScan(Intro_spread1, Intro_spread2);
    if (Intro_skipped) goto L16; else goto L15;
  L14:;
    return;
  L15:;
#line 468
    Intro_DrawTex(Intro_spread2);
#line 469
    Intro_Wait(200);
    if (Intro_skipped) goto L18; else goto L17;
  L16:;
    return;
  L17:;
#line 473
    Intro_FlipScan(Intro_spread2, Intro_spread3);
    if (Intro_skipped) goto L20; else goto L19;
  L18:;
    return;
  L19:;
#line 475
    Intro_DrawTex(Intro_spread3);
#line 476
    Intro_Wait(200);
    if (Intro_skipped) goto L22; else goto L21;
  L20:;
    return;
  L21:;
#line 480
    Intro_ZoomOut(Intro_spread3);
#line 483
    Intro_ShowPlacard();
#line 486
    Platform_BeginFrame();
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
    Canvas_Clear(Platform_ren);
    Platform_EndFrame();
    return;
  L22:;
    return;
}

/* Imported Module Compass */

static const int32_t Compass_CompW = 48;
static const int32_t Compass_CompH = 25;
static const int32_t Compass_CompScrX = 850;
static const int32_t Compass_CompScrY = 45;
static const int32_t Compass_CompScrW = 72;
static const int32_t Compass_CompScrH = 75;
static void Compass_InitCompass(void * ren);
static void Compass_DrawCompass(void * ren, int32_t dir);

void * Compass_dirTex[8 + 1];
static void Compass_InitCompass(void * ren) {
    (void)ren;
    int32_t i;
    char p[127 + 1];
    char num[127 + 1];
    i = 0;
    goto L1;
  L1:;
    if (i <= 8) goto L2; else goto L4;
  L2:;
    if ((i == 0)) goto L7;
    if ((i == 1)) goto L8;
    if ((i == 2)) goto L9;
    if ((i == 3)) goto L10;
    if ((i == 4)) goto L11;
    if ((i == 5)) goto L12;
    if ((i == 6)) goto L13;
    if ((i == 7)) goto L14;
    if ((i == 8)) goto L15;
    goto L6;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    return;
  L5:;
#line 42 "src/Compass.mod"
    Assets_AssetPath(num, 127, p, 127);
#line 43
    Compass_dirTex[i] = Platform_LoadBMPTexture(p, 127);
    if (Compass_dirTex[i] == NULL) goto L17; else goto L18;
  L6:;
#line 40
    m2_Strings_Assign("compass_8.bmp", num, 127);
    goto L5;
  L7:;
#line 30
    m2_Strings_Assign("compass_0.bmp", num, 127);
    goto L5;
  L8:;
#line 31
    m2_Strings_Assign("compass_1.bmp", num, 127);
    goto L5;
  L9:;
#line 32
    m2_Strings_Assign("compass_2.bmp", num, 127);
    goto L5;
  L10:;
#line 33
    m2_Strings_Assign("compass_3.bmp", num, 127);
    goto L5;
  L11:;
#line 34
    m2_Strings_Assign("compass_4.bmp", num, 127);
    goto L5;
  L12:;
#line 35
    m2_Strings_Assign("compass_5.bmp", num, 127);
    goto L5;
  L13:;
#line 36
    m2_Strings_Assign("compass_6.bmp", num, 127);
    goto L5;
  L14:;
#line 37
    m2_Strings_Assign("compass_7.bmp", num, 127);
    goto L5;
  L15:;
#line 38
    m2_Strings_Assign("compass_8.bmp", num, 127);
    goto L5;
  L16:;
    goto L3;
  L17:;
#line 45
    m2_WriteString("Compass FAIL: ");
    m2_WriteString(num);
    m2_WriteLn();
    goto L16;
  L18:;
#line 47
    m2_WriteString("Compass OK: ");
    m2_WriteString(num);
    m2_WriteLn();
    goto L16;
}

static void Compass_DrawCompass(void * ren, int32_t dir) {
    (void)ren;
    (void)dir;
    int32_t origDir;
    int32_t hudY;
    void * tex;
#line 56
    hudY = (143 * 3);
    if (dir >= 0) goto L4; else goto L3;
  L1:;
#line 75
    tex = Compass_dirTex[origDir];
    if (tex == NULL) goto L16; else goto L15;
  L2:;
    if ((dir == 0)) goto L7;
    if ((dir == 1)) goto L8;
    if ((dir == 2)) goto L9;
    if ((dir == 3)) goto L10;
    if ((dir == 4)) goto L11;
    if ((dir == 5)) goto L12;
    if ((dir == 6)) goto L13;
    if ((dir == 7)) goto L14;
    goto L6;
  L3:;
#line 72
    origDir = 8;
    goto L1;
  L4:;
    if (dir <= 7) goto L2; else goto L3;
  L5:;
    goto L1;
  L6:;
#line 69
    origDir = 8;
    goto L5;
  L7:;
#line 60
    origDir = 1;
    goto L5;
  L8:;
#line 61
    origDir = 2;
    goto L5;
  L9:;
#line 62
    origDir = 3;
    goto L5;
  L10:;
#line 63
    origDir = 4;
    goto L5;
  L11:;
#line 64
    origDir = 5;
    goto L5;
  L12:;
#line 65
    origDir = 6;
    goto L5;
  L13:;
#line 66
    origDir = 7;
    goto L5;
  L14:;
#line 67
    origDir = 0;
    goto L5;
  L15:;
#line 78
    Platform_DrawTexRegion(tex, 0, 0, 48, 25, 850, (hudY + 45), 72, 75);
    return;
  L16:;
    return;
}

/* Imported Module DebugMap */

static const int32_t DebugMap_FullW = 2048;
static const int32_t DebugMap_FullH = 1024;
static const int32_t DebugMap_WinW = 768;
static const int32_t DebugMap_WinH = 768;
static void DebugMap_InitPalette(void);
static void DebugMap_LoadWorldData(void);
static void DebugMap_InitTileColors(void);
static void DebugMap_GetTileMapColor(int32_t tileByte, int32_t *r, int32_t *g, int32_t *b);
static void DebugMap_InitDebugMap(void);
static int DebugMap_IsOpen(void);
static void DebugMap_ToggleDebugMap(void);
static int32_t DebugMap_GetWorldTile(int32_t tileX, int32_t tileY);
static void DebugMap_DrawFullMap(void);
static void DebugMap_DrawPlayerDot(void);
static void DebugMap_UpdateDebugMap(void);

void * DebugMap_dbgWin;
void * DebugMap_dbgRen;
int DebugMap_open;
int DebugMap_needRedraw;
int32_t DebugMap_lastDrawnRegion;
int32_t DebugMap_palR[31 + 1];
int32_t DebugMap_palG[31 + 1];
int32_t DebugMap_palB[31 + 1];
int32_t DebugMap_tc[255 + 1];
char DebugMap_wSector[32767 + 1];
char DebugMap_wMap[3 + 1][4095 + 1];
int DebugMap_worldLoaded;
static void DebugMap_InitPalette(void) {
#line 42 "src/DebugMap.mod"
    DebugMap_palR[0] = 0;
    DebugMap_palG[0] = 0;
    DebugMap_palB[0] = 0;
    DebugMap_palR[1] = 255;
    DebugMap_palG[1] = 255;
    DebugMap_palB[1] = 255;
#line 43
    DebugMap_palR[2] = 238;
    DebugMap_palG[2] = 153;
    DebugMap_palB[2] = 102;
    DebugMap_palR[3] = 187;
    DebugMap_palG[3] = 102;
    DebugMap_palB[3] = 51;
#line 44
    DebugMap_palR[4] = 102;
    DebugMap_palG[4] = 51;
    DebugMap_palB[4] = 17;
    DebugMap_palR[5] = 119;
    DebugMap_palG[5] = 187;
    DebugMap_palB[5] = 255;
#line 45
    DebugMap_palR[6] = 51;
    DebugMap_palG[6] = 51;
    DebugMap_palB[6] = 51;
    DebugMap_palR[7] = 221;
    DebugMap_palG[7] = 187;
    DebugMap_palB[7] = 136;
#line 46
    DebugMap_palR[8] = 34;
    DebugMap_palG[8] = 34;
    DebugMap_palB[8] = 51;
    DebugMap_palR[9] = 68;
    DebugMap_palG[9] = 68;
    DebugMap_palB[9] = 85;
#line 47
    DebugMap_palR[10] = 136;
    DebugMap_palG[10] = 136;
    DebugMap_palB[10] = 153;
    DebugMap_palR[11] = 187;
    DebugMap_palG[11] = 187;
    DebugMap_palB[11] = 204;
#line 48
    DebugMap_palR[12] = 85;
    DebugMap_palG[12] = 34;
    DebugMap_palB[12] = 17;
    DebugMap_palR[13] = 153;
    DebugMap_palG[13] = 68;
    DebugMap_palB[13] = 17;
#line 49
    DebugMap_palR[14] = 255;
    DebugMap_palG[14] = 136;
    DebugMap_palB[14] = 34;
    DebugMap_palR[15] = 255;
    DebugMap_palG[15] = 204;
    DebugMap_palB[15] = 119;
#line 50
    DebugMap_palR[16] = 0;
    DebugMap_palG[16] = 68;
    DebugMap_palB[16] = 0;
    DebugMap_palR[17] = 0;
    DebugMap_palG[17] = 119;
    DebugMap_palB[17] = 0;
#line 51
    DebugMap_palR[18] = 0;
    DebugMap_palG[18] = 187;
    DebugMap_palB[18] = 0;
    DebugMap_palR[19] = 102;
    DebugMap_palG[19] = 255;
    DebugMap_palB[19] = 102;
#line 52
    DebugMap_palR[20] = 0;
    DebugMap_palG[20] = 0;
    DebugMap_palB[20] = 85;
    DebugMap_palR[21] = 0;
    DebugMap_palG[21] = 0;
    DebugMap_palB[21] = 153;
#line 53
    DebugMap_palR[22] = 0;
    DebugMap_palG[22] = 0;
    DebugMap_palB[22] = 221;
    DebugMap_palR[23] = 51;
    DebugMap_palG[23] = 119;
    DebugMap_palB[23] = 255;
#line 54
    DebugMap_palR[24] = 204;
    DebugMap_palG[24] = 0;
    DebugMap_palB[24] = 0;
    DebugMap_palR[25] = 255;
    DebugMap_palG[25] = 85;
    DebugMap_palB[25] = 0;
#line 55
    DebugMap_palR[26] = 255;
    DebugMap_palG[26] = 170;
    DebugMap_palB[26] = 0;
    DebugMap_palR[27] = 255;
    DebugMap_palG[27] = 255;
    DebugMap_palB[27] = 102;
#line 56
    DebugMap_palR[28] = 238;
    DebugMap_palG[28] = 187;
    DebugMap_palB[28] = 102;
    DebugMap_palR[29] = 238;
    DebugMap_palG[29] = 170;
    DebugMap_palB[29] = 85;
#line 57
    DebugMap_palR[30] = 0;
    DebugMap_palG[30] = 0;
    DebugMap_palB[30] = 255;
    DebugMap_palR[31] = 187;
    DebugMap_palG[31] = 221;
    DebugMap_palB[31] = 255;
    return;
}

static void DebugMap_LoadWorldData(void) {
    int32_t fd;
    int32_t n;
    char modeBuf[3 + 1];
    char p[127 + 1];
#line 65
    m2_Strings_Assign("rb", modeBuf, 3);
#line 67
    Assets_AssetPath("sector_032.bin", 14, p, 127);
#line 68
    fd = m2sys_fopen(((void *)&(p)), ((void *)&(modeBuf)));
    if (fd >= 0) goto L2; else goto L1;
  L1:;
#line 74
    Assets_AssetPath("map_160.bin", 11, p, 127);
#line 75
    fd = m2sys_fopen(((void *)&(p)), ((void *)&(modeBuf)));
    if (fd >= 0) goto L4; else goto L3;
  L2:;
#line 70
    n = m2sys_fread_bytes(fd, ((void *)&(DebugMap_wSector)), 32768);
#line 71
    m2sys_fclose(fd);
    goto L1;
  L3:;
#line 78
    Assets_AssetPath("map_168.bin", 11, p, 127);
#line 79
    fd = m2sys_fopen(((void *)&(p)), ((void *)&(modeBuf)));
    if (fd >= 0) goto L6; else goto L5;
  L4:;
#line 76
    n = m2sys_fread_bytes(fd, ((void *)&(DebugMap_wMap[0])), 4096);
    m2sys_fclose(fd);
    goto L3;
  L5:;
#line 82
    Assets_AssetPath("map_176.bin", 11, p, 127);
#line 83
    fd = m2sys_fopen(((void *)&(p)), ((void *)&(modeBuf)));
    if (fd >= 0) goto L8; else goto L7;
  L6:;
#line 80
    n = m2sys_fread_bytes(fd, ((void *)&(DebugMap_wMap[1])), 4096);
    m2sys_fclose(fd);
    goto L5;
  L7:;
#line 86
    Assets_AssetPath("map_184.bin", 11, p, 127);
#line 87
    fd = m2sys_fopen(((void *)&(p)), ((void *)&(modeBuf)));
    if (fd >= 0) goto L10; else goto L9;
  L8:;
#line 84
    n = m2sys_fread_bytes(fd, ((void *)&(DebugMap_wMap[2])), 4096);
    m2sys_fclose(fd);
    goto L7;
  L9:;
#line 90
    DebugMap_worldLoaded = 1;
    return;
  L10:;
#line 88
    n = m2sys_fread_bytes(fd, ((void *)&(DebugMap_wMap[3])), 4096);
    m2sys_fclose(fd);
    goto L9;
}

static void DebugMap_InitTileColors(void) {
#line 95
    DebugMap_tc[0] = 11;
    DebugMap_tc[1] = 14;
    DebugMap_tc[2] = 12;
    DebugMap_tc[3] = 14;
    DebugMap_tc[4] = 12;
    DebugMap_tc[5] = 14;
    DebugMap_tc[6] = 12;
    DebugMap_tc[7] = 17;
#line 96
    DebugMap_tc[8] = 12;
    DebugMap_tc[9] = 14;
    DebugMap_tc[10] = 17;
    DebugMap_tc[11] = 17;
    DebugMap_tc[12] = 12;
    DebugMap_tc[13] = 14;
    DebugMap_tc[14] = 17;
    DebugMap_tc[15] = 8;
#line 97
    DebugMap_tc[16] = 8;
    DebugMap_tc[17] = 17;
    DebugMap_tc[18] = 21;
    DebugMap_tc[19] = 21;
    DebugMap_tc[20] = 21;
    DebugMap_tc[21] = 20;
    DebugMap_tc[22] = 23;
    DebugMap_tc[23] = 17;
#line 98
    DebugMap_tc[24] = 15;
    DebugMap_tc[25] = 17;
    DebugMap_tc[26] = 23;
    DebugMap_tc[27] = 5;
    DebugMap_tc[28] = 15;
    DebugMap_tc[29] = 17;
    DebugMap_tc[30] = 23;
    DebugMap_tc[31] = 23;
#line 99
    DebugMap_tc[32] = 15;
    DebugMap_tc[33] = 5;
    DebugMap_tc[34] = 21;
    DebugMap_tc[35] = 15;
    DebugMap_tc[36] = 15;
    DebugMap_tc[37] = 17;
    DebugMap_tc[38] = 16;
    DebugMap_tc[39] = 17;
#line 100
    DebugMap_tc[40] = 17;
    DebugMap_tc[41] = 17;
    DebugMap_tc[42] = 16;
    DebugMap_tc[43] = 17;
    DebugMap_tc[44] = 16;
    DebugMap_tc[45] = 16;
    DebugMap_tc[46] = 16;
    DebugMap_tc[47] = 16;
#line 101
    DebugMap_tc[48] = 16;
    DebugMap_tc[49] = 17;
    DebugMap_tc[50] = 16;
    DebugMap_tc[51] = 16;
    DebugMap_tc[52] = 16;
    DebugMap_tc[53] = 17;
    DebugMap_tc[54] = 16;
    DebugMap_tc[55] = 17;
#line 102
    DebugMap_tc[56] = 17;
    DebugMap_tc[57] = 16;
    DebugMap_tc[58] = 16;
    DebugMap_tc[59] = 16;
    DebugMap_tc[60] = 16;
    DebugMap_tc[61] = 17;
    DebugMap_tc[62] = 17;
    DebugMap_tc[63] = 17;
#line 103
    DebugMap_tc[64] = 8;
    DebugMap_tc[65] = 8;
    DebugMap_tc[66] = 0;
    DebugMap_tc[67] = 8;
    DebugMap_tc[68] = 16;
    DebugMap_tc[69] = 16;
    DebugMap_tc[70] = 18;
    DebugMap_tc[71] = 16;
#line 104
    DebugMap_tc[72] = 16;
    DebugMap_tc[73] = 16;
    DebugMap_tc[74] = 8;
    DebugMap_tc[75] = 0;
    DebugMap_tc[76] = 17;
    DebugMap_tc[77] = 17;
    DebugMap_tc[78] = 17;
    DebugMap_tc[79] = 17;
#line 105
    DebugMap_tc[80] = 16;
    DebugMap_tc[81] = 17;
    DebugMap_tc[82] = 17;
    DebugMap_tc[83] = 1;
    DebugMap_tc[84] = 23;
    DebugMap_tc[85] = 17;
    DebugMap_tc[86] = 5;
    DebugMap_tc[87] = 1;
#line 106
    DebugMap_tc[88] = 5;
    DebugMap_tc[89] = 17;
    DebugMap_tc[90] = 1;
    DebugMap_tc[91] = 16;
    DebugMap_tc[92] = 31;
    DebugMap_tc[93] = 29;
    DebugMap_tc[94] = 28;
    DebugMap_tc[95] = 29;
#line 107
    DebugMap_tc[96] = 15;
    DebugMap_tc[97] = 3;
    DebugMap_tc[98] = 31;
    DebugMap_tc[99] = 31;
    DebugMap_tc[100] = 31;
    DebugMap_tc[101] = 29;
    DebugMap_tc[102] = 31;
    DebugMap_tc[103] = 17;
#line 108
    DebugMap_tc[104] = 16;
    DebugMap_tc[105] = 3;
    DebugMap_tc[106] = 31;
    DebugMap_tc[107] = 31;
    DebugMap_tc[108] = 17;
    DebugMap_tc[109] = 17;
    DebugMap_tc[110] = 8;
    DebugMap_tc[111] = 9;
#line 109
    DebugMap_tc[112] = 10;
    DebugMap_tc[113] = 10;
    DebugMap_tc[114] = 10;
    DebugMap_tc[115] = 17;
    DebugMap_tc[116] = 17;
    DebugMap_tc[117] = 10;
    DebugMap_tc[118] = 9;
    DebugMap_tc[119] = 23;
#line 110
    DebugMap_tc[120] = 23;
    DebugMap_tc[121] = 9;
    DebugMap_tc[122] = 10;
    DebugMap_tc[123] = 15;
    DebugMap_tc[124] = 8;
    DebugMap_tc[125] = 9;
    DebugMap_tc[126] = 10;
    DebugMap_tc[127] = 15;
#line 111
    DebugMap_tc[128] = 5;
    DebugMap_tc[129] = 7;
    DebugMap_tc[130] = 6;
    DebugMap_tc[131] = 7;
    DebugMap_tc[132] = 6;
    DebugMap_tc[133] = 7;
    DebugMap_tc[134] = 6;
    DebugMap_tc[135] = 8;
#line 112
    DebugMap_tc[136] = 6;
    DebugMap_tc[137] = 7;
    DebugMap_tc[138] = 8;
    DebugMap_tc[139] = 8;
    DebugMap_tc[140] = 6;
    DebugMap_tc[141] = 7;
    DebugMap_tc[142] = 8;
    DebugMap_tc[143] = 4;
#line 113
    DebugMap_tc[144] = 4;
    DebugMap_tc[145] = 8;
    DebugMap_tc[146] = 10;
    DebugMap_tc[147] = 10;
    DebugMap_tc[148] = 10;
    DebugMap_tc[149] = 10;
    DebugMap_tc[150] = 11;
    DebugMap_tc[151] = 11;
#line 114
    DebugMap_tc[152] = 11;
    DebugMap_tc[153] = 8;
    DebugMap_tc[154] = 11;
    DebugMap_tc[155] = 2;
    DebugMap_tc[156] = 7;
    DebugMap_tc[157] = 8;
    DebugMap_tc[158] = 11;
    DebugMap_tc[159] = 11;
#line 115
    DebugMap_tc[160] = 7;
    DebugMap_tc[161] = 2;
    DebugMap_tc[162] = 11;
    DebugMap_tc[163] = 7;
    DebugMap_tc[164] = 7;
    DebugMap_tc[165] = 8;
    DebugMap_tc[166] = 8;
    DebugMap_tc[167] = 8;
#line 116
    DebugMap_tc[168] = 8;
    DebugMap_tc[169] = 8;
    DebugMap_tc[170] = 8;
    DebugMap_tc[171] = 8;
    DebugMap_tc[172] = 8;
    DebugMap_tc[173] = 8;
    DebugMap_tc[174] = 8;
    DebugMap_tc[175] = 8;
#line 117
    DebugMap_tc[176] = 8;
    DebugMap_tc[177] = 8;
    DebugMap_tc[178] = 8;
    DebugMap_tc[179] = 8;
    DebugMap_tc[180] = 8;
    DebugMap_tc[181] = 8;
    DebugMap_tc[182] = 8;
    DebugMap_tc[183] = 8;
#line 118
    DebugMap_tc[184] = 8;
    DebugMap_tc[185] = 8;
    DebugMap_tc[186] = 8;
    DebugMap_tc[187] = 8;
    DebugMap_tc[188] = 8;
    DebugMap_tc[189] = 8;
    DebugMap_tc[190] = 8;
    DebugMap_tc[191] = 8;
#line 119
    DebugMap_tc[192] = 5;
    DebugMap_tc[193] = 4;
    DebugMap_tc[194] = 1;
    DebugMap_tc[195] = 14;
    DebugMap_tc[196] = 14;
    DebugMap_tc[197] = 1;
    DebugMap_tc[198] = 8;
    DebugMap_tc[199] = 8;
#line 120
    DebugMap_tc[200] = 8;
    DebugMap_tc[201] = 8;
    DebugMap_tc[202] = 8;
    DebugMap_tc[203] = 8;
    DebugMap_tc[204] = 8;
    DebugMap_tc[205] = 5;
    DebugMap_tc[206] = 8;
    DebugMap_tc[207] = 8;
#line 121
    DebugMap_tc[208] = 5;
    DebugMap_tc[209] = 3;
    DebugMap_tc[210] = 3;
    DebugMap_tc[211] = 3;
    DebugMap_tc[212] = 3;
    DebugMap_tc[213] = 3;
    DebugMap_tc[214] = 3;
    DebugMap_tc[215] = 3;
#line 122
    DebugMap_tc[216] = 3;
    DebugMap_tc[217] = 3;
    DebugMap_tc[218] = 5;
    DebugMap_tc[219] = 4;
    DebugMap_tc[220] = 8;
    DebugMap_tc[221] = 6;
    DebugMap_tc[222] = 8;
    DebugMap_tc[223] = 1;
#line 123
    DebugMap_tc[224] = 6;
    DebugMap_tc[225] = 1;
    DebugMap_tc[226] = 8;
    DebugMap_tc[227] = 3;
    DebugMap_tc[228] = 1;
    DebugMap_tc[229] = 6;
    DebugMap_tc[230] = 3;
    DebugMap_tc[231] = 8;
#line 124
    DebugMap_tc[232] = 7;
    DebugMap_tc[233] = 8;
    DebugMap_tc[234] = 8;
    DebugMap_tc[235] = 3;
    DebugMap_tc[236] = 3;
    DebugMap_tc[237] = 7;
    DebugMap_tc[238] = 8;
    DebugMap_tc[239] = 8;
#line 125
    DebugMap_tc[240] = 8;
    DebugMap_tc[241] = 8;
    DebugMap_tc[242] = 8;
    DebugMap_tc[243] = 8;
    DebugMap_tc[244] = 8;
    DebugMap_tc[245] = 4;
    DebugMap_tc[246] = 8;
    DebugMap_tc[247] = 5;
#line 126
    DebugMap_tc[248] = 4;
    DebugMap_tc[249] = 5;
    DebugMap_tc[250] = 6;
    DebugMap_tc[251] = 1;
    DebugMap_tc[252] = 6;
    DebugMap_tc[253] = 6;
    DebugMap_tc[254] = 6;
    DebugMap_tc[255] = 1;
    return;
}

static void DebugMap_GetTileMapColor(int32_t tileByte, int32_t *r, int32_t *g, int32_t *b) {
    (void)tileByte;
    (void)r;
    (void)g;
    (void)b;
    int32_t idx;
    if (tileByte < 0) goto L2; else goto L3;
  L1:;
#line 136
    idx = DebugMap_tc[tileByte];
#line 137
    (*r) = DebugMap_palR[idx];
    (*g) = DebugMap_palG[idx];
    (*b) = DebugMap_palB[idx];
    return;
  L2:;
#line 133
    (*r) = 0;
    (*g) = 0;
    (*b) = 0;
    return;
  L3:;
    if (tileByte > 255) goto L2; else goto L1;
}

static void DebugMap_InitDebugMap(void) {
#line 142
    DebugMap_dbgWin = NULL;
#line 143
    DebugMap_dbgRen = NULL;
#line 144
    DebugMap_open = 0;
#line 145
    DebugMap_needRedraw = 1;
#line 146
    DebugMap_lastDrawnRegion = (-1);
#line 147
    DebugMap_worldLoaded = 0;
#line 148
    DebugMap_InitPalette();
#line 149
    DebugMap_InitTileColors();
    return;
}

static int DebugMap_IsOpen(void) {
    return DebugMap_open;
}

static void DebugMap_ToggleDebugMap(void) {
    if (DebugMap_open) goto L2; else goto L3;
  L1:;
    return;
  L2:;
    if (DebugMap_dbgRen != NULL) goto L5; else goto L4;
  L3:;
    if (DebugMap_worldLoaded) goto L8; else goto L9;
  L4:;
    if (DebugMap_dbgWin != NULL) goto L7; else goto L6;
  L5:;
#line 160
    Gfx_DestroyRenderer(DebugMap_dbgRen);
    DebugMap_dbgRen = NULL;
    goto L4;
  L6:;
#line 162
    DebugMap_open = 0;
    goto L1;
  L7:;
#line 161
    Gfx_DestroyWindow(DebugMap_dbgWin);
    DebugMap_dbgWin = NULL;
    goto L6;
  L8:;
#line 165
    DebugMap_dbgWin = Gfx_CreateWindow("Debug Map — Full World", 24, 768, 768, 1);
    if (DebugMap_dbgWin != NULL) goto L11; else goto L10;
  L9:;
#line 164
    DebugMap_LoadWorldData();
    goto L8;
  L10:;
    goto L1;
  L11:;
#line 168
    DebugMap_dbgRen = Gfx_CreateRenderer(DebugMap_dbgWin, 1);
    if (DebugMap_dbgRen != NULL) goto L13; else goto L14;
  L12:;
    goto L10;
  L13:;
#line 170
    DebugMap_open = 1;
#line 171
    DebugMap_needRedraw = 1;
    goto L12;
  L14:;
#line 173
    Gfx_DestroyWindow(DebugMap_dbgWin);
    DebugMap_dbgWin = NULL;
    goto L12;
}

static int32_t DebugMap_GetWorldTile(int32_t tileX, int32_t tileY) {
    (void)tileX;
    (void)tileY;
    int32_t mapRow;
    int32_t secx;
    int32_t secy;
    int32_t secNum;
    int32_t offset;
#line 183
    mapRow = m2_div(tileY, 256);
    if (mapRow < 0) goto L2; else goto L3;
  L1:;
#line 186
    secx = m2_mod(m2_div(tileX, 16), 128);
#line 187
    secy = m2_mod(m2_div(tileY, 8), 32);
#line 188
    offset = ((secy * 128) + secx);
    if (offset < 0) goto L5; else goto L6;
  L2:;
    return 19;
  L3:;
    if (mapRow > 3) goto L2; else goto L1;
  L4:;
#line 191
    secNum = ((int32_t)((unsigned char)(DebugMap_wMap[mapRow][offset])));
#line 192
    offset = (((secNum * 128) + (m2_mod(tileY, 8) * 16)) + m2_mod(tileX, 16));
    if (offset < 0) goto L8; else goto L9;
  L5:;
    return 19;
  L6:;
    if (offset >= 4096) goto L5; else goto L4;
  L7:;
    return ((int32_t)((unsigned char)(DebugMap_wSector[offset])));
  L8:;
    return 19;
  L9:;
    if (offset >= 32768) goto L8; else goto L7;
}

static void DebugMap_DrawFullMap(void) {
    int32_t tx;
    int32_t ty;
    int32_t tileByte;
    int32_t idx;
    int32_t px;
    int32_t py;
    int32_t r;
    int32_t g;
    int32_t b;
    if (DebugMap_dbgRen == NULL) goto L2; else goto L1;
  L1:;
#line 202
    Canvas_SetColor(DebugMap_dbgRen, 0, 0, 85, 255);
#line 203
    Canvas_Clear(DebugMap_dbgRen);
    ty = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (ty <= (1024 - 1)) goto L4; else goto L6;
  L4:;
    tx = 0;
    goto L7;
  L5:;
    ty = (ty + 1);
    goto L3;
  L6:;
#line 219
    DebugMap_needRedraw = 0;
    return;
  L7:;
    if (tx <= (2048 - 1)) goto L8; else goto L10;
  L8:;
#line 207
    tileByte = DebugMap_GetWorldTile(tx, ty);
#line 208
    idx = DebugMap_tc[tileByte];
#line 209
    r = DebugMap_palR[idx];
    g = DebugMap_palG[idx];
    b = DebugMap_palB[idx];
#line 211
    px = m2_div((tx * 768), 2048);
#line 212
    py = m2_div((ty * 768), 1024);
#line 213
    Canvas_SetColor(DebugMap_dbgRen, r, g, b, 255);
#line 214
    Canvas_FillRect(DebugMap_dbgRen, px, py, (m2_div(768, 2048) + 1), (m2_div(768, 1024) + 1));
    goto L9;
  L9:;
    tx = (tx + 1);
    goto L7;
  L10:;
    goto L5;
}

static void DebugMap_DrawPlayerDot(void) {
    int32_t imx;
    int32_t imy;
    int32_t dotX;
    int32_t dotY;
    if (DebugMap_dbgRen == NULL) goto L2; else goto L1;
  L1:;
#line 227
    imx = m2_div(Actor_actors[0].absX, 16);
#line 228
    imy = m2_div(Actor_actors[0].absY, 32);
#line 229
    dotX = m2_div((imx * 768), 2048);
#line 230
    dotY = m2_div((imy * 768), 1024);
    if (m2_mod(m2_div(GameState_cycle, 10), 2) == 0) goto L4; else goto L5;
  L2:;
    return;
  L3:;
#line 237
    Canvas_FillRect(DebugMap_dbgRen, (dotX - 3), (dotY - 3), 7, 7);
    return;
  L4:;
#line 233
    Canvas_SetColor(DebugMap_dbgRen, 255, 255, 255, 255);
    goto L3;
  L5:;
#line 235
    Canvas_SetColor(DebugMap_dbgRen, 255, 0, 0, 255);
    goto L3;
}

static void DebugMap_UpdateDebugMap(void) {
    if (DebugMap_open) goto L1; else goto L2;
  L1:;
    if (DebugMap_dbgRen == NULL) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (DebugMap_needRedraw) goto L6; else goto L7;
  L4:;
    return;
  L5:;
    return;
  L6:;
#line 246
    DebugMap_DrawFullMap();
#line 247
    DebugMap_DrawPlayerDot();
#line 248
    Gfx_Present(DebugMap_dbgRen);
    goto L5;
  L7:;
    if (m2_mod(GameState_cycle, 20) == 0) goto L8; else goto L5;
  L8:;
#line 250
    DebugMap_DrawFullMap();
#line 251
    DebugMap_DrawPlayerDot();
#line 252
    Gfx_Present(DebugMap_dbgRen);
    goto L5;
}

/* Imported Module Render */

typedef struct Render_StateEntry Render_StateEntry;
typedef struct Render_StateEntry Render_StateEntry;
struct Render_StateEntry {
    int32_t figure;
    int32_t wpnNo;
    int32_t wpnX;
    int32_t wpnY;
};

static const int32_t Render_TilePixW = 16;
static const int32_t Render_TilePixH = 32;
static const int32_t Render_SprW = 16;
static const int32_t Render_SprH = 32;
static const int32_t Render_WpnSprW = 16;
static const int32_t Render_WpnSprH = 16;
static void Render_InitOverlay(void);
static void Render_LoadCompass(void);
static int32_t Render_S(int32_t v);
static void Render_UpdateFade(void);
static void Render_DrawWorldTiled(void);
static void Render_DrawWorldFallback(void);
static void Render_TerrainColor(int32_t terrain, int32_t *r, int32_t *g, int32_t *b);
static void Render_DrawWorld(void);
static void Render_BuildSpriteMaskAt(int32_t worldX, int32_t worldY, int32_t groundY);
static void Render_BuildSpriteMaskFor(int32_t actorIdx);
static void Render_DrawItems(void);
static void Render_ItemColor(int32_t id, int32_t *r, int32_t *g, int32_t *b);
static int32_t Render_WalkBase(int32_t facing);
static int32_t Render_FightBase(int32_t facing);
static int32_t Render_GetPlayerFrame(int32_t i);
static int32_t Render_GetEnemyFrame(int32_t i);
static void Render_InitWpnState(void);
static void Render_InitBowOffsets(void);
static void Render_DrawWeaponOverlay(int32_t i, int32_t sx, int32_t sy, int32_t frame);
static void Render_DrawBrotherSprite(int32_t actorIdx, int32_t brotherIdx, int32_t frame, int32_t sx, int32_t sy, int32_t env);
static int32_t Render_RaceToTexIdx(int32_t race);
static void Render_DrawEnemySprite(int32_t actorIdx, int32_t texIdx, int32_t frame, int32_t sx, int32_t sy);
static int32_t Render_GetStateIdx(int32_t i);
static void Render_DrawActorBody(int32_t i, int32_t sx, int32_t sy);
static void Render_DrawActors(void);
static void Render_DrawHUD(void);
static void Render_DrawCompass(void);
static void Render_PalColor(int32_t idx, int32_t *r, int32_t *g, int32_t *b);
static void Render_GetOptionLabel(int32_t optIdx, char *buf, uint32_t buf_high);
static void Render_DrawMenu(void);
static void Render_DrawMinimap(void);
static void Render_DrawBirdView(void);
static void Render_DrawRegionFade(void);
static void Render_AppendInt(char *buf, uint32_t buf_high, int32_t *pos, int32_t n);
static void Render_BuildStat(char *label, uint32_t label_high, int32_t val, char *buf, uint32_t buf_high);
static void Render_DrawMessage(void);
static void Render_DrawInventory(void);
static void Render_DrawFairy(void);
static void Render_InitWitchPoints(void);
static void Render_DrawWitchBeam(void);

int32_t Render_fadeR;
int32_t Render_fadeG;
int32_t Render_fadeB;
int32_t Render_cpRng;
void * Render_compassBase;
void * Render_compassHi;
void * Render_raftTex;
void * Render_turtleTex;
void * Render_birdTex;
int Render_bmask[15 + 1][31 + 1];
Render_StateEntry Render_wpnState[86 + 1];
int Render_wpnInited;
int32_t Render_bowX[31 + 1];
int32_t Render_bowY[31 + 1];
int Render_bowInited;
int32_t Render_wpX[63 + 1];
int32_t Render_wpY[63 + 1];
int32_t Render_wpNX[63 + 1];
int32_t Render_wpNY[63 + 1];
int Render_witchInited;
static void Render_InitWpnState_W(int32_t i, int32_t fig, int32_t wn, int32_t wx, int32_t wy) {
#line 586 "src/Render.mod"
    Render_wpnState[i].figure = fig;
#line 587
    Render_wpnState[i].wpnNo = wn;
#line 588
    Render_wpnState[i].wpnX = wx;
#line 589
    Render_wpnState[i].wpnY = wy;
    return;
}

static void Render_DrawInventory_DrawInvSlot(int32_t imgNum, int32_t xoff, int32_t yoff, int32_t ydelta, int32_t imgOff, int32_t imgH, int32_t maxShow, int32_t count) {
    int32_t i;
    int32_t srcY;
    int32_t dx;
    int32_t dy;
    if (WorldObj_objTex == NULL) goto L2; else goto L1;
  L1:;
#line 1551
    Texture_SetColorMod(WorldObj_objTex, 255, 255, 255);
    if (count <= 0) goto L4; else goto L3;
  L2:;
    return;
  L3:;
    if (count > maxShow) goto L6; else goto L5;
  L4:;
    return;
  L5:;
#line 1555
    srcY = ((imgNum * 16) + imgOff);
#line 1557
    dx = Render_S((xoff + 20));
#line 1558
    dy = Render_S(yoff);
    i = 0;
    goto L7;
  L6:;
#line 1553
    count = maxShow;
    goto L5;
  L7:;
    if (i <= (count - 1)) goto L8; else goto L10;
  L8:;
#line 1560
    Platform_DrawTexRegion(WorldObj_objTex, 0, srcY, 16, imgH, dx, dy, Render_S(16), Render_S(imgH));
#line 1563
    (dy += Render_S(ydelta));
    goto L9;
  L9:;
    i = (i + 1);
    goto L7;
  L10:;
    return;
}

static void Render_InitOverlay(void) {
#line 56
    Render_fadeR = 255;
    Render_fadeG = 255;
    Render_fadeB = 255;
#line 57
    Render_cpRng = 77777;
#line 58
    Render_wpnInited = 0;
#line 59
    Render_bowInited = 0;
#line 60
    Render_witchInited = 0;
#line 61
    Render_compassBase = NULL;
#line 62
    Render_compassHi = NULL;
#line 63
    Render_UpdateFade();
    return;
}

static void Render_LoadCompass(void) {
    char p[127 + 1];
#line 69
    Assets_AssetPath("compass_base.bmp", 16, p, 127);
#line 70
    Render_compassBase = Platform_LoadBMPKeyedTexture(p, 127, 255, 0, 255);
#line 71
    Assets_AssetPath("compass_highlight.bmp", 21, p, 127);
#line 72
    Render_compassHi = Platform_LoadBMPKeyedTexture(p, 127, 255, 0, 255);
    if (Render_compassBase == NULL) goto L2; else goto L1;
  L1:;
    if (Render_compassHi == NULL) goto L4; else goto L3;
  L2:;
#line 74
    m2_WriteString("Compass: base load failed");
    m2_WriteLn();
    goto L1;
  L3:;
#line 80
    Assets_AssetPath("shape_4_Raft_32x32_x2.bmp", 25, p, 127);
#line 81
    Render_raftTex = Platform_LoadBMPKeyedTexture(p, 127, 255, 0, 255);
#line 82
    Assets_AssetPath("shape_5_Turtle_32x32_x16.bmp", 28, p, 127);
#line 83
    Render_turtleTex = Platform_LoadBMPKeyedTexture(p, 127, 255, 0, 255);
#line 84
    Assets_AssetPath("shape_11_Bird_64x64_x8.bmp", 26, p, 127);
#line 85
    Render_birdTex = Platform_LoadBMPKeyedTexture(p, 127, 255, 0, 255);
    return;
  L4:;
#line 77
    m2_WriteString("Compass: highlight load failed");
    m2_WriteLn();
    goto L3;
}

static int32_t Render_S(int32_t v) {
    (void)v;
    return (v * 3);
}

static void Render_UpdateFade(void) {
    int32_t r;
    int32_t g;
    int32_t b;
#line 104
    DayNight_GetFadeRGB(&r, &g, &b);
#line 105
    Render_fadeR = m2_div((r * 255), 100);
#line 106
    Render_fadeG = m2_div((g * 255), 100);
#line 107
    Render_fadeB = m2_div((b * 255), 100);
    if (Render_fadeR > 255) goto L2; else goto L1;
  L1:;
    if (Render_fadeG > 255) goto L4; else goto L3;
  L2:;
#line 108
    Render_fadeR = 255;
    goto L1;
  L3:;
    if (Render_fadeB > 255) goto L6; else goto L5;
  L4:;
#line 109
    Render_fadeG = 255;
    goto L3;
  L5:;
    return;
  L6:;
#line 110
    Render_fadeB = 255;
    goto L5;
}

static void Render_DrawWorldTiled(void) {
    int32_t imx;
    int32_t imy;
    int32_t sx;
    int32_t sy;
    int32_t secByte;
    int32_t imgIdx;
    int32_t tileY;
    int32_t tileReg;
    int32_t startIX;
    int32_t startIY;
    int32_t endIX;
    int32_t endIY;
    void * tex;
    if (GameState_colorPlayTimer > 0) goto L2; else goto L3;
  L1:;
#line 151
    Canvas_SetClip(Platform_ren, 0, 0, Render_S(320), Render_S(143));
#line 153
    startIX = (m2_div(World_camX, 16) - 1);
#line 154
    startIY = (m2_div(World_camY, 32) - 1);
#line 155
    endIX = (m2_div((World_camX + 320), 16) + 1);
#line 156
    endIY = (m2_div((World_camY + 143), 32) + 1);
    if (startIX < 0) goto L25; else goto L24;
  L2:;
#line 121
    Render_cpRng = ((Render_cpRng * 1103515245) + 12345);
    if (Render_cpRng < 0) goto L5; else goto L4;
  L3:;
    if (Assets_currentRegion >= 8) goto L10; else goto L11;
  L4:;
#line 123
    Render_fadeR = (m2_mod(m2_div(Render_cpRng, 256), 16) * 17);
#line 124
    Render_cpRng = ((Render_cpRng * 1103515245) + 12345);
    if (Render_cpRng < 0) goto L7; else goto L6;
  L5:;
#line 122
    Render_cpRng = (-Render_cpRng);
    goto L4;
  L6:;
#line 126
    Render_fadeG = (m2_mod(m2_div(Render_cpRng, 256), 16) * 17);
#line 127
    Render_cpRng = ((Render_cpRng * 1103515245) + 12345);
    if (Render_cpRng < 0) goto L9; else goto L8;
  L7:;
#line 125
    Render_cpRng = (-Render_cpRng);
    goto L6;
  L8:;
#line 129
    Render_fadeB = (m2_mod(m2_div(Render_cpRng, 256), 16) * 17);
#line 130
    (GameState_colorPlayTimer--);
    goto L1;
  L9:;
#line 128
    Render_cpRng = (-Render_cpRng);
    goto L8;
  L10:;
    if (Actor_actors[0].absX > 9216) goto L17; else goto L14;
  L11:;
    if (Render_fadeR == 255) goto L22; else goto L20;
  L12:;
    goto L1;
  L13:;
#line 139
    Render_fadeR = 140;
    Render_fadeG = 140;
    Render_fadeB = 255;
    goto L12;
  L14:;
#line 141
    Render_fadeR = 255;
    Render_fadeG = 255;
    Render_fadeB = 255;
    goto L12;
  L15:;
    if (Actor_actors[0].absY < 35328) goto L13; else goto L14;
  L16:;
    if (Actor_actors[0].absY > 33280) goto L15; else goto L14;
  L17:;
    if (Actor_actors[0].absX < 12544) goto L16; else goto L14;
  L18:;
    goto L1;
  L19:;
#line 145
    Render_UpdateFade();
    goto L18;
  L20:;
    if (DayNight_PaletteTickDue()) goto L23; else goto L18;
  L21:;
    if (Render_fadeB == 255) goto L19; else goto L20;
  L22:;
    if (Render_fadeG == 255) goto L21; else goto L20;
  L23:;
#line 147
    Render_UpdateFade();
    goto L18;
  L24:;
    if (startIY < 0) goto L27; else goto L26;
  L25:;
#line 157
    startIX = 0;
    goto L24;
  L26:;
    imy = startIY;
    goto L28;
  L27:;
#line 158
    startIY = 0;
    goto L26;
  L28:;
    if (imy <= endIY) goto L29; else goto L31;
  L29:;
    imx = startIX;
    goto L32;
  L30:;
    imy = (imy + 1);
    goto L28;
  L31:;
#line 182
    Canvas_ClearClip(Platform_ren);
    return;
  L32:;
    if (imx <= endIX) goto L33; else goto L35;
  L33:;
#line 165
    secByte = Assets_GetSectorByte((imx * 16), (imy * 32));
#line 166
    imgIdx = m2_div(secByte, 64);
#line 167
    tileY = (m2_mod(secByte, 64) * 32);
#line 168
    sx = ((imx * 16) - World_camX);
#line 169
    sy = ((imy * 32) - World_camY);
#line 170
    tex = Assets_tileTex[imgIdx];
    if (tex != NULL) goto L37; else goto L36;
  L34:;
    imx = (imx + 1);
    goto L32;
  L35:;
    goto L30;
  L36:;
    goto L34;
  L37:;
#line 174
    Texture_SetColorMod(tex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 175
    Platform_DrawTexRegion(tex, 0, tileY, 16, 32, Render_S(sx), Render_S(sy), Render_S(16), Render_S(32));
    goto L36;
}

static void Render_DrawWorldFallback(void) {
    int32_t tx;
    int32_t ty;
    int32_t sx;
    int32_t sy;
    int32_t r;
    int32_t g;
    int32_t b;
    int32_t startTX;
    int32_t startTY;
    int32_t endTX;
    int32_t endTY;
#line 189
    Canvas_SetClip(Platform_ren, 0, 0, Render_S(320), Render_S(143));
#line 191
    startTX = m2_div(World_camX, 16);
#line 192
    startTY = m2_div(World_camY, 16);
#line 193
    endTX = (m2_div((World_camX + 320), 16) + 1);
#line 194
    endTY = (m2_div((World_camY + 143), 16) + 1);
    if (startTX < 0) goto L2; else goto L1;
  L1:;
    if (startTY < 0) goto L4; else goto L3;
  L2:;
#line 196
    startTX = 0;
    goto L1;
  L3:;
    if (endTX > 64) goto L6; else goto L5;
  L4:;
#line 197
    startTY = 0;
    goto L3;
  L5:;
    if (endTY > 64) goto L8; else goto L7;
  L6:;
#line 198
    endTX = 64;
    goto L5;
  L7:;
    tx = startTX;
    goto L9;
  L8:;
#line 199
    endTY = 64;
    goto L7;
  L9:;
    if (tx <= (endTX - 1)) goto L10; else goto L12;
  L10:;
    ty = startTY;
    goto L13;
  L11:;
    tx = (tx + 1);
    goto L9;
  L12:;
#line 211
    Canvas_ClearClip(Platform_ren);
    return;
  L13:;
    if (ty <= (endTY - 1)) goto L14; else goto L16;
  L14:;
#line 203
    sx = (((tx * 16) - World_camX) * 3);
#line 204
    sy = (((ty * 16) - World_camY) * 3);
#line 205
    Render_TerrainColor(World_tiles[tx][ty].terrain, &r, &g, &b);
#line 206
    Canvas_SetColor(Platform_ren, r, g, b, 255);
#line 207
    Canvas_FillRect(Platform_ren, sx, sy, (16 * 3), (16 * 3));
    goto L15;
  L15:;
    ty = (ty + 1);
    goto L13;
  L16:;
    goto L11;
}

static void Render_TerrainColor(int32_t terrain, int32_t *r, int32_t *g, int32_t *b) {
    (void)terrain;
    (void)r;
    (void)g;
    (void)b;
    if ((terrain == 0)) goto L3;
    if ((terrain == 1)) goto L4;
    if ((terrain == 2)) goto L5;
    if ((terrain == 3)) goto L6;
    if ((terrain == 4)) goto L7;
    if ((terrain == 5)) goto L8;
    if ((terrain == 6)) goto L9;
    if ((terrain == 7)) goto L10;
    if ((terrain == 8)) goto L11;
    if ((terrain == 9)) goto L12;
    if ((terrain == 10)) goto L13;
    goto L2;
  L1:;
    return;
  L2:;
#line 229
    (*r) = 0;
    (*g) = 0;
    (*b) = 0;
    goto L1;
  L3:;
#line 217
    (*r) = 34;
    (*g) = 139;
    (*b) = 34;
    goto L1;
  L4:;
#line 218
    (*r) = 30;
    (*g) = 90;
    (*b) = 200;
    goto L1;
  L5:;
#line 219
    (*r) = 0;
    (*g) = 80;
    (*b) = 20;
    goto L1;
  L6:;
#line 220
    (*r) = 100;
    (*g) = 85;
    (*b) = 65;
    goto L1;
  L7:;
#line 221
    (*r) = 180;
    (*g) = 160;
    (*b) = 100;
    goto L1;
  L8:;
#line 222
    (*r) = 80;
    (*g) = 80;
    (*b) = 80;
    goto L1;
  L9:;
#line 223
    (*r) = 140;
    (*g) = 100;
    (*b) = 40;
    goto L1;
  L10:;
#line 224
    (*r) = 220;
    (*g) = 200;
    (*b) = 140;
    goto L1;
  L11:;
#line 225
    (*r) = 60;
    (*g) = 90;
    (*b) = 50;
    goto L1;
  L12:;
#line 226
    (*r) = 160;
    (*g) = 120;
    (*b) = 60;
    goto L1;
  L13:;
#line 227
    (*r) = 120;
    (*g) = 110;
    (*b) = 100;
    goto L1;
}

static void Render_DrawWorld(void) {
    if (Assets_currentRegion >= 0) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 236
    Render_DrawWorldTiled();
    goto L1;
  L3:;
#line 238
    Render_DrawWorldFallback();
    goto L1;
}

static void Render_BuildSpriteMaskAt(int32_t worldX, int32_t worldY, int32_t groundY) {
    (void)worldX;
    (void)worldY;
    (void)groundY;
    int32_t xm;
    int32_t ym;
    int32_t imx;
    int32_t imy;
    int32_t px;
    int32_t py;
    int32_t secByte;
    int32_t maskType;
    int32_t maskY;
    int32_t ystop;
    int32_t heroSec;
    int32_t ground;
    int32_t xbw;
    int32_t ym1;
    int32_t ym2;
    int32_t blitwide;
    int32_t sprWorldX;
    int32_t sprWorldY;
    int32_t tileWorldX;
    int32_t tileWorldY;
    int32_t localX;
    int32_t localY;
    int32_t shadowX;
    int32_t shadowY;
    int doMask;
    px = 0;
    goto L1;
  L1:;
    if (px <= 15) goto L2; else goto L4;
  L2:;
    py = 0;
    goto L5;
  L3:;
    px = (px + 1);
    goto L1;
  L4:;
    if (Assets_currentRegion < 0) goto L10; else goto L9;
  L5:;
    if (py <= 31) goto L6; else goto L8;
  L6:;
#line 262
    Render_bmask[px][py] = 1;
    goto L7;
  L7:;
    py = (py + 1);
    goto L5;
  L8:;
    goto L3;
  L9:;
    if (Assets_shadowPB == NULL) goto L12; else goto L11;
  L10:;
    return;
  L11:;
#line 269
    sprWorldX = worldX;
#line 270
    sprWorldY = worldY;
#line 271
    ground = groundY;
#line 273
    xbw = m2_div(sprWorldX, 16);
#line 274
    ym1 = m2_div(sprWorldY, 32);
#line 275
    blitwide = ((m2_div(((sprWorldX + 16) - 1), 16) - xbw) + 1);
#line 276
    ym2 = (m2_div(((sprWorldY + 32) - 1), 32) - ym1);
#line 278
    heroSec = Assets_GetSectorByte((sprWorldX + 8), (sprWorldY + 16));
    xm = 0;
    goto L13;
  L12:;
    return;
  L13:;
    if (xm <= (blitwide - 1)) goto L14; else goto L16;
  L14:;
    ym = 0;
    goto L17;
  L15:;
    xm = (xm + 1);
    goto L13;
  L16:;
    return;
  L17:;
    if (ym <= ym2) goto L18; else goto L20;
  L18:;
#line 282
    imx = (xbw + xm);
#line 283
    imy = (ym1 + ym);
#line 284
    ystop = (ground - ((imy * 32) - World_camY));
#line 286
    secByte = Assets_GetSectorByte((imx * 16), (imy * 32));
#line 287
    maskType = Assets_GetMaskType(secByte);
#line 289
    doMask = 1;
    if ((maskType == 0)) goto L23;
    if ((maskType == 1)) goto L24;
    if ((maskType == 2)) goto L25;
    if ((maskType == 3)) goto L26;
    if ((maskType == 4)) goto L27;
    if ((maskType == 5)) goto L28;
    if ((maskType == 6)) goto L29;
    if ((maskType == 7)) goto L30;
    goto L22;
  L19:;
    ym = (ym + 1);
    goto L17;
  L20:;
    goto L15;
  L21:;
    if (doMask) goto L44; else goto L43;
  L22:;
#line 301
    doMask = 0;
    goto L21;
  L23:;
#line 291
    doMask = 0;
    goto L21;
  L24:;
    if (xm == 0) goto L32; else goto L31;
  L25:;
    if (ystop > 35) goto L34; else goto L33;
  L26:;
    goto L21;
  L27:;
    if (xm == 0) goto L36; else goto L37;
  L28:;
    if (xm == 0) goto L40; else goto L38;
  L29:;
    goto L21;
  L30:;
    if (ystop > 20) goto L42; else goto L41;
  L31:;
    goto L21;
  L32:;
#line 292
    doMask = 0;
    goto L31;
  L33:;
    goto L21;
  L34:;
#line 293
    doMask = 0;
    goto L33;
  L35:;
    goto L21;
  L36:;
#line 296
    doMask = 0;
    goto L35;
  L37:;
    if (ystop > 35) goto L36; else goto L35;
  L38:;
    goto L21;
  L39:;
#line 297
    doMask = 0;
    goto L38;
  L40:;
    if (ystop > 35) goto L39; else goto L38;
  L41:;
    goto L21;
  L42:;
#line 299
    doMask = 0;
    goto L41;
  L43:;
    goto L19;
  L44:;
#line 306
    maskY = (Assets_GetMapTag(secByte) * 32);
#line 307
    tileWorldX = (imx * 16);
#line 308
    tileWorldY = (imy * 32);
    py = 0;
    goto L45;
  L45:;
    if (py <= (32 - 1)) goto L46; else goto L48;
  L46:;
    px = 0;
    goto L49;
  L47:;
    py = (py + 1);
    goto L45;
  L48:;
    goto L43;
  L49:;
    if (px <= (16 - 1)) goto L50; else goto L52;
  L50:;
#line 315
    localX = ((tileWorldX + px) - sprWorldX);
#line 316
    localY = ((tileWorldY + py) - sprWorldY);
    if (localX >= 0) goto L57; else goto L53;
  L51:;
    px = (px + 1);
    goto L49;
  L52:;
    goto L47;
  L53:;
    goto L51;
  L54:;
#line 320
    shadowX = px;
#line 321
    shadowY = (maskY + py);
    if (shadowY >= 0) goto L60; else goto L58;
  L55:;
    if (localY < 32) goto L54; else goto L53;
  L56:;
    if (localY >= 0) goto L55; else goto L53;
  L57:;
    if (localX < 16) goto L56; else goto L53;
  L58:;
    goto L53;
  L59:;
    if (PixBuf_GetPix(Assets_shadowPB, shadowX, shadowY) != 0) goto L62; else goto L61;
  L60:;
    if (shadowY < 6144) goto L59; else goto L58;
  L61:;
    goto L58;
  L62:;
#line 324
    Render_bmask[localX][localY] = 0;
    goto L61;
}

static void Render_BuildSpriteMaskFor(int32_t actorIdx) {
    (void)actorIdx;
#line 337
    Render_BuildSpriteMaskAt((Actor_actors[actorIdx].absX - 8), (Actor_actors[actorIdx].absY - 16), ((Actor_actors[actorIdx].absY - World_camY) + 16));
    return;
}

static void Render_DrawItems(void) {
    int32_t i;
    int32_t sx;
    int32_t sy;
    int32_t r;
    int32_t g;
    int32_t b;
#line 348
    Canvas_SetClip(Platform_ren, 0, 0, Render_S(320), Render_S(143));
    i = 0;
    goto L1;
  L1:;
    if (i <= (Items_itemCount - 1)) goto L2; else goto L4;
  L2:;
    if (Items_items[i].active) goto L6; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 369
    Canvas_ClearClip(Platform_ren);
    return;
  L5:;
    goto L3;
  L6:;
#line 352
    sx = ((Items_items[i].x - World_camX) * 3);
#line 353
    sy = ((Items_items[i].y - World_camY) * 3);
    if (sx > (-Render_S(16))) goto L11; else goto L7;
  L7:;
    goto L5;
  L8:;
#line 356
    Render_ItemColor(Items_items[i].itemId, &r, &g, &b);
    if (m2_mod(m2_div(GameState_cycle, 8), 2) == 0) goto L13; else goto L14;
  L9:;
    if (sy < (Render_S(143) + 16)) goto L8; else goto L7;
  L10:;
    if (sy > (-Render_S(16))) goto L9; else goto L7;
  L11:;
    if (sx < (Render_S(320) + 16)) goto L10; else goto L7;
  L12:;
#line 362
    Canvas_FillRect(Platform_ren, (sx - Render_S(3)), (sy - Render_S(3)), Render_S(6), Render_S(6));
#line 363
    Canvas_SetColor(Platform_ren, 255, 255, 255, 255);
#line 364
    Canvas_DrawRect(Platform_ren, (sx - Render_S(3)), (sy - Render_S(3)), Render_S(6), Render_S(6));
    goto L7;
  L13:;
#line 358
    Canvas_SetColor(Platform_ren, r, g, b, 255);
    goto L12;
  L14:;
#line 360
    Canvas_SetColor(Platform_ren, m2_div(r, 2), m2_div(g, 2), m2_div(b, 2), 255);
    goto L12;
}

static void Render_ItemColor(int32_t id, int32_t *r, int32_t *g, int32_t *b) {
    (void)id;
    (void)r;
    (void)g;
    (void)b;
    if ((id == 1)) goto L3;
    if ((id == 2)) goto L4;
    if ((id == 3)) goto L5;
    if ((id == 4)) goto L6;
    if ((id == 5)) goto L7;
    if ((id == 6)) goto L8;
    if ((id == 7)) goto L9;
    if ((id == 8)) goto L10;
    goto L2;
  L1:;
    return;
  L2:;
#line 384
    (*r) = 200;
    (*g) = 200;
    (*b) = 200;
    goto L1;
  L3:;
#line 375
    (*r) = 255;
    (*g) = 215;
    (*b) = 0;
    goto L1;
  L4:;
#line 376
    (*r) = 180;
    (*g) = 100;
    (*b) = 40;
    goto L1;
  L5:;
#line 377
    (*r) = 200;
    (*g) = 200;
    (*b) = 50;
    goto L1;
  L6:;
#line 378
    (*r) = 180;
    (*g) = 180;
    (*b) = 200;
    goto L1;
  L7:;
#line 379
    (*r) = 100;
    (*g) = 140;
    (*b) = 200;
    goto L1;
  L8:;
#line 380
    (*r) = 200;
    (*g) = 50;
    (*b) = 200;
    goto L1;
  L9:;
#line 381
    (*r) = 50;
    (*g) = 200;
    (*b) = 220;
    goto L1;
  L10:;
#line 382
    (*r) = 240;
    (*g) = 230;
    (*b) = 200;
    goto L1;
}

static int32_t Render_WalkBase(int32_t facing) {
    (void)facing;
    if ((facing == 0)) goto L3;
    if ((facing == 1)) goto L4;
    if ((facing == 2)) goto L5;
    if ((facing == 3)) goto L6;
    if ((facing == 4)) goto L7;
    if ((facing == 5)) goto L8;
    if ((facing == 6)) goto L9;
    if ((facing == 7)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 16;
  L4:;
    return 16;
  L5:;
    return 24;
  L6:;
    return 0;
  L7:;
    return 0;
  L8:;
    return 0;
  L9:;
    return 8;
  L10:;
    return 16;
}

static int32_t Render_FightBase(int32_t facing) {
    (void)facing;
    if ((facing == 0)) goto L3;
    if ((facing == 1)) goto L4;
    if ((facing == 2)) goto L5;
    if ((facing == 3)) goto L6;
    if ((facing == 4)) goto L7;
    if ((facing == 5)) goto L8;
    if ((facing == 6)) goto L9;
    if ((facing == 7)) goto L10;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 32;
  L3:;
    return 56;
  L4:;
    return 56;
  L5:;
    return 68;
  L6:;
    return 32;
  L7:;
    return 32;
  L8:;
    return 32;
  L9:;
    return 44;
  L10:;
    return 56;
}

static int32_t Render_GetPlayerFrame(int32_t i) {
    (void)i;
    int32_t base;
    int32_t inum;
    int32_t frame;
    if (Render_wpnInited) goto L1; else goto L2;
  L1:;
    if (Carrier_riding != 0) goto L5; else goto L3;
  L2:;
#line 429
    Render_InitWpnState();
    goto L1;
  L3:;
    if (Actor_actors[i].state == 12) goto L7; else goto L8;
  L4:;
    return Render_WalkBase(Actor_actors[i].facing);
  L5:;
    if (i == 0) goto L4; else goto L3;
  L6:;
    return 0;
  L7:;
    return (Render_WalkBase(Actor_actors[i].facing) + m2_mod((GameState_cycle + i), 8));
  L8:;
    if (Actor_actors[i].state == 0) goto L9; else goto L10;
  L9:;
#line 440
    base = Render_FightBase(Actor_actors[i].facing);
#line 441
    inum = (base + m2_mod(m2_div((GameState_cycle + i), 2), 12));
    if (inum > 86) goto L12; else goto L11;
  L10:;
    if (Actor_actors[i].state == 23) goto L13; else goto L14;
  L11:;
    return Render_wpnState[inum].figure;
  L12:;
#line 442
    inum = 86;
    goto L11;
  L13:;
    return Render_wpnState[86].figure;
  L14:;
    if (Actor_actors[i].state == 14) goto L15; else goto L17;
  L15:;
#line 447
    inum = 80;
#line 448
    frame = m2_mod(m2_div((GameState_cycle + i), 4), 3);
    if (Actor_actors[i].state == 15) goto L19; else goto L18;
  L16:;
    return (Render_WalkBase(Actor_actors[i].facing) + 1);
  L17:;
    if (Actor_actors[i].state == 15) goto L15; else goto L16;
  L18:;
    return Render_wpnState[(inum + frame)].figure;
  L19:;
#line 449
    frame = 2;
    goto L18;
}

static int32_t Render_GetEnemyFrame(int32_t i) {
    (void)i;
    int32_t base;
    int32_t frame;
    int32_t inum;
    int odd;
    if (Render_wpnInited) goto L1; else goto L2;
  L1:;
#line 461
    odd = ((uint32_t)(((uint32_t)(Actor_actors[i].race))) & (uint32_t)(1)) != 0;
    if (Actor_actors[i].state == 12) goto L4; else goto L5;
  L2:;
#line 460
    Render_InitWpnState();
    goto L1;
  L3:;
    if (Actor_actors[i].race == 4) goto L38; else goto L37;
  L4:;
#line 473
    base = Render_WalkBase(Actor_actors[i].facing);
#line 474
    inum = (base + m2_mod((GameState_cycle + i), 8));
    goto L3;
  L5:;
    if (Actor_actors[i].state == 0) goto L9; else goto L8;
  L6:;
    if ((Actor_actors[i].facing == 0) || (Actor_actors[i].facing == 1) || (Actor_actors[i].facing == 7)) goto L12;
    if ((Actor_actors[i].facing == 2)) goto L13;
    if ((Actor_actors[i].facing == 3) || (Actor_actors[i].facing == 4) || (Actor_actors[i].facing == 5)) goto L14;
    if ((Actor_actors[i].facing == 6)) goto L15;
    goto L11;
  L7:;
    if (Actor_actors[i].state == 14) goto L20; else goto L21;
  L8:;
    if (Actor_actors[i].state == 0) goto L6; else goto L7;
  L9:;
    if (Actor_actors[i].state < 12) goto L6; else goto L8;
  L10:;
#line 488
    frame = m2_mod(m2_div((GameState_cycle + i), 4), 6);
    if (frame > 5) goto L17; else goto L16;
  L11:;
#line 486
    base = 32;
    goto L10;
  L12:;
#line 482
    base = 56;
    goto L10;
  L13:;
#line 483
    base = 68;
    goto L10;
  L14:;
#line 484
    base = 32;
    goto L10;
  L15:;
#line 485
    base = 44;
    goto L10;
  L16:;
#line 490
    inum = (base + frame);
    if (inum > 86) goto L19; else goto L18;
  L17:;
#line 489
    frame = 5;
    goto L16;
  L18:;
#line 492
    inum = Render_wpnState[inum].figure;
    goto L3;
  L19:;
#line 491
    inum = base;
    goto L18;
  L20:;
    if (Actor_actors[i].tactic > 15) goto L23; else goto L24;
  L21:;
    if (Actor_actors[i].state == 15) goto L35; else goto L36;
  L22:;
    goto L3;
  L23:;
    if (Actor_actors[i].facing == 0) goto L26; else goto L28;
  L24:;
    if (Actor_actors[i].tactic > 0) goto L29; else goto L30;
  L25:;
    goto L22;
  L26:;
#line 496
    inum = Render_wpnState[80].figure;
    goto L25;
  L27:;
#line 498
    inum = Render_wpnState[81].figure;
    goto L25;
  L28:;
    if (Actor_actors[i].facing > 4) goto L26; else goto L27;
  L29:;
    if (Actor_actors[i].facing == 0) goto L32; else goto L34;
  L30:;
#line 507
    inum = Render_wpnState[82].figure;
    goto L22;
  L31:;
    goto L22;
  L32:;
#line 502
    inum = Render_wpnState[81].figure;
    goto L31;
  L33:;
#line 504
    inum = Render_wpnState[80].figure;
    goto L31;
  L34:;
    if (Actor_actors[i].facing > 4) goto L32; else goto L33;
  L35:;
#line 510
    inum = Render_wpnState[82].figure;
    goto L3;
  L36:;
#line 512
    inum = (Render_WalkBase(Actor_actors[i].facing) + 1);
    goto L3;
  L37:;
    if (Actor_actors[i].race == 8) goto L50; else goto L49;
  L38:;
    if ((Actor_actors[i].facing == 0) || (Actor_actors[i].facing == 1) || (Actor_actors[i].facing == 7)) goto L41;
    if ((Actor_actors[i].facing == 2) || (Actor_actors[i].facing == 3)) goto L42;
    if ((Actor_actors[i].facing == 4) || (Actor_actors[i].facing == 5)) goto L43;
    if ((Actor_actors[i].facing == 6)) goto L44;
    goto L40;
  L39:;
    if (Actor_actors[i].state == 14) goto L46; else goto L48;
  L40:;
#line 525
    base = 36;
    goto L39;
  L41:;
#line 521
    base = 52;
    goto L39;
  L42:;
#line 522
    base = 60;
    goto L39;
  L43:;
#line 523
    base = 36;
    goto L39;
  L44:;
#line 524
    base = 44;
    goto L39;
  L45:;
    return 0;
  L46:;
    return (base + 3);
  L47:;
    return (base + m2_mod(m2_div(GameState_cycle, 4), 2));
  L48:;
    if (Actor_actors[i].state == 15) goto L46; else goto L47;
  L49:;
    if (odd) goto L64; else goto L65;
  L50:;
    if (Actor_actors[i].state == 14) goto L52; else goto L53;
  L51:;
    return 0;
  L52:;
    return 63;
  L53:;
    if (Actor_actors[i].state == 15) goto L54; else goto L55;
  L54:;
    return 63;
  L55:;
#line 542
    frame = (m2_mod(GameState_cycle, 4) * 2);
    if (frame > 4) goto L57; else goto L56;
  L56:;
    if ((m2_mod(i, 3) == 0)) goto L60;
    if ((m2_mod(i, 3) == 1)) goto L61;
    if ((m2_mod(i, 3) == 2)) goto L62;
    goto L59;
  L57:;
#line 543
    (frame--);
    goto L56;
  L58:;
    return 0;
  L59:;
    return 37;
  L60:;
    return 37;
  L61:;
    return (40 + frame);
  L62:;
    return (48 + frame);
  L63:;
    return inum;
  L64:;
#line 555
    inum = ((uint32_t)(((int32_t)(((uint32_t)(inum))))) | (uint32_t)(1));
    goto L63;
  L65:;
#line 557
    inum = ((uint32_t)(((uint32_t)(inum))) & (uint32_t)(65534));
    goto L63;
}

static void Render_InitWpnState(void) {
#line 593
    Render_InitWpnState_W(0, 0, 11, (-2), 11);
    Render_InitWpnState_W(1, 1, 11, (-3), 11);
    Render_InitWpnState_W(2, 2, 11, (-3), 10);
    Render_InitWpnState_W(3, 3, 11, (-3), 9);
#line 594
    Render_InitWpnState_W(4, 4, 11, (-3), 10);
    Render_InitWpnState_W(5, 5, 11, (-3), 11);
    Render_InitWpnState_W(6, 6, 11, (-2), 11);
    Render_InitWpnState_W(7, 7, 11, (-1), 11);
#line 596
    Render_InitWpnState_W(8, 8, 9, (-12), 11);
    Render_InitWpnState_W(9, 9, 9, (-11), 12);
    Render_InitWpnState_W(10, 10, 9, (-8), 13);
    Render_InitWpnState_W(11, 11, 9, (-4), 13);
#line 597
    Render_InitWpnState_W(12, 12, 9, 0, 13);
    Render_InitWpnState_W(13, 13, 9, (-4), 13);
    Render_InitWpnState_W(14, 14, 9, (-8), 13);
    Render_InitWpnState_W(15, 15, 9, (-11), 12);
#line 599
    Render_InitWpnState_W(16, 16, 14, (-1), 1);
    Render_InitWpnState_W(17, 17, 14, (-1), 2);
    Render_InitWpnState_W(18, 18, 14, (-1), 3);
    Render_InitWpnState_W(19, 19, 14, (-1), 4);
#line 600
    Render_InitWpnState_W(20, 20, 14, (-1), 3);
    Render_InitWpnState_W(21, 21, 14, (-1), 2);
    Render_InitWpnState_W(22, 22, 14, (-1), 1);
    Render_InitWpnState_W(23, 23, 14, (-1), 1);
#line 602
    Render_InitWpnState_W(24, 24, 10, 5, 12);
    Render_InitWpnState_W(25, 25, 10, 3, 12);
    Render_InitWpnState_W(26, 26, 10, 2, 12);
    Render_InitWpnState_W(27, 27, 10, 3, 12);
#line 603
    Render_InitWpnState_W(28, 28, 10, 5, 12);
    Render_InitWpnState_W(29, 29, 10, 6, 12);
    Render_InitWpnState_W(30, 30, 10, 6, 11);
    Render_InitWpnState_W(31, 31, 10, 6, 12);
#line 605
    Render_InitWpnState_W(32, 32, 11, (-2), 12);
    Render_InitWpnState_W(33, 32, 10, 0, 12);
    Render_InitWpnState_W(34, 33, 0, 2, 10);
    Render_InitWpnState_W(35, 34, 1, 4, 6);
#line 606
    Render_InitWpnState_W(36, 34, 2, 1, 4);
    Render_InitWpnState_W(37, 34, 3, 0, 4);
    Render_InitWpnState_W(38, 36, 4, (-5), 0);
    Render_InitWpnState_W(39, 36, 5, (-10), 1);
#line 607
    Render_InitWpnState_W(40, 35, 12, (-5), 5);
    Render_InitWpnState_W(41, 36, 0, 0, 6);
    Render_InitWpnState_W(42, 38, 85, (-6), 5);
    Render_InitWpnState_W(43, 37, 81, (-6), 5);
#line 609
    Render_InitWpnState_W(44, 40, 9, (-7), 12);
    Render_InitWpnState_W(45, 40, 8, (-9), 9);
    Render_InitWpnState_W(46, 41, 7, (-10), 5);
    Render_InitWpnState_W(47, 42, 7, (-12), 4);
#line 610
    Render_InitWpnState_W(48, 42, 6, (-12), 3);
    Render_InitWpnState_W(49, 42, 5, (-12), 3);
    Render_InitWpnState_W(50, 44, 5, (-8), 3);
    Render_InitWpnState_W(51, 44, 14, (-7), 6);
#line 611
    Render_InitWpnState_W(52, 43, 13, (-7), 8);
    Render_InitWpnState_W(53, 42, 5, (-12), 3);
    Render_InitWpnState_W(54, 46, 86, (-3), 0);
    Render_InitWpnState_W(55, 45, 82, (-3), 0);
#line 613
    Render_InitWpnState_W(56, 48, 14, (-3), 0);
    Render_InitWpnState_W(57, 48, 6, (-3), (-1));
    Render_InitWpnState_W(58, 49, 5, (-2), (-3));
    Render_InitWpnState_W(59, 50, 5, (-3), (-4));
#line 614
    Render_InitWpnState_W(60, 50, 4, 0, 0);
    Render_InitWpnState_W(61, 50, 3, 3, 0);
    Render_InitWpnState_W(62, 52, 4, 6, 1);
    Render_InitWpnState_W(63, 52, 15, 7, 3);
#line 615
    Render_InitWpnState_W(64, 51, 14, 1, 6);
    Render_InitWpnState_W(65, 50, 4, 0, 0);
    Render_InitWpnState_W(66, 54, 87, 3, 0);
    Render_InitWpnState_W(67, 53, 83, 3, 0);
#line 617
    Render_InitWpnState_W(68, 56, 10, 5, 11);
    Render_InitWpnState_W(69, 56, 0, 6, 9);
    Render_InitWpnState_W(70, 57, 1, 10, 6);
    Render_InitWpnState_W(71, 58, 1, 10, 5);
#line 618
    Render_InitWpnState_W(72, 58, 2, 7, 3);
    Render_InitWpnState_W(73, 58, 3, 6, 3);
    Render_InitWpnState_W(74, 60, 4, 1, 0);
    Render_InitWpnState_W(75, 60, 3, 3, 2);
#line 619
    Render_InitWpnState_W(76, 59, 15, 4, 1);
    Render_InitWpnState_W(77, 58, 4, 5, 1);
    Render_InitWpnState_W(78, 62, 84, 3, 0);
    Render_InitWpnState_W(79, 61, 80, 3, 0);
#line 621
    Render_InitWpnState_W(80, 47, 0, 5, 11);
    Render_InitWpnState_W(81, 63, 0, 6, 9);
    Render_InitWpnState_W(82, 39, 0, 6, 9);
#line 623
    Render_InitWpnState_W(83, 55, 10, 5, 11);
    Render_InitWpnState_W(84, 64, 10, 5, 11);
    Render_InitWpnState_W(85, 65, 10, 5, 11);
    Render_InitWpnState_W(86, 66, 10, 5, 11);
#line 624
    Render_wpnInited = 1;
    return;
}

static void Render_InitBowOffsets(void) {
#line 635
    Render_bowX[0] = 1;
    Render_bowX[1] = 2;
    Render_bowX[2] = 3;
    Render_bowX[3] = 4;
#line 636
    Render_bowX[4] = 3;
    Render_bowX[5] = 2;
    Render_bowX[6] = 1;
    Render_bowX[7] = 0;
#line 638
    Render_bowX[8] = 3;
    Render_bowX[9] = 2;
    Render_bowX[10] = 0;
    Render_bowX[11] = (-2);
#line 639
    Render_bowX[12] = (-3);
    Render_bowX[13] = (-2);
    Render_bowX[14] = 0;
    Render_bowX[15] = 2;
#line 641
    Render_bowX[16] = (-3);
    Render_bowX[17] = (-3);
    Render_bowX[18] = (-3);
    Render_bowX[19] = (-3);
#line 642
    Render_bowX[20] = (-3);
    Render_bowX[21] = (-3);
    Render_bowX[22] = (-3);
    Render_bowX[23] = (-2);
#line 644
    Render_bowX[24] = 0;
    Render_bowX[25] = 1;
    Render_bowX[26] = 1;
    Render_bowX[27] = 1;
#line 645
    Render_bowX[28] = 0;
    Render_bowX[29] = (-2);
    Render_bowX[30] = (-3);
    Render_bowX[31] = (-2);
#line 647
    Render_bowY[0] = 8;
    Render_bowY[1] = 8;
    Render_bowY[2] = 8;
    Render_bowY[3] = 7;
#line 648
    Render_bowY[4] = 8;
    Render_bowY[5] = 8;
    Render_bowY[6] = 8;
    Render_bowY[7] = 8;
#line 649
    Render_bowY[8] = 11;
    Render_bowY[9] = 12;
    Render_bowY[10] = 13;
    Render_bowY[11] = 13;
#line 650
    Render_bowY[12] = 13;
    Render_bowY[13] = 13;
    Render_bowY[14] = 13;
    Render_bowY[15] = 12;
#line 651
    Render_bowY[16] = 8;
    Render_bowY[17] = 7;
    Render_bowY[18] = 6;
    Render_bowY[19] = 5;
#line 652
    Render_bowY[20] = 6;
    Render_bowY[21] = 7;
    Render_bowY[22] = 8;
    Render_bowY[23] = 9;
#line 653
    Render_bowY[24] = 12;
    Render_bowY[25] = 12;
    Render_bowY[26] = 12;
    Render_bowY[27] = 12;
#line 654
    Render_bowY[28] = 12;
    Render_bowY[29] = 12;
    Render_bowY[30] = 11;
    Render_bowY[31] = 12;
#line 655
    Render_bowInited = 1;
    return;
}

static void Render_DrawWeaponOverlay(int32_t i, int32_t sx, int32_t sy, int32_t frame) {
    (void)i;
    (void)sx;
    (void)sy;
    (void)frame;
    int32_t weapon;
    int32_t wpnFrame;
    int32_t objFrame;
    int32_t ox;
    int32_t oy;
    int32_t f;
    int32_t dirGroup;
    int32_t srcY;
    int32_t px;
    int32_t py;
    int32_t dx;
    int32_t dy;
    int32_t bx;
    int32_t by;
    if (WorldObj_objTex == NULL) goto L2; else goto L1;
  L1:;
#line 663
    weapon = Actor_actors[i].weapon;
    if (weapon <= 0) goto L4; else goto L5;
  L2:;
    return;
  L3:;
    if (Actor_actors[i].state == 15) goto L7; else goto L8;
  L4:;
    return;
  L5:;
    if (weapon > 5) goto L4; else goto L3;
  L6:;
    if (Actor_actors[i].environ > 4) goto L10; else goto L9;
  L7:;
    return;
  L8:;
    if (Actor_actors[i].state == 14) goto L7; else goto L6;
  L9:;
    if (Render_wpnInited) goto L11; else goto L12;
  L10:;
    return;
  L11:;
    if (Render_bowInited) goto L13; else goto L14;
  L12:;
#line 667
    Render_InitWpnState();
    goto L11;
  L13:;
    if (frame < 0) goto L16; else goto L17;
  L14:;
#line 668
    Render_InitBowOffsets();
    goto L13;
  L15:;
#line 672
    f = Actor_actors[i].facing;
    if (f == 0) goto L19; else goto L21;
  L16:;
    return;
  L17:;
    if (frame > 86) goto L16; else goto L15;
  L18:;
    if (weapon == 4) goto L25; else goto L24;
  L19:;
    return;
  L20:;
    if (f == 7) goto L19; else goto L18;
  L21:;
    if (f == 1) goto L19; else goto L20;
  L22:;
    if (objFrame < 0) goto L43; else goto L44;
  L23:;
#line 677
    ox = Render_bowX[frame];
#line 678
    oy = Render_bowY[frame];
#line 681
    dirGroup = m2_div(frame, 8);
    if (((uint32_t)(((uint32_t)(dirGroup))) & (uint32_t)(1)) != 0) goto L27; else goto L28;
  L24:;
    if (weapon == 5) goto L31; else goto L32;
  L25:;
    if (frame < 32) goto L23; else goto L24;
  L26:;
    goto L22;
  L27:;
#line 683
    objFrame = 30;
    goto L26;
  L28:;
    if (((uint32_t)(((uint32_t)(dirGroup))) & (uint32_t)(2)) != 0) goto L29; else goto L30;
  L29:;
#line 685
    objFrame = 83;
    goto L26;
  L30:;
#line 687
    objFrame = 81;
    goto L26;
  L31:;
#line 691
    ox = Render_wpnState[frame].wpnX;
#line 692
    oy = Render_wpnState[frame].wpnY;
#line 693
    objFrame = (Actor_actors[i].facing + 103);
    if (Actor_actors[i].facing == 2) goto L34; else goto L33;
  L32:;
    if (weapon == 4) goto L35; else goto L36;
  L33:;
    goto L22;
  L34:;
#line 694
    (oy -= 6);
    goto L33;
  L35:;
    return;
  L36:;
#line 701
    ox = Render_wpnState[frame].wpnX;
#line 702
    oy = Render_wpnState[frame].wpnY;
#line 703
    wpnFrame = Render_wpnState[frame].wpnNo;
    if ((weapon == 1)) goto L39;
    if ((weapon == 2)) goto L40;
    if ((weapon == 3)) goto L41;
    goto L38;
  L37:;
    goto L22;
  L38:;
#line 709
    objFrame = wpnFrame;
    goto L37;
  L39:;
#line 705
    objFrame = (wpnFrame + 64);
    goto L37;
  L40:;
#line 706
    objFrame = (wpnFrame + 32);
    goto L37;
  L41:;
#line 707
    objFrame = (wpnFrame + 48);
    goto L37;
  L42:;
#line 715
    srcY = (objFrame * 16);
#line 716
    Texture_SetColorMod(WorldObj_objTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 720
    dx = ((sx - Render_S(8)) + Render_S(ox));
#line 721
    dy = ((sy - Render_S(16)) + Render_S(oy));
    if (i == 0) goto L46; else goto L47;
  L43:;
    return;
  L44:;
    if (objFrame >= 116) goto L43; else goto L42;
  L45:;
    return;
  L46:;
#line 727
    Render_BuildSpriteMaskAt(((Actor_actors[0].absX - 8) + ox), ((Actor_actors[0].absY - 16) + oy), ((Actor_actors[0].absY - World_camY) + 16));
    py = 0;
    goto L48;
  L47:;
#line 749
    Render_BuildSpriteMaskAt(((Actor_actors[i].absX - 8) + ox), ((Actor_actors[i].absY - 16) + oy), ((Actor_actors[i].absY - World_camY) + 16));
    py = 0;
    goto L62;
  L48:;
    if (py <= (16 - 1)) goto L49; else goto L51;
  L49:;
    px = 0;
    goto L52;
  L50:;
    py = (py + 1);
    goto L48;
  L51:;
    goto L45;
  L52:;
    if (px <= (16 - 1)) goto L53; else goto L55;
  L53:;
    if (px < 16) goto L59; else goto L58;
  L54:;
    px = (px + 1);
    goto L52;
  L55:;
    goto L50;
  L56:;
    goto L54;
  L57:;
    if (Render_bmask[px][py]) goto L61; else goto L60;
  L58:;
#line 741
    Platform_DrawTexRegion(WorldObj_objTex, px, (srcY + py), 1, 1, (dx + (px * 3)), (dy + (py * 3)), 3, 3);
    goto L56;
  L59:;
    if (py < 32) goto L57; else goto L58;
  L60:;
    goto L56;
  L61:;
#line 736
    Platform_DrawTexRegion(WorldObj_objTex, px, (srcY + py), 1, 1, (dx + (px * 3)), (dy + (py * 3)), 3, 3);
    goto L60;
  L62:;
    if (py <= (16 - 1)) goto L63; else goto L65;
  L63:;
    px = 0;
    goto L66;
  L64:;
    py = (py + 1);
    goto L62;
  L65:;
    goto L45;
  L66:;
    if (px <= (16 - 1)) goto L67; else goto L69;
  L67:;
    if (px < 16) goto L73; else goto L72;
  L68:;
    px = (px + 1);
    goto L66;
  L69:;
    goto L64;
  L70:;
    goto L68;
  L71:;
    if (Render_bmask[px][py]) goto L75; else goto L74;
  L72:;
#line 762
    Platform_DrawTexRegion(WorldObj_objTex, px, (srcY + py), 1, 1, (dx + (px * 3)), (dy + (py * 3)), 3, 3);
    goto L70;
  L73:;
    if (py < 32) goto L71; else goto L72;
  L74:;
    goto L70;
  L75:;
#line 757
    Platform_DrawTexRegion(WorldObj_objTex, px, (srcY + py), 1, 1, (dx + (px * 3)), (dy + (py * 3)), 3, 3);
    goto L74;
}

static void Render_DrawBrotherSprite(int32_t actorIdx, int32_t brotherIdx, int32_t frame, int32_t sx, int32_t sy, int32_t env) {
    (void)actorIdx;
    (void)brotherIdx;
    (void)frame;
    (void)sx;
    (void)sy;
    (void)env;
    void * tex;
    int32_t srcY;
    int32_t srcH;
    int32_t dstY;
    int32_t dstH;
    int32_t clipBot;
    int32_t px;
    int32_t py;
    if (brotherIdx < 0) goto L2; else goto L3;
  L1:;
#line 776
    tex = Assets_brotherTex[brotherIdx];
    if (tex == NULL) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (brotherIdx > 2) goto L2; else goto L1;
  L4:;
    if (frame < 0) goto L7; else goto L8;
  L5:;
    return;
  L6:;
#line 780
    srcY = (frame * 32);
#line 781
    srcH = 32;
#line 782
    dstY = (sy - Render_S(16));
#line 783
    dstH = Render_S(32);
#line 784
    clipBot = 0;
    if (env > 29) goto L10; else goto L9;
  L7:;
#line 778
    frame = 0;
    goto L6;
  L8:;
    if (frame > 66) goto L7; else goto L6;
  L9:;
    if (env > 19) goto L14; else goto L13;
  L10:;
    if (WorldObj_objTex != NULL) goto L12; else goto L11;
  L11:;
    return;
  L12:;
#line 795
    srcY = ((97 + m2_mod(GameState_cycle, 2)) * 16);
#line 796
    Texture_SetColorMod(WorldObj_objTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 797
    Platform_DrawTexRegion(WorldObj_objTex, 0, srcY, 16, 8, (sx - Render_S(8)), (sy - Render_S(4)), Render_S(16), Render_S(8));
    goto L11;
  L13:;
    if (env == 2) goto L16; else goto L17;
  L14:;
#line 805
    Texture_SetColorMod(tex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 806
    srcY = (56 * 32);
#line 807
    Platform_DrawTexRegion(tex, 0, srcY, 16, 12, (sx - Render_S(8)), ((sy - Render_S(16)) + Render_S((env - 8))), Render_S(16), Render_S(12));
    return;
  L15:;
    if (clipBot > 0) goto L22; else goto L21;
  L16:;
#line 812
    clipBot = 10;
    goto L15;
  L17:;
    if (env > 2) goto L18; else goto L15;
  L18:;
#line 814
    clipBot = env;
    if (clipBot > (32 - 4)) goto L20; else goto L19;
  L19:;
    goto L15;
  L20:;
#line 815
    clipBot = (32 - 4);
    goto L19;
  L21:;
    if (srcH <= 0) goto L24; else goto L23;
  L22:;
#line 819
    (srcH -= clipBot);
#line 820
    (dstH -= Render_S(clipBot));
    goto L21;
  L23:;
#line 826
    Texture_SetColorMod(tex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 829
    Render_BuildSpriteMaskFor(actorIdx);
    py = 0;
    goto L25;
  L24:;
    return;
  L25:;
    if (py <= (srcH - 1)) goto L26; else goto L28;
  L26:;
    px = 0;
    goto L29;
  L27:;
    py = (py + 1);
    goto L25;
  L28:;
    return;
  L29:;
    if (px <= (16 - 1)) goto L30; else goto L32;
  L30:;
    if (Render_bmask[px][py]) goto L34; else goto L33;
  L31:;
    px = (px + 1);
    goto L29;
  L32:;
    goto L27;
  L33:;
    goto L31;
  L34:;
#line 836
    Platform_DrawTexRegion(tex, px, (srcY + py), 1, 1, ((sx - Render_S(8)) + (px * 3)), (dstY + (py * 3)), 3, 3);
    goto L33;
}

static int32_t Render_RaceToTexIdx(int32_t race) {
    (void)race;
    if ((race == 0) || (race == 1)) goto L3;
    if ((race == 2) || (race == 3) || (race == 5)) goto L4;
    if ((race == 4) || (race == 6) || (race == 7)) goto L5;
    if ((race == 8) || (race == 9) || (race == 10)) goto L6;
    goto L2;
  L1:;
    return 0;
  L2:;
    return 0;
  L3:;
    return 0;
  L4:;
    return 1;
  L5:;
    return 2;
  L6:;
    return 3;
}

static void Render_DrawEnemySprite(int32_t actorIdx, int32_t texIdx, int32_t frame, int32_t sx, int32_t sy) {
    (void)actorIdx;
    (void)texIdx;
    (void)frame;
    (void)sx;
    (void)sy;
    void * tex;
    int32_t srcY;
    int32_t srcH;
    int32_t px;
    int32_t py;
    int32_t dstY;
    int32_t dstH;
    int32_t clipBot;
    int32_t env;
    if (texIdx < 0) goto L2; else goto L3;
  L1:;
#line 863
    tex = Assets_enemyTex[texIdx];
    if (tex == NULL) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (texIdx > 4) goto L2; else goto L1;
  L4:;
#line 865
    srcY = (frame * 32);
#line 866
    srcH = 32;
#line 867
    dstY = (sy - Render_S(16));
#line 868
    dstH = Render_S(32);
#line 870
    Texture_SetColorMod(tex, Render_fadeR, Render_fadeG, Render_fadeB);
    if (Actor_actors[actorIdx].race == 2) goto L7; else goto L6;
  L5:;
    return;
  L6:;
#line 880
    env = Actor_actors[actorIdx].environ;
#line 881
    clipBot = 0;
    if (env == 2) goto L9; else goto L10;
  L7:;
#line 874
    Platform_DrawTexRegion(tex, 0, srcY, 16, 32, (sx - Render_S(8)), dstY, Render_S(16), Render_S(32));
    return;
  L8:;
    if (clipBot > 0) goto L15; else goto L14;
  L9:;
#line 883
    clipBot = 10;
    goto L8;
  L10:;
    if (env > 2) goto L11; else goto L8;
  L11:;
#line 885
    clipBot = env;
    if (clipBot > (32 - 4)) goto L13; else goto L12;
  L12:;
    goto L8;
  L13:;
#line 886
    clipBot = (32 - 4);
    goto L12;
  L14:;
    if (srcH <= 0) goto L17; else goto L16;
  L15:;
#line 889
    (srcH -= clipBot);
#line 890
    (dstH -= Render_S(clipBot));
    goto L14;
  L16:;
#line 895
    Render_BuildSpriteMaskFor(actorIdx);
    py = 0;
    goto L18;
  L17:;
    return;
  L18:;
    if (py <= (srcH - 1)) goto L19; else goto L21;
  L19:;
    px = 0;
    goto L22;
  L20:;
    py = (py + 1);
    goto L18;
  L21:;
    return;
  L22:;
    if (px <= (16 - 1)) goto L23; else goto L25;
  L23:;
    if (Render_bmask[px][py]) goto L27; else goto L26;
  L24:;
    px = (px + 1);
    goto L22;
  L25:;
    goto L20;
  L26:;
    goto L24;
  L27:;
#line 899
    Platform_DrawTexRegion(tex, px, (srcY + py), 1, 1, ((sx - Render_S(8)) + (px * 3)), (dstY + (py * 3)), 3, 3);
    goto L26;
}

static int32_t Render_GetStateIdx(int32_t i) {
    (void)i;
    int32_t base;
    if (Actor_actors[i].state == 12) goto L2; else goto L3;
  L1:;
    return 0;
  L2:;
    return (Render_WalkBase(Actor_actors[i].facing) + m2_mod((GameState_cycle + i), 8));
  L3:;
    if (Actor_actors[i].state == 0) goto L4; else goto L5;
  L4:;
#line 914
    base = Render_FightBase(Actor_actors[i].facing);
    return (base + m2_mod(m2_div((GameState_cycle + i), 2), 12));
  L5:;
    if (Actor_actors[i].state == 14) goto L6; else goto L8;
  L6:;
    return 80;
  L7:;
    return (Render_WalkBase(Actor_actors[i].facing) + 1);
  L8:;
    if (Actor_actors[i].state == 15) goto L6; else goto L7;
}

static void Render_DrawActorBody(int32_t i, int32_t sx, int32_t sy) {
    (void)i;
    (void)sx;
    (void)sy;
    int32_t frame;
    int32_t texIdx;
    int32_t stateIdx;
    int32_t mx;
    int32_t my;
    int32_t npcBank;
    int32_t npcFrame;
    if (i == 0) goto L3; else goto L1;
  L1:;
    if (i == 0) goto L28; else goto L26;
  L2:;
    if (Assets_enemyTex[3] != NULL) goto L5; else goto L4;
  L3:;
    if (Actor_actors[i].state == 22) goto L2; else goto L1;
  L4:;
    return;
  L5:;
    if ((Brothers_activeBrother == 0)) goto L8;
    if ((Brothers_activeBrother == 1)) goto L9;
    if ((Brothers_activeBrother == 2)) goto L10;
    goto L7;
  L6:;
#line 943
    Texture_SetColorMod(Assets_enemyTex[3], Render_fadeR, Render_fadeG, Render_fadeB);
#line 944
    Platform_DrawTexRegion(Assets_enemyTex[3], 0, (frame * 32), 16, 32, (sx - Render_S(8)), (sy - Render_S(16)), Render_S(16), Render_S(32));
    goto L4;
  L7:;
#line 941
    frame = 30;
    goto L6;
  L8:;
    if ((m2_div(Actor_actors[i].tactic, 10) == 0)) goto L13;
    if ((m2_div(Actor_actors[i].tactic, 10) == 1)) goto L14;
    if ((m2_div(Actor_actors[i].tactic, 10) == 2)) goto L15;
    goto L12;
  L9:;
    if ((m2_div(Actor_actors[i].tactic, 10) == 0)) goto L18;
    if ((m2_div(Actor_actors[i].tactic, 10) == 1)) goto L19;
    if ((m2_div(Actor_actors[i].tactic, 10) == 2)) goto L20;
    goto L17;
  L10:;
    if ((m2_div(Actor_actors[i].tactic, 10) == 0)) goto L23;
    if ((m2_div(Actor_actors[i].tactic, 10) == 1)) goto L24;
    if ((m2_div(Actor_actors[i].tactic, 10) == 2)) goto L25;
    goto L22;
  L11:;
    goto L6;
  L12:;
#line 934
    frame = 58;
    goto L11;
  L13:;
#line 933
    frame = 32;
    goto L11;
  L14:;
    frame = 34;
    goto L11;
  L15:;
    frame = 58;
    goto L11;
  L16:;
    goto L6;
  L17:;
#line 937
    frame = 60;
    goto L16;
  L18:;
#line 936
    frame = 36;
    goto L16;
  L19:;
    frame = 39;
    goto L16;
  L20:;
    frame = 60;
    goto L16;
  L21:;
    goto L6;
  L22:;
#line 940
    frame = 61;
    goto L21;
  L23:;
#line 939
    frame = 55;
    goto L21;
  L24:;
    frame = 56;
    goto L21;
  L25:;
    frame = 61;
    goto L21;
  L26:;
    if (Actor_actors[i].actorType == 2) goto L37; else goto L36;
  L27:;
#line 951
    frame = Render_GetPlayerFrame(i);
#line 952
    stateIdx = Render_GetStateIdx(i);
    if (Carrier_riding == 11) goto L30; else goto L31;
  L28:;
    if (Assets_brotherTex[Brothers_activeBrother] != NULL) goto L27; else goto L26;
  L29:;
    if (Carrier_riding == 0) goto L35; else goto L34;
  L30:;
#line 959
    Render_DrawBrotherSprite(i, Brothers_activeBrother, frame, sx, sy, 16);
    goto L29;
  L31:;
    if (Carrier_riding != 0) goto L32; else goto L33;
  L32:;
#line 961
    Render_DrawBrotherSprite(i, Brothers_activeBrother, frame, sx, (sy - Render_S(10)), Actor_actors[i].environ);
    goto L29;
  L33:;
#line 963
    Render_DrawBrotherSprite(i, Brothers_activeBrother, frame, sx, sy, Actor_actors[i].environ);
    goto L29;
  L34:;
    return;
  L35:;
#line 966
    Render_DrawWeaponOverlay(i, sx, sy, stateIdx);
    goto L34;
  L36:;
    if (Actor_actors[i].actorType == 4) goto L41; else goto L40;
  L37:;
#line 973
    texIdx = Render_RaceToTexIdx(Actor_actors[i].race);
    if (Assets_enemyTex[texIdx] != NULL) goto L39; else goto L38;
  L38:;
    goto L36;
  L39:;
#line 975
    frame = Render_GetEnemyFrame(i);
#line 976
    stateIdx = Render_GetStateIdx(i);
#line 977
    Render_DrawEnemySprite(i, texIdx, frame, sx, sy);
#line 978
    Render_DrawWeaponOverlay(i, sx, sy, stateIdx);
    return;
  L40:;
    if (Actor_actors[i].actorType == 6) goto L80; else goto L79;
  L41:;
    if (Actor_actors[i].state == 15) goto L43; else goto L42;
  L42:;
    if (Actor_actors[i].race == 14) goto L45; else goto L44;
  L43:;
    return;
  L44:;
#line 996
    NPC_GetSetfigSprite(Actor_actors[i].race, &npcBank, &npcFrame);
    if (Actor_actors[i].race == 9) goto L49; else goto L48;
  L45:;
#line 991
    frame = Render_GetEnemyFrame(i);
    if (((uint32_t)(((uint32_t)(frame))) & (uint32_t)(1)) == 0) goto L47; else goto L46;
  L46:;
#line 993
    Render_DrawEnemySprite(i, 1, frame, sx, sy);
    return;
  L47:;
#line 992
    (frame++);
    goto L46;
  L48:;
    if (npcBank >= 0) goto L56; else goto L53;
  L49:;
    if (Actor_actors[i].state == 14) goto L51; else goto L52;
  L50:;
    goto L48;
  L51:;
#line 1001
    npcFrame = 7;
    goto L50;
  L52:;
#line 1003
    npcFrame = m2_div(Actor_actors[i].facing, 2);
    goto L50;
  L53:;
    goto L40;
  L54:;
#line 1007
    Texture_SetColorMod(Assets_npcTex[npcBank], Render_fadeR, Render_fadeG, Render_fadeB);
    if (Actor_actors[i].race == 5) goto L58; else goto L60;
  L55:;
    if (Assets_npcTex[npcBank] != NULL) goto L54; else goto L53;
  L56:;
    if (npcBank <= 5) goto L55; else goto L53;
  L57:;
    my = 0;
    goto L69;
  L58:;
    my = 0;
    goto L61;
  L59:;
#line 1014
    Render_BuildSpriteMaskFor(i);
    goto L57;
  L60:;
    if (Actor_actors[i].race == 7) goto L58; else goto L59;
  L61:;
    if (my <= (32 - 1)) goto L62; else goto L64;
  L62:;
    mx = 0;
    goto L65;
  L63:;
    my = (my + 1);
    goto L61;
  L64:;
    goto L57;
  L65:;
    if (mx <= (16 - 1)) goto L66; else goto L68;
  L66:;
#line 1011
    Render_bmask[mx][my] = 1;
    goto L67;
  L67:;
    mx = (mx + 1);
    goto L65;
  L68:;
    goto L63;
  L69:;
    if (my <= (32 - 1)) goto L70; else goto L72;
  L70:;
    mx = 0;
    goto L73;
  L71:;
    my = (my + 1);
    goto L69;
  L72:;
    return;
  L73:;
    if (mx <= (16 - 1)) goto L74; else goto L76;
  L74:;
    if (Render_bmask[mx][my]) goto L78; else goto L77;
  L75:;
    mx = (mx + 1);
    goto L73;
  L76:;
    goto L71;
  L77:;
    goto L75;
  L78:;
#line 1019
    Platform_DrawTexRegion(Assets_npcTex[npcBank], mx, ((npcFrame * 32) + my), 1, 1, ((sx - Render_S(8)) + (mx * 3)), ((sy - Render_S(16)) + (my * 3)), 3, 3);
    goto L77;
  L79:;
    if (Actor_actors[i].actorType == 3) goto L89; else goto L88;
  L80:;
    if (Assets_dragonTex != NULL) goto L82; else goto L81;
  L81:;
    return;
  L82:;
    if (Actor_actors[i].state == 15) goto L84; else goto L85;
  L83:;
#line 1038
    Texture_SetColorMod(Assets_dragonTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 1039
    Platform_DrawTexRegion(Assets_dragonTex, 0, (frame * 40), 48, 40, (sx - Render_S(24)), (sy - Render_S(20)), Render_S(48), Render_S(40));
    goto L81;
  L84:;
#line 1034
    frame = 4;
    goto L83;
  L85:;
    if (Actor_actors[i].state == 14) goto L86; else goto L87;
  L86:;
#line 1035
    frame = 3;
    goto L83;
  L87:;
#line 1036
    frame = m2_mod(m2_div(GameState_cycle, 8), 3);
    goto L83;
  L88:;
    if (Actor_actors[i].actorType == 5) goto L93; else goto L92;
  L89:;
    if (Render_raftTex != NULL) goto L91; else goto L90;
  L90:;
    return;
  L91:;
#line 1048
    Texture_SetColorMod(Render_raftTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 1049
    Platform_DrawTexRegion(Render_raftTex, 0, 0, 32, 32, (sx - Render_S(16)), (sy - Render_S(16)), Render_S(32), Render_S(32));
    goto L90;
  L92:;
    if (Actor_actors[i].actorType == 2) goto L108; else goto L109;
  L93:;
    if (Actor_actors[i].race == 5) goto L95; else goto L96;
  L94:;
    return;
  L95:;
    if (Render_turtleTex != NULL) goto L98; else goto L97;
  L96:;
    if (Actor_actors[i].race == 11) goto L99; else goto L94;
  L97:;
    goto L94;
  L98:;
#line 1063
    frame = ((m2_mod((Actor_actors[i].facing + 1), 8) * 2) + m2_mod(m2_div(GameState_cycle, 4), 2));
#line 1064
    Texture_SetColorMod(Render_turtleTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 1065
    Platform_DrawTexRegion(Render_turtleTex, 0, (frame * 32), 32, 32, (sx - Render_S(16)), (sy - Render_S(16)), Render_S(32), Render_S(32));
    goto L97;
  L99:;
    if (Carrier_riding == 11) goto L101; else goto L102;
  L100:;
    goto L94;
  L101:;
    if (Render_birdTex != NULL) goto L104; else goto L103;
  L102:;
    if (Render_raftTex != NULL) goto L106; else goto L105;
  L103:;
    goto L100;
  L104:;
#line 1074
    frame = m2_mod((Actor_actors[i].facing + 1), 8);
#line 1075
    Texture_SetColorMod(Render_birdTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 1076
    Platform_DrawTexRegion(Render_birdTex, 0, (frame * 64), 64, 64, (sx - Render_S(32)), (sy - Render_S(32)), Render_S(64), Render_S(64));
    goto L103;
  L105:;
    goto L100;
  L106:;
#line 1082
    Texture_SetColorMod(Render_raftTex, Render_fadeR, Render_fadeG, Render_fadeB);
#line 1083
    Platform_DrawTexRegion(Render_raftTex, 0, 32, 32, 32, (sx - Render_S(16)), (sy - Render_S(16)), Render_S(32), Render_S(32));
    goto L105;
  L107:;
#line 1097
    Canvas_FillRect(Platform_ren, (sx - Render_S(4)), (sy - Render_S(6)), Render_S(8), Render_S(12));
#line 1098
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 1099
    Canvas_DrawRect(Platform_ren, (sx - Render_S(4)), (sy - Render_S(6)), Render_S(8), Render_S(12));
    return;
  L108:;
#line 1093
    Canvas_SetColor(Platform_ren, 200, 40, 40, 255);
    goto L107;
  L109:;
#line 1095
    Canvas_SetColor(Platform_ren, 60, 160, 220, 255);
    goto L107;
}

static void Render_DrawActors(void) {
    int32_t i;
    int32_t j;
    int32_t sx;
    int32_t sy;
    int32_t n;
    int32_t tmp;
    int32_t order[47 + 1];
#line 1106
    Canvas_SetClip(Platform_ren, 0, 0, Render_S(320), Render_S(143));
#line 1112
    n = 0;
    i = 0;
    goto L1;
  L1:;
    if (i <= (Actor_actorCount - 1)) goto L2; else goto L4;
  L2:;
#line 1114
    order[n] = i;
#line 1115
    j = n;
#line 1116
    sy = Actor_actors[i].absY;
    if (i == 0) goto L7; else goto L5;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
    j = 0;
    goto L16;
  L5:;
    goto L8;
  L6:;
#line 1117
    (sy++);
    goto L5;
  L7:;
    if (Carrier_riding != 0) goto L6; else goto L5;
  L8:;
    if (j > 0) goto L9; else goto L10;
  L9:;
#line 1119
    tmp = Actor_actors[order[(j - 1)]].absY;
    if (order[(j - 1)] == 0) goto L13; else goto L11;
  L10:;
#line 1125
    order[j] = i;
#line 1126
    (n++);
    goto L3;
  L11:;
    if (tmp <= sy) goto L15; else goto L14;
  L12:;
#line 1120
    (tmp++);
    goto L11;
  L13:;
    if (Carrier_riding != 0) goto L12; else goto L11;
  L14:;
#line 1122
    order[j] = order[(j - 1)];
#line 1123
    (j--);
    goto L8;
  L15:;
    goto L10;
  L16:;
    if (j <= (n - 1)) goto L17; else goto L19;
  L17:;
#line 1130
    i = order[j];
#line 1131
    sx = ((Actor_actors[i].absX - World_camX) * 3);
#line 1132
    sy = ((Actor_actors[i].absY - World_camY) * 3);
    if (sx > (-Render_S(20))) goto L24; else goto L20;
  L18:;
    j = (j + 1);
    goto L16;
  L19:;
#line 1139
    Canvas_ClearClip(Platform_ren);
    return;
  L20:;
    goto L18;
  L21:;
#line 1135
    Render_DrawActorBody(i, sx, sy);
    goto L20;
  L22:;
    if (sy < (Render_S(143) + 20)) goto L21; else goto L20;
  L23:;
    if (sy > (-Render_S(20))) goto L22; else goto L20;
  L24:;
    if (sx < (Render_S(320) + 20)) goto L23; else goto L20;
}

static void Render_DrawHUD(void) {
    if (Assets_hudTex != NULL) goto L2; else goto L3;
  L1:;
    return;
  L2:;
#line 1150
    Texture_Draw(Platform_ren, Assets_hudTex, 0, Render_S(143));
    goto L1;
  L3:;
#line 1152
    Canvas_SetColor(Platform_ren, 30, 25, 20, 255);
#line 1153
    Canvas_FillRect(Platform_ren, 0, Render_S(143), Render_S(320), Render_S(57));
    goto L1;
}

static void Render_DrawCompass(void) {
    int32_t dir;
    int32_t cx;
    int32_t cy;
    int32_t dx;
    int32_t dy;
    int32_t sz;
    int32_t ox[7 + 1];
    int32_t oy[7 + 1];
#line 1163
    dir = Actor_actors[0].facing;
    if (dir < 0) goto L2; else goto L3;
  L1:;
#line 1168
    cx = m2_div(((591 * 320) * 3), 640);
#line 1169
    cy = ((143 * 3) + m2_div(((27 * 57) * 3), 57));
#line 1170
    sz = m2_div(((5 * 320) * 3), 640);
    if (sz < 3) goto L5; else goto L4;
  L2:;
    return;
  L3:;
    if (dir > 7) goto L2; else goto L1;
  L4:;
#line 1174
    dx = m2_div(((11 * 320) * 3), 640);
#line 1175
    dy = m2_div(((11 * 57) * 3), 57);
#line 1177
    ox[0] = 0;
    oy[0] = (-dy);
#line 1178
    ox[1] = dx;
    oy[1] = (-dy);
#line 1179
    ox[2] = dx;
    oy[2] = 0;
#line 1180
    ox[3] = dx;
    oy[3] = dy;
#line 1181
    ox[4] = 0;
    oy[4] = dy;
#line 1182
    ox[5] = (-dx);
    oy[5] = dy;
#line 1183
    ox[6] = (-dx);
    oy[6] = 0;
#line 1184
    ox[7] = (-dx);
    oy[7] = (-dy);
#line 1187
    Canvas_SetColor(Platform_ren, 0, 176, 0, 255);
#line 1188
    Canvas_FillRect(Platform_ren, ((cx + ox[dir]) - sz), ((cy + oy[dir]) - m2_div(sz, 2)), (sz * 2), sz);
#line 1189
    Canvas_SetColor(Platform_ren, 0, 220, 0, 255);
#line 1190
    Canvas_FillRect(Platform_ren, (((cx + ox[dir]) - sz) + 1), (((cy + oy[dir]) - m2_div(sz, 2)) + 1), ((sz * 2) - 2), (sz - 2));
    return;
  L5:;
#line 1171
    sz = 3;
    goto L4;
}

static void Render_PalColor(int32_t idx, int32_t *r, int32_t *g, int32_t *b) {
    (void)idx;
    (void)r;
    (void)g;
    (void)b;
    if ((idx == 0)) goto L3;
    if ((idx == 1)) goto L4;
    if ((idx == 2)) goto L5;
    if ((idx == 3)) goto L6;
    if ((idx == 4)) goto L7;
    if ((idx == 5)) goto L8;
    if ((idx == 6)) goto L9;
    if ((idx == 7)) goto L10;
    if ((idx == 8)) goto L11;
    if ((idx == 9)) goto L12;
    if ((idx == 10)) goto L13;
    if ((idx == 11)) goto L14;
    if ((idx == 12)) goto L15;
    if ((idx == 13)) goto L16;
    if ((idx == 14)) goto L17;
    if ((idx == 15)) goto L18;
    goto L2;
  L1:;
    return;
  L2:;
#line 1216
    (*r) = 0;
    (*g) = 0;
    (*b) = 0;
    goto L1;
  L3:;
#line 1199
    (*r) = 0;
    (*g) = 0;
    (*b) = 0;
    goto L1;
  L4:;
#line 1200
    (*r) = 255;
    (*g) = 255;
    (*b) = 255;
    goto L1;
  L5:;
#line 1201
    (*r) = 204;
    (*g) = 0;
    (*b) = 0;
    goto L1;
  L6:;
#line 1202
    (*r) = 255;
    (*g) = 102;
    (*b) = 0;
    goto L1;
  L7:;
#line 1203
    (*r) = 0;
    (*g) = 0;
    (*b) = 255;
    goto L1;
  L8:;
#line 1204
    (*r) = 204;
    (*g) = 0;
    (*b) = 255;
    goto L1;
  L9:;
#line 1205
    (*r) = 0;
    (*g) = 153;
    (*b) = 0;
    goto L1;
  L10:;
#line 1206
    (*r) = 255;
    (*g) = 255;
    (*b) = 0;
    goto L1;
  L11:;
#line 1207
    (*r) = 255;
    (*g) = 153;
    (*b) = 0;
    goto L1;
  L12:;
#line 1208
    (*r) = 255;
    (*g) = 0;
    (*b) = 204;
    goto L1;
  L13:;
#line 1209
    (*r) = 170;
    (*g) = 85;
    (*b) = 0;
    goto L1;
  L14:;
#line 1210
    (*r) = 255;
    (*g) = 221;
    (*b) = 187;
    goto L1;
  L15:;
#line 1211
    (*r) = 238;
    (*g) = 187;
    (*b) = 119;
    goto L1;
  L16:;
#line 1212
    (*r) = 204;
    (*g) = 204;
    (*b) = 204;
    goto L1;
  L17:;
#line 1213
    (*r) = 136;
    (*g) = 136;
    (*b) = 136;
    goto L1;
  L18:;
#line 1214
    (*r) = 68;
    (*g) = 68;
    (*b) = 68;
    goto L1;
}

static void Render_GetOptionLabel(int32_t optIdx, char *buf, uint32_t buf_high) {
    (void)optIdx;
    (void)buf;
    (void)buf_high;
    int32_t i;
    int32_t base;
    char tabLabels[24 + 1];
#line 1225
    tabLabels[0] = 'I';
    tabLabels[1] = 't';
    tabLabels[2] = 'e';
#line 1226
    tabLabels[3] = 'm';
    tabLabels[4] = 's';
#line 1227
    tabLabels[5] = 'M';
    tabLabels[6] = 'a';
    tabLabels[7] = 'g';
#line 1228
    tabLabels[8] = 'i';
    tabLabels[9] = 'c';
#line 1229
    tabLabels[10] = 'T';
    tabLabels[11] = 'a';
    tabLabels[12] = 'l';
#line 1230
    tabLabels[13] = 'k';
    tabLabels[14] = ' ';
#line 1231
    tabLabels[15] = 'T';
    tabLabels[16] = 'r';
    tabLabels[17] = 'a';
#line 1232
    tabLabels[18] = 'd';
    tabLabels[19] = 'e';
#line 1233
    tabLabels[20] = 'G';
    tabLabels[21] = 'a';
    tabLabels[22] = 'm';
#line 1234
    tabLabels[23] = 'e';
    tabLabels[24] = ' ';
    if (optIdx < 5) goto L2; else goto L3;
  L1:;
#line 1251
    buf[5] = '\0';
    return;
  L2:;
#line 1237
    base = (optIdx * 5);
    i = 0;
    goto L4;
  L3:;
#line 1242
    base = ((optIdx - 5) * 5);
    i = 0;
    goto L8;
  L4:;
    if (i <= 4) goto L5; else goto L7;
  L5:;
#line 1239
    buf[i] = tabLabels[(base + i)];
    goto L6;
  L6:;
    i = (i + 1);
    goto L4;
  L7:;
    goto L1;
  L8:;
    if (i <= 4) goto L9; else goto L11;
  L9:;
    if ((base + i) <= 74) goto L13; else goto L14;
  L10:;
    i = (i + 1);
    goto L8;
  L11:;
    goto L1;
  L12:;
    goto L10;
  L13:;
#line 1245
    buf[i] = Menu_menus[Menu_cmode].labels[(base + i)];
    goto L12;
  L14:;
#line 1247
    buf[i] = ' ';
    goto L12;
}

static void Render_DrawMenu(void) {
    static const int32_t LabelOff = 4;
    static const int32_t LabelChars = 6;
    int32_t j;
    int32_t optIdx;
    int32_t col;
    int32_t penb;
    int32_t bx;
    int32_t by;
    int32_t bgR;
    int32_t bgG;
    int32_t bgB;
    int32_t fgR;
    int32_t fgG;
    int32_t fgB;
    int selected;
    char label[5 + 1];
    j = 0;
    goto L1;
  L1:;
    if (j <= (Menu_optionCount - 1)) goto L2; else goto L4;
  L2:;
#line 1264
    optIdx = Menu_realOptions[j];
    if (optIdx < 0) goto L6; else goto L7;
  L3:;
    j = (j + 1);
    goto L1;
  L4:;
    return;
  L5:;
    goto L3;
  L6:;
    goto L5;
  L7:;
#line 1267
    selected = ((uint32_t)(((uint32_t)(Menu_menus[Menu_cmode].enabled[optIdx]))) & (uint32_t)(1)) != 0;
#line 1269
    col = m2_mod(j, 2);
#line 1270
    bx = (430 + (col * 52));
#line 1271
    by = (2 + (m2_div(j, 2) * 9));
    if (optIdx < 5) goto L9; else goto L10;
  L8:;
#line 1298
    Render_PalColor(penb, &bgR, &bgG, &bgB);
    if (selected) goto L28; else goto L29;
  L9:;
#line 1277
    penb = 4;
    goto L8;
  L10:;
    if (Menu_cmode == 8) goto L11; else goto L12;
  L11:;
#line 1279
    penb = 14;
    goto L8;
  L12:;
    if (Menu_cmode == 9) goto L13; else goto L14;
  L13:;
#line 1281
    penb = 13;
    goto L8;
  L14:;
    if (Menu_cmode == 6) goto L15; else goto L16;
  L15:;
    if (((optIdx - 5) == 0)) goto L19;
    if (((optIdx - 5) == 1)) goto L20;
    if (((optIdx - 5) == 2)) goto L21;
    if (((optIdx - 5) == 3)) goto L22;
    if (((optIdx - 5) == 4)) goto L23;
    if (((optIdx - 5) == 5)) goto L24;
    goto L18;
  L16:;
    if (Menu_cmode == 5) goto L25; else goto L26;
  L17:;
    goto L8;
  L18:;
#line 1291
    penb = Menu_menus[Menu_cmode].color;
    goto L17;
  L19:;
#line 1285
    penb = 8;
    goto L17;
  L20:;
#line 1286
    penb = 6;
    goto L17;
  L21:;
#line 1287
    penb = 4;
    goto L17;
  L22:;
#line 1288
    penb = 2;
    goto L17;
  L23:;
#line 1289
    penb = 14;
    goto L17;
  L24:;
#line 1290
    penb = 1;
    goto L17;
  L25:;
#line 1294
    penb = optIdx;
    goto L8;
  L26:;
#line 1296
    penb = Menu_menus[Menu_cmode].color;
    goto L8;
  L27:;
#line 1306
    Render_GetOptionLabel(optIdx, label, 5);
#line 1307
    HudFont_DrawMenuStr(Platform_ren, label, 5, bx, by, 6, 4, fgR, fgG, fgB, bgR, bgG, bgB);
    goto L5;
  L28:;
#line 1301
    fgR = 255;
    fgG = 255;
    fgB = 255;
    goto L27;
  L29:;
#line 1303
    fgR = 0;
    fgG = 0;
    fgB = 0;
    goto L27;
}

static void Render_DrawMinimap(void) {
    int32_t x;
    int32_t y;
    int32_t mx;
    int32_t my;
    int32_t r;
    int32_t g;
    int32_t b;
    int32_t px;
    int32_t py;
    if (Assets_hudTex != NULL) goto L2; else goto L1;
  L1:;
#line 1319
    mx = Render_S((320 - 66));
#line 1320
    my = Render_S((143 + 2));
#line 1322
    Canvas_SetColor(Platform_ren, 10, 10, 20, 180);
#line 1323
    Canvas_FillRect(Platform_ren, (mx - Render_S(1)), (my - Render_S(1)), Render_S(66), Render_S((57 - 2)));
    x = 0;
    goto L3;
  L2:;
    return;
  L3:;
    if (x <= (64 - 1)) goto L4; else goto L6;
  L4:;
    y = 0;
    goto L7;
  L5:;
    x = (x + 1);
    goto L3;
  L6:;
#line 1333
    px = m2_div(Actor_actors[0].absX, 16);
#line 1334
    py = m2_div(Actor_actors[0].absY, 16);
    if (m2_mod(m2_div(GameState_cycle, 10), 2) == 0) goto L12; else goto L13;
  L7:;
    if (y <= (64 - 1)) goto L8; else goto L10;
  L8:;
#line 1327
    Render_TerrainColor(World_tiles[x][y].terrain, &r, &g, &b);
#line 1328
    Canvas_SetColor(Platform_ren, r, g, b, 255);
#line 1329
    Canvas_FillRect(Platform_ren, (mx + (x * 3)), (my + (y * 3)), 3, 3);
    goto L9;
  L9:;
    y = (y + 1);
    goto L7;
  L10:;
    goto L5;
  L11:;
#line 1340
    Canvas_FillRect(Platform_ren, (mx + (px * 3)), (my + (py * 3)), 3, 3);
#line 1342
    Canvas_SetColor(Platform_ren, 255, 40, 40, 255);
    x = 1;
    goto L14;
  L12:;
#line 1336
    Canvas_SetColor(Platform_ren, 255, 255, 255, 255);
    goto L11;
  L13:;
#line 1338
    Canvas_SetColor(Platform_ren, 255, 255, 0, 255);
    goto L11;
  L14:;
    if (x <= (Actor_actorCount - 1)) goto L15; else goto L17;
  L15:;
    if (Actor_actors[x].state != 15) goto L19; else goto L18;
  L16:;
    x = (x + 1);
    goto L14;
  L17:;
    return;
  L18:;
    goto L16;
  L19:;
#line 1345
    px = m2_div(Actor_actors[x].absX, 16);
#line 1346
    py = m2_div(Actor_actors[x].absY, 16);
#line 1347
    Canvas_FillRect(Platform_ren, (mx + (px * 3)), (my + (py * 3)), 3, 3);
    goto L18;
}

static void Render_DrawBirdView(void) {
    static const int32_t MapCols = 128;
    static const int32_t MapRows = 64;
    static const int32_t CellSize = 2;
    static const int32_t SampleSize = 32;
    static const int32_t RegionW = 16384;
    static const int32_t RegionH = 8192;
    int32_t x;
    int32_t y;
    int32_t i;
    int32_t r;
    int32_t g;
    int32_t b;
    int32_t regionX;
    int32_t regionY;
    int32_t baseX;
    int32_t baseY;
    int32_t mapX;
    int32_t mapY;
    int32_t px;
    int32_t py;
    int32_t sampleX;
    int32_t sampleY;
    if (Assets_currentRegion < 0) goto L2; else goto L1;
  L1:;
    if (Assets_currentRegion < 8) goto L4; else goto L5;
  L2:;
    return;
  L3:;
#line 1374
    baseX = (Actor_actors[0].absX - m2_div((128 * 32), 2));
#line 1375
    baseY = (Actor_actors[0].absY - m2_div((64 * 32), 2));
    if (baseX < regionX) goto L7; else goto L6;
  L4:;
#line 1368
    regionX = (m2_mod(Assets_currentRegion, 2) * 16384);
#line 1369
    regionY = (m2_div(Assets_currentRegion, 2) * 8192);
    goto L3;
  L5:;
#line 1371
    regionX = 0;
#line 1372
    regionY = (m2_div(Assets_currentRegion, 2) * 8192);
    goto L3;
  L6:;
    if (baseY < regionY) goto L9; else goto L8;
  L7:;
#line 1376
    baseX = regionX;
    goto L6;
  L8:;
    if ((baseX + (128 * 32)) > (regionX + 16384)) goto L11; else goto L10;
  L9:;
#line 1377
    baseY = regionY;
    goto L8;
  L10:;
    if ((baseY + (64 * 32)) > (regionY + 8192)) goto L13; else goto L12;
  L11:;
#line 1379
    baseX = ((regionX + 16384) - (128 * 32));
    goto L10;
  L12:;
#line 1384
    mapX = m2_div((320 - (128 * 2)), 2);
#line 1385
    mapY = m2_div((143 - (64 * 2)), 2);
#line 1387
    Canvas_SetColor(Platform_ren, 6, 10, 14, 255);
#line 1388
    Canvas_FillRect(Platform_ren, 0, 0, Render_S(320), Render_S(143));
    y = 0;
    goto L14;
  L13:;
#line 1382
    baseY = ((regionY + 8192) - (64 * 32));
    goto L12;
  L14:;
    if (y <= (64 - 1)) goto L15; else goto L17;
  L15:;
    x = 0;
    goto L18;
  L16:;
    y = (y + 1);
    goto L14;
  L17:;
#line 1399
    Canvas_SetColor(Platform_ren, 190, 200, 205, 255);
#line 1400
    Canvas_DrawRect(Platform_ren, Render_S((mapX - 1)), Render_S((mapY - 1)), Render_S(((128 * 2) + 2)), Render_S(((64 * 2) + 2)));
#line 1403
    Canvas_SetColor(Platform_ren, 40, 240, 240, 255);
    i = 0;
    goto L22;
  L18:;
    if (x <= (128 - 1)) goto L19; else goto L21;
  L19:;
#line 1391
    sampleX = ((baseX + (x * 32)) + m2_div(32, 2));
#line 1392
    sampleY = ((baseY + (y * 32)) + m2_div(32, 2));
#line 1393
    DebugMap_GetTileMapColor(Assets_GetSectorByte(sampleX, sampleY), &r, &g, &b);
#line 1394
    Canvas_SetColor(Platform_ren, r, g, b, 255);
#line 1395
    Canvas_FillRect(Platform_ren, Render_S((mapX + (x * 2))), Render_S((mapY + (y * 2))), Render_S(2), Render_S(2));
    goto L20;
  L20:;
    x = (x + 1);
    goto L18;
  L21:;
    goto L16;
  L22:;
    if (i <= (11 - 1)) goto L23; else goto L25;
  L23:;
#line 1405
    GameState_GetStoneCircle(i, &sampleX, &sampleY);
    if (sampleX >= baseX) goto L30; else goto L26;
  L24:;
    i = (i + 1);
    goto L22;
  L25:;
#line 1419
    Canvas_SetColor(Platform_ren, 230, 55, 45, 255);
    i = 1;
    goto L31;
  L26:;
    goto L24;
  L27:;
#line 1410
    px = m2_div((sampleX - baseX), 32);
#line 1411
    py = m2_div((sampleY - baseY), 32);
#line 1412
    Canvas_DrawRect(Platform_ren, Render_S(((mapX + (px * 2)) - 3)), Render_S(((mapY + (py * 2)) - 3)), Render_S(7), Render_S(7));
#line 1414
    Canvas_DrawRect(Platform_ren, Render_S(((mapX + (px * 2)) - 2)), Render_S(((mapY + (py * 2)) - 2)), Render_S(5), Render_S(5));
    goto L26;
  L28:;
    if (sampleY < (baseY + (64 * 32))) goto L27; else goto L26;
  L29:;
    if (sampleY >= baseY) goto L28; else goto L26;
  L30:;
    if (sampleX < (baseX + (128 * 32))) goto L29; else goto L26;
  L31:;
    if (i <= (Actor_actorCount - 1)) goto L32; else goto L34;
  L32:;
    if (Actor_actors[i].state != 15) goto L40; else goto L35;
  L33:;
    i = (i + 1);
    goto L31;
  L34:;
    if (Actor_actors[0].absX >= baseX) goto L45; else goto L41;
  L35:;
    goto L33;
  L36:;
#line 1426
    px = m2_div((Actor_actors[i].absX - baseX), 32);
#line 1427
    py = m2_div((Actor_actors[i].absY - baseY), 32);
#line 1428
    Canvas_FillRect(Platform_ren, Render_S((mapX + (px * 2))), Render_S((mapY + (py * 2))), Render_S(2), Render_S(2));
    goto L35;
  L37:;
    if (Actor_actors[i].absY < (baseY + (64 * 32))) goto L36; else goto L35;
  L38:;
    if (Actor_actors[i].absY >= baseY) goto L37; else goto L35;
  L39:;
    if (Actor_actors[i].absX < (baseX + (128 * 32))) goto L38; else goto L35;
  L40:;
    if (Actor_actors[i].absX >= baseX) goto L39; else goto L35;
  L41:;
    return;
  L42:;
#line 1437
    px = m2_div((Actor_actors[0].absX - baseX), 32);
#line 1438
    py = m2_div((Actor_actors[0].absY - baseY), 32);
    if (m2_mod(m2_div(GameState_cycle, 10), 2) == 0) goto L47; else goto L48;
  L43:;
    if (Actor_actors[0].absY < (baseY + (64 * 32))) goto L42; else goto L41;
  L44:;
    if (Actor_actors[0].absY >= baseY) goto L43; else goto L41;
  L45:;
    if (Actor_actors[0].absX < (baseX + (128 * 32))) goto L44; else goto L41;
  L46:;
#line 1444
    Canvas_FillRect(Platform_ren, Render_S(((mapX + (px * 2)) - 1)), Render_S(((mapY + (py * 2)) - 1)), Render_S((2 + 2)), Render_S((2 + 2)));
    goto L41;
  L47:;
#line 1440
    Canvas_SetColor(Platform_ren, 255, 255, 255, 255);
    goto L46;
  L48:;
#line 1442
    Canvas_SetColor(Platform_ren, 255, 230, 30, 255);
    goto L46;
}

static void Render_DrawRegionFade(void) {
    int32_t alpha;
    if (GameState_regionFade <= 0) goto L2; else goto L1;
  L1:;
#line 1455
    alpha = (GameState_regionFade * 25);
    if (alpha > 255) goto L4; else goto L3;
  L2:;
    return;
  L3:;
#line 1457
    Canvas_SetColor(Platform_ren, 0, 0, 0, alpha);
#line 1458
    Canvas_FillRect(Platform_ren, 0, 0, Render_S(320), Render_S(143));
    return;
  L4:;
#line 1456
    alpha = 255;
    goto L3;
}

static void Render_AppendInt(char *buf, uint32_t buf_high, int32_t *pos, int32_t n) {
    (void)buf;
    (void)buf_high;
    (void)pos;
    (void)n;
    int32_t i;
    int32_t len;
    char tmp[7 + 1];
    if (n < 0) goto L2; else goto L1;
  L1:;
#line 1466
    len = 0;
    if (n == 0) goto L4; else goto L5;
  L2:;
#line 1465
    n = 0;
    goto L1;
  L3:;
    i = (len - 1);
    goto L9;
  L4:;
#line 1468
    tmp[0] = '0';
    len = 1;
    goto L3;
  L5:;
    goto L6;
  L6:;
    if (n > 0) goto L7; else goto L8;
  L7:;
#line 1471
    tmp[len] = ((char)((((int32_t)((unsigned char)('0'))) + m2_mod(n, 10))));
#line 1472
    n = m2_div(n, 10);
#line 1473
    (len++);
    goto L6;
  L8:;
    goto L3;
  L9:;
    if (i >= 0) goto L10; else goto L12;
  L10:;
    if ((*pos) <= buf_high) goto L14; else goto L13;
  L11:;
    i = (i + (-1));
    goto L9;
  L12:;
    if ((*pos) <= buf_high) goto L16; else goto L15;
  L13:;
    goto L11;
  L14:;
#line 1478
    buf[(*pos)] = tmp[i];
    ((*pos)++);
    goto L13;
  L15:;
    return;
  L16:;
#line 1481
    buf[(*pos)] = '\0';
    goto L15;
}

static void Render_BuildStat(char *label, uint32_t label_high, int32_t val, char *buf, uint32_t buf_high) {
    (void)label;
    (void)label_high;
    (void)val;
    (void)buf;
    (void)buf_high;
    int32_t i;
    int32_t p;
#line 1488
    p = 0;
#line 1489
    i = 0;
    goto L1;
  L1:;
    if (i <= label_high) goto L4; else goto L3;
  L2:;
    if (p <= buf_high) goto L6; else goto L5;
  L3:;
#line 1494
    Render_AppendInt(buf, buf_high, &p, val);
    return;
  L4:;
    if (label[i] != '\0') goto L2; else goto L3;
  L5:;
#line 1492
    (i++);
    goto L1;
  L6:;
#line 1491
    buf[p] = label[i];
    (p++);
    goto L5;
}

static void Render_DrawMessage(void) {
    static const int32_t TXMIN = 16;
    static const int32_t TYMIN = 5;
    static const int32_t RowH = 10;
    static const int32_t StatY = 45;
    int32_t row;
    char line[39 + 1];
    char statBuf[15 + 1];
    row = 0;
    goto L1;
  L1:;
    if (row <= 3) goto L2; else goto L4;
  L2:;
#line 1511
    HudLog_GetLine(row, line, 39);
    if (line[0] != '\0') goto L6; else goto L5;
  L3:;
    row = (row + 1);
    goto L1;
  L4:;
#line 1518
    Render_BuildStat("Brv:", 4, HudLog_GetStatBrv(), statBuf, 15);
#line 1519
    HudFont_DrawHudStr(Platform_ren, statBuf, 15, 14, 45);
#line 1521
    Render_BuildStat("Lck:", 4, HudLog_GetStatLck(), statBuf, 15);
#line 1522
    HudFont_DrawHudStr(Platform_ren, statBuf, 15, 90, 45);
#line 1524
    Render_BuildStat("Knd:", 4, HudLog_GetStatKnd(), statBuf, 15);
#line 1525
    HudFont_DrawHudStr(Platform_ren, statBuf, 15, 168, 45);
#line 1527
    Render_BuildStat("Vit:", 4, HudLog_GetStatVit(), statBuf, 15);
#line 1528
    HudFont_DrawHudStr(Platform_ren, statBuf, 15, 245, 45);
#line 1530
    Render_BuildStat("Wlth:", 5, HudLog_GetStatWlth(), statBuf, 15);
#line 1531
    HudFont_DrawHudStr(Platform_ren, statBuf, 15, 321, 45);
#line 1533
    HudLog_logDirty = 0;
#line 1534
    HudLog_statDirty = 0;
    return;
  L5:;
    goto L3;
  L6:;
#line 1513
    HudFont_DrawHudStr(Platform_ren, line, 39, 16, (5 + (row * 10)));
    goto L5;
}

static void Render_DrawInventory(void) {
    int32_t b;
    int32_t i;
    int32_t stuff[44 + 1];
#line 1573
    b = Brothers_activeBrother;
    i = 0;
    goto L1;
  L1:;
    if (i <= 44) goto L2; else goto L4;
  L2:;
#line 1577
    stuff[i] = Brothers_brothers[b].stuff[i];
    goto L3;
  L3:;
    i = (i + 1);
    goto L1;
  L4:;
#line 1580
    Canvas_SetColor(Platform_ren, 0, 0, 0, 255);
#line 1581
    Canvas_FillRect(Platform_ren, 0, 0, Render_S(320), Render_S(143));
#line 1585
    Render_DrawInventory_DrawInvSlot(12, 10, 0, 0, 0, 8, 1, stuff[0]);
#line 1586
    Render_DrawInventory_DrawInvSlot(9, 10, 10, 0, 0, 8, 1, stuff[1]);
#line 1587
    Render_DrawInventory_DrawInvSlot(8, 10, 20, 0, 0, 8, 1, stuff[2]);
#line 1588
    Render_DrawInventory_DrawInvSlot(10, 10, 30, 0, 0, 8, 1, stuff[3]);
#line 1589
    Render_DrawInventory_DrawInvSlot(17, 10, 40, 0, 8, 8, 1, stuff[4]);
#line 1590
    Render_DrawInventory_DrawInvSlot(27, 10, 50, 0, 0, 8, 1, stuff[5]);
#line 1591
    Render_DrawInventory_DrawInvSlot(23, 10, 60, 0, 8, 8, 1, stuff[6]);
#line 1592
    Render_DrawInventory_DrawInvSlot(27, 10, 70, 0, 8, 8, 1, stuff[7]);
#line 1593
    Render_DrawInventory_DrawInvSlot(3, 30, 0, 3, 7, 1, 45, stuff[8]);
#line 1594
    Render_DrawInventory_DrawInvSlot(18, 50, 0, 9, 0, 8, 15, stuff[9]);
#line 1595
    Render_DrawInventory_DrawInvSlot(19, 65, 0, 6, 0, 5, 23, stuff[10]);
#line 1596
    Render_DrawInventory_DrawInvSlot(22, 80, 0, 8, 0, 7, 17, stuff[11]);
#line 1597
    Render_DrawInventory_DrawInvSlot(21, 95, 0, 7, 0, 6, 20, stuff[12]);
#line 1598
    Render_DrawInventory_DrawInvSlot(23, 110, 0, 10, 0, 9, 14, stuff[13]);
#line 1599
    Render_DrawInventory_DrawInvSlot(17, 125, 0, 6, 0, 5, 23, stuff[14]);
#line 1600
    Render_DrawInventory_DrawInvSlot(24, 140, 0, 10, 0, 9, 14, stuff[15]);
#line 1601
    Render_DrawInventory_DrawInvSlot(25, 160, 0, 5, 0, 5, 25, stuff[16]);
#line 1602
    Render_DrawInventory_DrawInvSlot(25, 172, 0, 5, 8, 5, 25, stuff[17]);
#line 1603
    Render_DrawInventory_DrawInvSlot(114, 184, 0, 5, 0, 5, 25, stuff[18]);
#line 1604
    Render_DrawInventory_DrawInvSlot(114, 196, 0, 5, 8, 5, 25, stuff[19]);
#line 1605
    Render_DrawInventory_DrawInvSlot(26, 208, 0, 5, 0, 5, 25, stuff[20]);
#line 1606
    Render_DrawInventory_DrawInvSlot(26, 220, 0, 5, 8, 5, 25, stuff[21]);
#line 1607
    Render_DrawInventory_DrawInvSlot(11, 0, 80, 0, 8, 8, 1, stuff[22]);
#line 1608
    Render_DrawInventory_DrawInvSlot(19, 0, 90, 0, 8, 8, 1, stuff[23]);
#line 1609
    Render_DrawInventory_DrawInvSlot(20, 0, 100, 0, 8, 8, 1, stuff[24]);
#line 1610
    Render_DrawInventory_DrawInvSlot(21, 232, 0, 10, 8, 8, 5, stuff[25]);
#line 1611
    Render_DrawInventory_DrawInvSlot(22, 0, 110, 0, 8, 8, 1, stuff[26]);
#line 1612
    Render_DrawInventory_DrawInvSlot(8, 14, 80, 0, 8, 8, 1, stuff[27]);
#line 1613
    Render_DrawInventory_DrawInvSlot(9, 14, 90, 0, 8, 8, 1, stuff[28]);
#line 1614
    Render_DrawInventory_DrawInvSlot(10, 14, 100, 0, 8, 8, 1, stuff[29]);
#line 1615
    Render_DrawInventory_DrawInvSlot(12, 14, 110, 0, 8, 8, 1, stuff[30]);
#line 1616
    Render_DrawInventory_DrawInvSlot(116, 145, 80, 3, 0, 16, 4, stuff[31]);
#line 1617
    Render_DrawInventory_DrawInvSlot(117, 170, 80, 3, 0, 16, 4, stuff[32]);
#line 1618
    Render_DrawInventory_DrawInvSlot(118, 195, 80, 3, 0, 16, 4, stuff[33]);
#line 1619
    Render_DrawInventory_DrawInvSlot(119, 220, 80, 3, 0, 16, 4, stuff[34]);
#line 1620
    Render_DrawInventory_DrawInvSlot(120, 245, 80, 3, 0, 16, 4, stuff[35]);
#line 1621
    Render_DrawInventory_DrawInvSlot(121, 270, 80, 3, 0, 16, 4, stuff[36]);
#line 1622
    Render_DrawInventory_DrawInvSlot(122, 120, 110, 3, 0, 16, 1, stuff[37]);
#line 1623
    Render_DrawInventory_DrawInvSlot(123, 140, 110, 3, 0, 16, 1, stuff[38]);
#line 1624
    Render_DrawInventory_DrawInvSlot(124, 160, 110, 3, 0, 16, 1, stuff[39]);
#line 1625
    Render_DrawInventory_DrawInvSlot(122, 180, 110, 3, 0, 16, 1, stuff[40]);
#line 1626
    Render_DrawInventory_DrawInvSlot(123, 200, 110, 3, 0, 16, 1, stuff[41]);
#line 1627
    Render_DrawInventory_DrawInvSlot(124, 220, 110, 3, 0, 16, 1, stuff[42]);
#line 1628
    Render_DrawInventory_DrawInvSlot(122, 240, 110, 3, 0, 16, 1, stuff[43]);
#line 1629
    Render_DrawInventory_DrawInvSlot(123, 260, 110, 3, 0, 16, 1, stuff[44]);
    return;
}

static void Render_DrawFairy(void) {
    int32_t sx;
    int32_t sy;
    int32_t sprY;
    if (WorldObj_objTex == NULL) goto L2; else goto L1;
  L1:;
    if (GameState_fairyActive) goto L3; else goto L4;
  L2:;
    return;
  L3:;
#line 1637
    Texture_SetColorMod(WorldObj_objTex, 255, 255, 255);
#line 1638
    sx = ((GameState_fairyX - World_camX) * 3);
#line 1639
    sy = ((Actor_actors[0].absY - World_camY) * 3);
#line 1640
    sprY = ((100 + m2_mod(GameState_cycle, 2)) * 16);
#line 1641
    Platform_DrawTexRegion(WorldObj_objTex, 0, sprY, 16, 16, (sx - Render_S(8)), (sy - Render_S(8)), Render_S(16), Render_S(16));
    return;
  L4:;
    return;
}

static void Render_InitWitchPoints(void) {
#line 1654
    Render_wpX[0] = 0;
    Render_wpY[0] = 100;
    Render_wpNX[0] = 0;
    Render_wpNY[0] = 10;
#line 1655
    Render_wpX[1] = 9;
    Render_wpY[1] = 99;
    Render_wpNX[1] = 0;
    Render_wpNY[1] = 9;
#line 1656
    Render_wpX[2] = 19;
    Render_wpY[2] = 98;
    Render_wpNX[2] = 1;
    Render_wpNY[2] = 9;
#line 1657
    Render_wpX[3] = 29;
    Render_wpY[3] = 95;
    Render_wpNX[3] = 2;
    Render_wpNY[3] = 9;
#line 1658
    Render_wpX[4] = 38;
    Render_wpY[4] = 92;
    Render_wpNX[4] = 3;
    Render_wpNY[4] = 9;
#line 1659
    Render_wpX[5] = 47;
    Render_wpY[5] = 88;
    Render_wpNX[5] = 4;
    Render_wpNY[5] = 8;
#line 1660
    Render_wpX[6] = 55;
    Render_wpY[6] = 83;
    Render_wpNX[6] = 5;
    Render_wpNY[6] = 8;
#line 1661
    Render_wpX[7] = 63;
    Render_wpY[7] = 77;
    Render_wpNX[7] = 6;
    Render_wpNY[7] = 7;
#line 1662
    Render_wpX[8] = 70;
    Render_wpY[8] = 70;
    Render_wpNX[8] = 7;
    Render_wpNY[8] = 7;
#line 1663
    Render_wpX[9] = 77;
    Render_wpY[9] = 63;
    Render_wpNX[9] = 7;
    Render_wpNY[9] = 6;
#line 1664
    Render_wpX[10] = 83;
    Render_wpY[10] = 55;
    Render_wpNX[10] = 8;
    Render_wpNY[10] = 5;
#line 1665
    Render_wpX[11] = 88;
    Render_wpY[11] = 47;
    Render_wpNX[11] = 8;
    Render_wpNY[11] = 4;
#line 1666
    Render_wpX[12] = 92;
    Render_wpY[12] = 38;
    Render_wpNX[12] = 9;
    Render_wpNY[12] = 3;
#line 1667
    Render_wpX[13] = 95;
    Render_wpY[13] = 29;
    Render_wpNX[13] = 9;
    Render_wpNY[13] = 2;
#line 1668
    Render_wpX[14] = 98;
    Render_wpY[14] = 19;
    Render_wpNX[14] = 9;
    Render_wpNY[14] = 1;
#line 1669
    Render_wpX[15] = 99;
    Render_wpY[15] = 9;
    Render_wpNX[15] = 9;
    Render_wpNY[15] = 0;
#line 1670
    Render_wpX[16] = 100;
    Render_wpY[16] = 0;
    Render_wpNX[16] = 10;
    Render_wpNY[16] = 0;
#line 1671
    Render_wpX[17] = 99;
    Render_wpY[17] = (-10);
    Render_wpNX[17] = 9;
    Render_wpNY[17] = (-1);
#line 1672
    Render_wpX[18] = 98;
    Render_wpY[18] = (-20);
    Render_wpNX[18] = 9;
    Render_wpNY[18] = (-2);
#line 1673
    Render_wpX[19] = 95;
    Render_wpY[19] = (-30);
    Render_wpNX[19] = 9;
    Render_wpNY[19] = (-3);
#line 1674
    Render_wpX[20] = 92;
    Render_wpY[20] = (-39);
    Render_wpNX[20] = 9;
    Render_wpNY[20] = (-4);
#line 1675
    Render_wpX[21] = 88;
    Render_wpY[21] = (-48);
    Render_wpNX[21] = 8;
    Render_wpNY[21] = (-5);
#line 1676
    Render_wpX[22] = 83;
    Render_wpY[22] = (-56);
    Render_wpNX[22] = 8;
    Render_wpNY[22] = (-6);
#line 1677
    Render_wpX[23] = 77;
    Render_wpY[23] = (-64);
    Render_wpNX[23] = 7;
    Render_wpNY[23] = (-7);
#line 1678
    Render_wpX[24] = 70;
    Render_wpY[24] = (-71);
    Render_wpNX[24] = 7;
    Render_wpNY[24] = (-8);
#line 1679
    Render_wpX[25] = 63;
    Render_wpY[25] = (-78);
    Render_wpNX[25] = 6;
    Render_wpNY[25] = (-8);
#line 1680
    Render_wpX[26] = 55;
    Render_wpY[26] = (-84);
    Render_wpNX[26] = 5;
    Render_wpNY[26] = (-9);
#line 1681
    Render_wpX[27] = 47;
    Render_wpY[27] = (-89);
    Render_wpNX[27] = 4;
    Render_wpNY[27] = (-9);
#line 1682
    Render_wpX[28] = 38;
    Render_wpY[28] = (-93);
    Render_wpNX[28] = 3;
    Render_wpNY[28] = (-10);
#line 1683
    Render_wpX[29] = 29;
    Render_wpY[29] = (-96);
    Render_wpNX[29] = 2;
    Render_wpNY[29] = (-10);
#line 1684
    Render_wpX[30] = 19;
    Render_wpY[30] = (-99);
    Render_wpNX[30] = 1;
    Render_wpNY[30] = (-10);
#line 1685
    Render_wpX[31] = 9;
    Render_wpY[31] = (-100);
    Render_wpNX[31] = 0;
    Render_wpNY[31] = (-10);
#line 1686
    Render_wpX[32] = 0;
    Render_wpY[32] = (-100);
    Render_wpNX[32] = 0;
    Render_wpNY[32] = (-10);
#line 1687
    Render_wpX[33] = (-10);
    Render_wpY[33] = (-100);
    Render_wpNX[33] = (-1);
    Render_wpNY[33] = (-10);
#line 1688
    Render_wpX[34] = (-20);
    Render_wpY[34] = (-99);
    Render_wpNX[34] = (-2);
    Render_wpNY[34] = (-10);
#line 1689
    Render_wpX[35] = (-30);
    Render_wpY[35] = (-96);
    Render_wpNX[35] = (-3);
    Render_wpNY[35] = (-10);
#line 1690
    Render_wpX[36] = (-39);
    Render_wpY[36] = (-93);
    Render_wpNX[36] = (-4);
    Render_wpNY[36] = (-10);
#line 1691
    Render_wpX[37] = (-48);
    Render_wpY[37] = (-89);
    Render_wpNX[37] = (-5);
    Render_wpNY[37] = (-9);
#line 1692
    Render_wpX[38] = (-56);
    Render_wpY[38] = (-84);
    Render_wpNX[38] = (-6);
    Render_wpNY[38] = (-9);
#line 1693
    Render_wpX[39] = (-64);
    Render_wpY[39] = (-78);
    Render_wpNX[39] = (-7);
    Render_wpNY[39] = (-8);
#line 1694
    Render_wpX[40] = (-71);
    Render_wpY[40] = (-71);
    Render_wpNX[40] = (-8);
    Render_wpNY[40] = (-8);
#line 1695
    Render_wpX[41] = (-78);
    Render_wpY[41] = (-64);
    Render_wpNX[41] = (-8);
    Render_wpNY[41] = (-7);
#line 1696
    Render_wpX[42] = (-84);
    Render_wpY[42] = (-56);
    Render_wpNX[42] = (-9);
    Render_wpNY[42] = (-6);
#line 1697
    Render_wpX[43] = (-89);
    Render_wpY[43] = (-48);
    Render_wpNX[43] = (-9);
    Render_wpNY[43] = (-5);
#line 1698
    Render_wpX[44] = (-93);
    Render_wpY[44] = (-39);
    Render_wpNX[44] = (-10);
    Render_wpNY[44] = (-4);
#line 1699
    Render_wpX[45] = (-96);
    Render_wpY[45] = (-30);
    Render_wpNX[45] = (-10);
    Render_wpNY[45] = (-3);
#line 1700
    Render_wpX[46] = (-99);
    Render_wpY[46] = (-20);
    Render_wpNX[46] = (-10);
    Render_wpNY[46] = (-2);
#line 1701
    Render_wpX[47] = (-100);
    Render_wpY[47] = (-10);
    Render_wpNX[47] = (-10);
    Render_wpNY[47] = (-1);
#line 1702
    Render_wpX[48] = (-100);
    Render_wpY[48] = (-1);
    Render_wpNX[48] = (-10);
    Render_wpNY[48] = (-1);
#line 1703
    Render_wpX[49] = (-100);
    Render_wpY[49] = 9;
    Render_wpNX[49] = (-10);
    Render_wpNY[49] = 0;
#line 1704
    Render_wpX[50] = (-99);
    Render_wpY[50] = 19;
    Render_wpNX[50] = (-10);
    Render_wpNY[50] = 1;
#line 1705
    Render_wpX[51] = (-96);
    Render_wpY[51] = 29;
    Render_wpNX[51] = (-10);
    Render_wpNY[51] = 2;
#line 1706
    Render_wpX[52] = (-93);
    Render_wpY[52] = 38;
    Render_wpNX[52] = (-10);
    Render_wpNY[52] = 3;
#line 1707
    Render_wpX[53] = (-89);
    Render_wpY[53] = 47;
    Render_wpNX[53] = (-9);
    Render_wpNY[53] = 4;
#line 1708
    Render_wpX[54] = (-84);
    Render_wpY[54] = 55;
    Render_wpNX[54] = (-9);
    Render_wpNY[54] = 5;
#line 1709
    Render_wpX[55] = (-78);
    Render_wpY[55] = 63;
    Render_wpNX[55] = (-8);
    Render_wpNY[55] = 6;
#line 1710
    Render_wpX[56] = (-71);
    Render_wpY[56] = 70;
    Render_wpNX[56] = (-8);
    Render_wpNY[56] = 7;
#line 1711
    Render_wpX[57] = (-64);
    Render_wpY[57] = 77;
    Render_wpNX[57] = (-7);
    Render_wpNY[57] = 7;
#line 1712
    Render_wpX[58] = (-56);
    Render_wpY[58] = 83;
    Render_wpNX[58] = (-6);
    Render_wpNY[58] = 8;
#line 1713
    Render_wpX[59] = (-48);
    Render_wpY[59] = 88;
    Render_wpNX[59] = (-5);
    Render_wpNY[59] = 8;
#line 1714
    Render_wpX[60] = (-39);
    Render_wpY[60] = 92;
    Render_wpNX[60] = (-4);
    Render_wpNY[60] = 9;
#line 1715
    Render_wpX[61] = (-30);
    Render_wpY[61] = 95;
    Render_wpNX[61] = (-3);
    Render_wpNY[61] = 9;
#line 1716
    Render_wpX[62] = (-20);
    Render_wpY[62] = 98;
    Render_wpNX[62] = (-2);
    Render_wpNY[62] = 9;
#line 1717
    Render_wpX[63] = (-10);
    Render_wpY[63] = 99;
    Render_wpNX[63] = (-1);
    Render_wpNY[63] = 9;
#line 1718
    Render_witchInited = 1;
    return;
}

static void Render_DrawWitchBeam(void) {
    int32_t i;
    int32_t wx;
    int32_t wy;
    int32_t a1;
    int32_t a2;
    int32_t x1;
    int32_t m2_y1;
    int32_t x2;
    int32_t y2;
    int32_t x3;
    int32_t y3;
    int32_t x4;
    int32_t y4;
    int32_t dx;
    int32_t dy;
    int32_t dx1;
    int32_t dy1;
    int32_t hx;
    int32_t hy;
    if (GameState_witchFlag) goto L1; else goto L2;
  L1:;
    if (Render_witchInited) goto L3; else goto L4;
  L2:;
    return;
  L3:;
    i = 1;
    goto L5;
  L4:;
#line 1731
    Render_InitWitchPoints();
    goto L3;
  L5:;
    if (i <= (Actor_actorCount - 1)) goto L6; else goto L8;
  L6:;
    if (Actor_actors[i].actorType == 4) goto L12; else goto L9;
  L7:;
    i = (i + 1);
    goto L5;
  L8:;
    return;
  L9:;
    goto L7;
  L10:;
#line 1739
    wx = ((Actor_actors[i].absX - World_camX) * 3);
#line 1740
    wy = (((Actor_actors[i].absY - 15) - World_camY) * 3);
#line 1743
    a1 = m2_mod((GameState_witchIndex + 63), 64);
#line 1744
    a2 = m2_mod((GameState_witchIndex + 1), 64);
#line 1747
    x1 = (wx + (Render_wpX[a1] * 3));
#line 1748
    m2_y1 = (wy + (Render_wpY[a1] * 3));
#line 1749
    x2 = (wx + (Render_wpNX[a1] * 3));
#line 1750
    y2 = (wy + (Render_wpNY[a1] * 3));
#line 1753
    x3 = (wx + (Render_wpX[a2] * 3));
#line 1754
    y3 = (wy + (Render_wpY[a2] * 3));
#line 1755
    x4 = (wx + (Render_wpNX[a2] * 3));
#line 1756
    y4 = (wy + (Render_wpNY[a2] * 3));
#line 1761
    hx = ((Actor_actors[0].absX - World_camX) * 3);
#line 1762
    hy = ((Actor_actors[0].absY - World_camY) * 3);
#line 1764
    dx = (x1 - x2);
    dy = (m2_y1 - y2);
#line 1765
    dx1 = (hx - x2);
    dy1 = (hy - y2);
#line 1766
    GameState_witchS1 = ((dx * dy1) - (dy * dx1));
#line 1768
    dx = (x3 - x4);
    dy = (y3 - y4);
#line 1769
    dx1 = (hx - x4);
    dy1 = (hy - y4);
#line 1770
    GameState_witchS2 = ((dx * dy1) - (dy * dx1));
#line 1775
    Canvas_SetClip(Platform_ren, 0, 0, Render_S(320), Render_S(143));
#line 1776
    Canvas_SetBlendMode(Platform_ren, 2);
#line 1777
    Canvas_SetColor(Platform_ren, 30, 40, 80, 255);
#line 1778
    Canvas_FillTriangle(Platform_ren, wx, wy, x1, m2_y1, x3, y3);
#line 1779
    Canvas_SetColor(Platform_ren, 20, 30, 60, 255);
#line 1780
    Canvas_FillTriangle(Platform_ren, wx, wy, x2, y2, x4, y4);
#line 1781
    Canvas_SetBlendMode(Platform_ren, 0);
#line 1782
    Canvas_ClearClip(Platform_ren);
    return;
  L11:;
    if (Actor_actors[i].state != 15) goto L10; else goto L9;
  L12:;
    if (Actor_actors[i].race == 9) goto L11; else goto L9;
}

/* Module Main */


int32_t frameStart;
int32_t elapsed;
int main(int _m2_argc, char **_m2_argv) {
    m2_argc = _m2_argc; m2_argv = _m2_argv;
    InOut_init();
    FileSystem_init();
    PixBuf_init();
    BinaryIO_init();
    Doors_init();
    Carrier_init();
    Movement_init();
    Combat_init();
    EnemyAI_init();
    Narration_init();
    Quest_init();
#line 27 "src/Main.mod"
    m2_WriteString("Faery Tale Adventure - Modula-2 reimplementation");
    m2_WriteLn();
#line 28
    m2_WriteString("  WASD/Arrows/Left Mouse=move  Space/Right Mouse=attack  F11=fullscreen");
    m2_WriteLn();
    if (Platform_Init()) goto L1; else goto L2;
  L1:;
#line 35
    Menu_InitMenus();
#line 36
    Render_InitOverlay();
#line 37
    GameState_InitGame();
#line 38
    Compass_InitCompass(Platform_ren);
#line 39
    Render_LoadCompass();
    if (HudFont_LoadHudFont(Platform_ren)) goto L3; else goto L4;
  L2:;
#line 31
    m2_WriteString("Failed to initialize platform");
    m2_WriteLn();
#line 32
    exit(0);
    goto L1;
  L3:;
#line 43
    WorldObj_InitWorldObjects();
#line 44
    WorldObj_LoadObjectSprites();
    if (Music_InitMusic()) goto L5; else goto L6;
  L4:;
#line 41
    m2_WriteString("Warning: HUD font load failed");
    m2_WriteLn();
    goto L3;
  L5:;
    if (SFX_InitSFX()) goto L7; else goto L8;
  L6:;
#line 46
    m2_WriteString("Warning: music init failed");
    m2_WriteLn();
    goto L5;
  L7:;
#line 51
    DebugMap_InitDebugMap();
#line 53
    Intro_RunIntro();
    goto L9;
  L8:;
#line 49
    m2_WriteString("Warning: SFX init failed");
    m2_WriteLn();
    goto L7;
  L9:;
    if (GameState_running) goto L10; else goto L11;
  L10:;
#line 56
    frameStart = Platform_GetTicks();
#line 58
    GameState_UpdateGame();
#line 59
    Music_UpdateMusic();
    if (GameState_mapToggled) goto L13; else goto L12;
  L11:;
#line 95
    SFX_ShutdownSFX();
#line 96
    Music_ShutdownMusic();
#line 97
    Platform_Shutdown();
    return 0;
  L12:;
#line 63
    Platform_BeginFrame();
    if (GameState_viewStatus == 4) goto L15; else goto L16;
  L13:;
#line 61
    DebugMap_ToggleDebugMap();
    goto L12;
  L14:;
#line 77
    Render_DrawHUD();
    if (Actor_actors[0].state == 12) goto L20; else goto L21;
  L15:;
#line 65
    Render_DrawInventory();
    goto L14;
  L16:;
    if (GameState_viewStatus == 5) goto L17; else goto L18;
  L17:;
#line 67
    Render_DrawBirdView();
    goto L14;
  L18:;
#line 69
    Render_DrawWorld();
#line 70
    WorldObj_DrawWorldObjects();
#line 71
    Render_DrawItems();
#line 72
    Render_DrawActors();
#line 73
    Render_DrawFairy();
#line 74
    Render_DrawWitchBeam();
#line 75
    Missile_DrawMissiles();
    goto L14;
  L19:;
#line 83
    Render_DrawMenu();
#line 84
    Render_DrawMessage();
#line 85
    Platform_EndFrame();
#line 87
    DebugMap_UpdateDebugMap();
#line 89
    elapsed = (Platform_GetTicks() - frameStart);
    if (elapsed < 33) goto L23; else goto L22;
  L20:;
#line 79
    Compass_DrawCompass(Platform_ren, Actor_actors[0].facing);
    goto L19;
  L21:;
#line 81
    Compass_DrawCompass(Platform_ren, (-1));
    goto L19;
  L22:;
    goto L9;
  L23:;
#line 91
    Platform_DelayMs((33 - elapsed));
    goto L22;
}

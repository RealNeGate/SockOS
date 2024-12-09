#include <kernel.h>

#define SPALL_MIN(a, b) (((a) < (b)) ? (a) : (b))

#pragma pack(push, 1)
typedef struct SpallHeader {
    uint64_t magic_header; // = 0x0BADF00D
    uint64_t version; // = 1
    double   timestamp_unit;
    uint64_t must_be_0;
} SpallHeader;

enum {
    SpallEventType_Invalid             = 0,
    SpallEventType_Custom_Data         = 1, // Basic readers can skip this.
    SpallEventType_StreamOver          = 2,

    SpallEventType_Begin               = 3,
    SpallEventType_End                 = 4,
    SpallEventType_Instant             = 5,

    SpallEventType_Overwrite_Timestamp = 6, // Retroactively change timestamp units - useful for incrementally improving RDTSC frequency.
    SpallEventType_Pad_Skip            = 7,
};

typedef struct SpallBeginEvent {
    uint8_t type; // = SpallEventType_Begin
    uint8_t category;

    uint32_t pid;
    uint32_t tid;
    double   when;

    uint8_t name_length;
    uint8_t args_length;
} SpallBeginEvent;

typedef struct SpallBeginEventMax {
    SpallBeginEvent event;
    char name_bytes[255];
    char args_bytes[255];
} SpallBeginEventMax;

typedef struct SpallEndEvent {
    uint8_t  type; // = SpallEventType_End
    uint32_t pid;
    uint32_t tid;
    double   when;
} SpallEndEvent;

typedef struct SpallPadSkipEvent {
    uint8_t  type; // = SpallEventType_Pad_Skip
    uint32_t size;
} SpallPadSkipEvent;
#pragma pack(pop)

static Lock spall_lock;

static void spall_write(const void* p, size_t n) {
    spin_lock(&spall_lock);

    // writing trace to COM2
    const char* pp = p;
    for (size_t i = 0; i < n; i++) {
        io_out8(0x2f8, pp[i]);
    }

    spin_unlock(&spall_lock);
}

void spall_header(void) {
    SpallHeader header = {
        .magic_header = 0x0BADF00D,
        .version = 1,
        .timestamp_unit = 1.0 / boot_info->tsc_freq,
        .must_be_0 = 0
    };
    spall_write(&header, sizeof(SpallHeader));
}

void spall_begin_event(const char* name, int tid) {
    int name_len = 0;
    while (name[name_len]) { name_len++; }

    double when = __rdtsc();

    uint8_t trunc_name_len = (uint8_t)SPALL_MIN(name_len, 255); // will be interpreted as truncated in the app (?)
    size_t ev_size = sizeof(SpallBeginEvent) + trunc_name_len;
    SpallBeginEventMax ev = {
        .event.type = SpallEventType_Begin,
        .event.category = 0,
        .event.pid = 0,
        .event.tid = tid,
        .event.when = when,
        .event.name_length = trunc_name_len,
    };
    memcpy(ev.name_bytes, name, trunc_name_len);
    spall_write(&ev, ev_size);
}

void spall_end_event(int tid) {
    double when = __rdtsc();
    SpallEndEvent ev = {
        .type = SpallEventType_End,
        .pid = 0,
        .tid = tid,
        .when = when
    };
    spall_write(&ev, sizeof(SpallEndEvent));
}

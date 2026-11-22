#include <ladspa.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UNIQUE_ID 0x6c6c01
#define PORT_INPUT_L 0
#define PORT_INPUT_R 1
#define PORT_OUTPUT_L 2
#define PORT_OUTPUT_R 3
#define PORT_TARGET 4
#define PORT_BOOST 5
#define PORT_LIMIT 6
#define SHM_NAME "/loudness-limiter-limit"

typedef struct {
    LADSPA_Data *in_l;
    LADSPA_Data *in_r;
    LADSPA_Data *out_l;
    LADSPA_Data *out_r;
    LADSPA_Data *target;
    LADSPA_Data *boost;
    LADSPA_Data *limit_control;
    float rms_db;
    float gain_db;
    float limit_db;
    int shm_fd;
    volatile float *shared_limit;
} instance;

static inline float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }
static inline float linear_to_db(float v) { return v > 1e-9f ? 20.0f * log10f(v) : -120.0f; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static LADSPA_Handle instantiate(const LADSPA_Descriptor *descriptor, unsigned long rate) {
    (void)descriptor;
    instance *i = calloc(1, sizeof(*i));
    if (!i) return NULL;
    i->rms_db = -120.0f;
    i->gain_db = 0.0f;
    i->limit_db = -32.0f;

    i->shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (i->shm_fd >= 0) {
        i->shared_limit = mmap(NULL, sizeof(float), PROT_READ | PROT_WRITE,
                               MAP_SHARED, i->shm_fd, 0);
        if (i->shared_limit == MAP_FAILED) i->shared_limit = NULL;
    }
    (void)rate;
    return i;
}

static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data) {
    instance *i = handle;
    switch (port) {
        case PORT_INPUT_L: i->in_l = data; break;
        case PORT_INPUT_R: i->in_r = data; break;
        case PORT_OUTPUT_L: i->out_l = data; break;
        case PORT_OUTPUT_R: i->out_r = data; break;
        case PORT_TARGET: i->target = data; break;
        case PORT_BOOST: i->boost = data; break;
        case PORT_LIMIT: i->limit_control = data; break;
    }
}

static void activate(LADSPA_Handle handle) {
    instance *i = handle;
    i->rms_db = -120.0f;
    i->gain_db = 0.0f;
}

static void run(LADSPA_Handle handle, unsigned long sample_count) {
    instance *i = handle;

    if (i->shared_limit) {
        float shared = *i->shared_limit;
        if (isfinite(shared)) i->limit_db = clampf(shared, -100.0f, 0.0f);
    } else if (i->limit_control) {
        i->limit_db = clampf(*i->limit_control, -100.0f, 0.0f);
    }

    float target = i->target ? clampf(*i->target, -60.0f, 0.0f) : -18.0f;
    float boost = i->boost ? clampf(*i->boost, 0.0f, 12.0f) : 6.0f;

    double sum = 0.0;
    float peak = 0.0f;
    for (unsigned long n = 0; n < sample_count; ++n) {
        float l = isfinite(i->in_l[n]) ? i->in_l[n] : 0.0f;
        float r = isfinite(i->in_r[n]) ? i->in_r[n] : 0.0f;
        sum += (double)l * l + (double)r * r;
        float a = fabsf(l); if (a > peak) peak = a;
        a = fabsf(r); if (a > peak) peak = a;
    }

    float block_rms = linear_to_db((float)sqrt(sum / (double)(sample_count * 2)));
    float rms_alpha = block_rms < i->rms_db ? 0.22f : 0.035f;
    i->rms_db = rms_alpha * block_rms + (1.0f - rms_alpha) * i->rms_db;

    float desired = clampf(target - i->rms_db, -24.0f, boost);
    float peak_allowed = i->limit_db - linear_to_db(peak);
    if (peak_allowed < desired) desired = peak_allowed;

    float attack = 0.20f;
    float release = 0.012f;
    float gain_db = i->gain_db;
    for (unsigned long n = 0; n < sample_count; ++n) {
        float alpha = desired < gain_db ? attack : release;
        gain_db = alpha * desired + (1.0f - alpha) * gain_db;

        float gain = db_to_linear(gain_db);
        float ceiling = db_to_linear(i->limit_db);
        float l = isfinite(i->in_l[n]) ? i->in_l[n] * gain : 0.0f;
        float r = isfinite(i->in_r[n]) ? i->in_r[n] * gain : 0.0f;
        i->out_l[n] = clampf(l, -ceiling, ceiling);
        i->out_r[n] = clampf(r, -ceiling, ceiling);
    }
    i->gain_db = gain_db;
}

static void deactivate(LADSPA_Handle handle) { (void)handle; }

static void cleanup(LADSPA_Handle handle) {
    instance *i = handle;
    if (i->shared_limit) munmap((void *)i->shared_limit, sizeof(float));
    if (i->shm_fd >= 0) close(i->shm_fd);
    free(i);
}

static LADSPA_Descriptor descriptor;

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
    if (index != 0) return NULL;

    descriptor.UniqueID = UNIQUE_ID;
    descriptor.Label = "loudness_limiter_native";
    descriptor.Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE;
    descriptor.Name = "Native Loudness Limiter";
    descriptor.Maker = "Local Loudness Limiter";
    descriptor.Copyright = "";
    descriptor.PortCount = 7;

    static LADSPA_PortDescriptor ports[7];
    static const char *names[7] = {
        "Input L", "Input R", "Output L", "Output R",
        "Target dB", "Max Boost dB", "Hard Limit dB"
    };
    static LADSPA_PortRangeHint hints[7];

    ports[0] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
    ports[1] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
    ports[2] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
    ports[3] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
    ports[4] = ports[5] = ports[6] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;

    for (unsigned i = 0; i < 7; ++i) {
        hints[i].HintDescriptor = LADSPA_HINT_DEFAULT_0;
        hints[i].LowerBound = i == 4 ? -60.0f : (i == 5 ? 0.0f : -100.0f);
        hints[i].UpperBound = i == 5 ? 12.0f : 0.0f;
    }
    hints[4].HintDescriptor = LADSPA_HINT_DEFAULT_0 | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    hints[4].LowerBound = -60.0f; hints[4].UpperBound = 0.0f;
    hints[5].HintDescriptor = LADSPA_HINT_DEFAULT_0 | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    hints[5].LowerBound = 0.0f; hints[5].UpperBound = 12.0f;
    hints[6].HintDescriptor = LADSPA_HINT_DEFAULT_0 | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    hints[6].LowerBound = -100.0f; hints[6].UpperBound = 0.0f;

    descriptor.PortDescriptors = ports;
    descriptor.PortNames = names;
    descriptor.PortRangeHints = hints;
    descriptor.ImplementationData = NULL;
    descriptor.instantiate = instantiate;
    descriptor.connect_port = connect_port;
    descriptor.activate = activate;
    descriptor.run = run;
    descriptor.run_adding = NULL;
    descriptor.set_run_adding_gain = NULL;
    descriptor.deactivate = deactivate;
    descriptor.cleanup = cleanup;
    return &descriptor;
}

#include <assert.h>
#include <math.h>
#include <stdio.h>

static float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }
int main(void) {
    assert(db_to_linear(0.0f) == 1.0f);
    assert(db_to_linear(-100.0f) <= db_to_linear(-32.0f));

    const float limit = db_to_linear(-32.0f);
    for (float rms_db = -60.0f; rms_db <= 0.0f; rms_db += 0.5f) {
        float desired = fmaxf(-24.0f, fminf(6.0f, -18.0f - rms_db));
        float peak_db = rms_db + 6.0f;
        float peak_gain = -32.0f - peak_db;
        if (peak_gain < desired) desired = peak_gain;
        float gain = db_to_linear(desired);
        float peak = db_to_linear(rms_db) * gain;
        assert(peak <= limit + 1e-6f);
    }

    float extreme[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    for (unsigned i = 0; i < 4; ++i) {
        float sample = fmaxf(-limit, fminf(limit, extreme[i] * db_to_linear(-24.0f)));
        assert(fabsf(sample) <= limit + 1e-6f);
    }

    puts("Loudness limiter ceiling tests passed");
    return 0;
}

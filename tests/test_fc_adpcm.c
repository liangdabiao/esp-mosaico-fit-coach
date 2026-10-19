// SPDX-License-Identifier: Apache-2.0
// tests/test_fc_adpcm.c —— C 解码器与 Python 参考编码器的一致性（固定向量）。
//
// 向量来自 tools/fit_coach/gen_voice.py 生成的 tests/fc_adpcm_fixture.h：
// 衰减正弦 + 满幅方波。断言 C 流式解码逐样本等于 Python 解码结果，
// 且正弦段的重建误差不超过生成器量出的上界。
#include <stdio.h>
#include <string.h>

#include "fc_adpcm.h"
#include "fc_adpcm_fixture.h"

static int failures;
#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok  %s\n", name);                                     \
        } else {                                                            \
            printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);        \
            failures++;                                                     \
        }                                                                   \
    } while (0)

int main(void)
{
    printf("test_fc_adpcm\n");

    CHECK(FC_ADPCM_FIXTURE_ENCODED_BYTES ==
              fc_adpcm_encoded_size(FC_ADPCM_FIXTURE_SAMPLES),
          "encoded size matches reference");

    int16_t out[FC_ADPCM_FIXTURE_SAMPLES];
    fc_adpcm_stream_t stream;
    fc_adpcm_stream_open(&stream, fc_adpcm_fixture_encoded,
                         FC_ADPCM_FIXTURE_SAMPLES);
    uint32_t got = fc_adpcm_stream_read(&stream, out, FC_ADPCM_FIXTURE_SAMPLES);
    CHECK(got == FC_ADPCM_FIXTURE_SAMPLES, "stream reads all samples");
    CHECK(fc_adpcm_stream_done(&stream), "stream done");
    CHECK(memcmp(out, fc_adpcm_fixture_expected, sizeof(out)) == 0,
          "C decode == python decode, sample-exact");

    int worst = 0;
    for (int i = 0; i < FC_ADPCM_FIXTURE_SINE; i++) {
        const int err = out[i] - fc_adpcm_fixture_source[i];
        if (err < 0 ? -err : err) { /* no-op 保号 */ }
        const int mag = err < 0 ? -err : err;
        if (mag > worst) worst = mag;
    }
    CHECK(worst <= FC_ADPCM_FIXTURE_WORST_SINE_ERROR,
          "sine worst error within bound");

    /* 分块读（模拟播放器 512 样本一块）与一次读完结果一致。 */
    fc_adpcm_stream_open(&stream, fc_adpcm_fixture_encoded,
                         FC_ADPCM_FIXTURE_SAMPLES);
    uint32_t filled = 0;
    while (filled < FC_ADPCM_FIXTURE_SAMPLES) {
        const uint32_t n = fc_adpcm_stream_read(&stream, out + filled, 7);
        if (n == 0) break;
        filled += n;
    }
    CHECK(filled == FC_ADPCM_FIXTURE_SAMPLES &&
              memcmp(out, fc_adpcm_fixture_expected, sizeof(out)) == 0,
          "chunked decode == full decode");

    /* 空流与 rewind。 */
    fc_adpcm_stream_open(&stream, fc_adpcm_fixture_encoded, 0);
    CHECK(fc_adpcm_stream_read(&stream, out, 4) == 0, "empty stream yields 0");

    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}

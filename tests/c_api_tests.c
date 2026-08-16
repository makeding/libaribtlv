#include <aribtlv/aribtlv.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct state {
    unsigned errors;
    int callback_valid;
};

static void on_error(void *opaque, const aribtlv_error *error)
{
    struct state *state = opaque;
    state->callback_valid = error != NULL && error->message != NULL;
    ++state->errors;
}

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void)
{
    aribtlv_callbacks callbacks;
    aribtlv_config config;
    struct state state = {0, 1};
    const uint8_t incomplete_tlv[] = {0x7f, 0x03, 0x00, 0x08, 0x00};

    CHECK(aribtlv_version() == ARIBTLV_VERSION_INT);
    CHECK(strcmp(aribtlv_version_string(), "0.4.0") == 0);
    CHECK(ARIBTLV_C_API_VERSION == 5);

    aribtlv_hlg_sdr_lut_info lut_info;
    CHECK(aribtlv_hlg_sdr_lut_describe(
              ARIBTLV_HLG_SDR_LUT_DISPLAY, &lut_info) == ARIBTLV_OK);
    CHECK(lut_info.dimension == 33U &&
          lut_info.rgb_float_count == 33U * 33U * 33U * 3U);
    aribtlv_hlg_sdr_lut_info prototype_info;
    CHECK(aribtlv_hlg_sdr_lut_describe(
              ARIBTLV_HLG_SDR_LUT_BT2446_PROTOTYPE, &prototype_info) == ARIBTLV_OK);
    CHECK(prototype_info.dimension == 128U &&
          prototype_info.rgb_float_count == 128U * 128U * 128U * 3U);
    const size_t lut_value_count = lut_info.rgb_float_count;
    float *lut = malloc(lut_value_count * sizeof(*lut));
    CHECK(lut != NULL);
    CHECK(aribtlv_hlg_sdr_lut_generate(
              ARIBTLV_HLG_SDR_LUT_DISPLAY, lut, lut_value_count) == ARIBTLV_OK);
    CHECK(lut[0] == 0.0F && lut[1] == 0.0F && lut[2] == 0.0F);
    const float one_step = 8.0F / 255.0F;
    CHECK(lut[3] == one_step &&
          lut[33U * 3U + 1U] == one_step &&
          lut[33U * 33U * 3U + 2U] == one_step);
    CHECK(lut[lut_value_count - 3] == 1.0F &&
          lut[lut_value_count - 2] == 1.0F &&
          lut[lut_value_count - 1] == 1.0F);
    lut[0] = 0.25F;
    CHECK(aribtlv_hlg_sdr_lut_generate(
              ARIBTLV_HLG_SDR_LUT_DISPLAY, lut, lut_value_count - 1) ==
          ARIBTLV_ERROR_BUFFER_TOO_SMALL);
    CHECK(lut[0] == 0.25F);
    free(lut);
    CHECK(aribtlv_hlg_sdr_lut_describe(
              (aribtlv_hlg_sdr_lut_profile)99, &lut_info) ==
          ARIBTLV_ERROR_INVALID_ARGUMENT);
    CHECK(aribtlv_hlg_sdr_lut_describe(
              ARIBTLV_HLG_SDR_LUT_DISPLAY, NULL) ==
          ARIBTLV_ERROR_INVALID_ARGUMENT);
    CHECK(aribtlv_hlg_sdr_lut_generate(
              ARIBTLV_HLG_SDR_LUT_DISPLAY, NULL, lut_value_count) ==
          ARIBTLV_ERROR_INVALID_ARGUMENT);

    aribtlv_callbacks_init(&callbacks);
    callbacks.on_error = on_error;
    aribtlv_config_init(&config);
    config.collect_application_resources = 0;

    aribtlv_demuxer *demuxer = aribtlv_demuxer_create(&config, &callbacks, &state);
    CHECK(demuxer != NULL);
    CHECK(aribtlv_demuxer_push(demuxer, incomplete_tlv, sizeof(incomplete_tlv)) ==
          ARIBTLV_OK);
    CHECK(aribtlv_demuxer_flush(demuxer) == ARIBTLV_OK);
    CHECK(state.errors > 0 && state.callback_valid);
    CHECK(aribtlv_demuxer_select_track(demuxer, (aribtlv_track_kind)99, 1) ==
          ARIBTLV_ERROR_INVALID_ARGUMENT);
    CHECK(aribtlv_demuxer_reset(demuxer) == ARIBTLV_OK);
    CHECK(aribtlv_demuxer_last_error(demuxer)[0] == '\0');
    aribtlv_demuxer_destroy(demuxer);

    aribtlv_duration_probe_options probe_options;
    aribtlv_duration_probe_options_init(&probe_options);
    probe_options.initial_range_size = 4;
    probe_options.max_range_size = 8;
    aribtlv_duration_probe *probe = aribtlv_duration_probe_create();
    CHECK(probe != NULL);
    CHECK(aribtlv_duration_probe_begin(probe, 16, &probe_options) == ARIBTLV_OK);
    CHECK(aribtlv_duration_probe_get_state(probe) == ARIBTLV_DURATION_PROBE_NEED_RANGE);
    aribtlv_range_request request;
    CHECK(aribtlv_duration_probe_next_range(probe, &request) == 1);
    CHECK(request.offset == 0 && request.length == 4);
    CHECK(aribtlv_duration_probe_fail_range(probe, request.request_id) == ARIBTLV_OK);
    CHECK(aribtlv_duration_probe_get_state(probe) == ARIBTLV_DURATION_PROBE_FAILED);
    CHECK(aribtlv_duration_probe_get_failure(probe) ==
          ARIBTLV_DURATION_PROBE_FAILURE_SOURCE_ERROR);
    aribtlv_duration_probe_destroy(probe);

    aribtlv_recording_scan_options scan_options;
    aribtlv_recording_scan_options_init(&scan_options);
    aribtlv_recording_scanner *scanner =
        aribtlv_recording_scanner_create(&scan_options);
    CHECK(scanner != NULL);
    aribtlv_recording_scanner_fail_source(scanner);
    aribtlv_recording_scan_result scan_result;
    CHECK(aribtlv_recording_scanner_finish(scanner, &scan_result) == ARIBTLV_OK);
    CHECK(scan_result.failure == ARIBTLV_RECORDING_SCAN_FAILURE_SOURCE_ERROR);
    aribtlv_recording_seek_result seek_result;
    CHECK(aribtlv_recording_scanner_seek_from_start(
              scanner, (aribtlv_timestamp){0, 1000000}, &seek_result) == 0);
    aribtlv_recording_scanner_destroy(scanner);

    CHECK(aribtlv_demuxer_push(NULL, NULL, 0) == ARIBTLV_ERROR_INVALID_ARGUMENT);
    return 0;
}

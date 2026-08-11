#include <aribtlv/aribtlv.h>

#include <stdint.h>
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
    CHECK(strcmp(aribtlv_version_string(), "0.1.0") == 0);

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

    CHECK(aribtlv_demuxer_push(NULL, NULL, 0) == ARIBTLV_ERROR_INVALID_ARGUMENT);
    return 0;
}

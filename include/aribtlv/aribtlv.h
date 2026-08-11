#ifndef ARIBTLV_ARIBTLV_H
#define ARIBTLV_ARIBTLV_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ARIBTLV_STATIC)
#    define ARIBTLV_API
#  elif defined(ARIBTLV_BUILDING_LIBRARY)
#    define ARIBTLV_API __declspec(dllexport)
#  else
#    define ARIBTLV_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define ARIBTLV_API __attribute__((visibility("default")))
#else
#  define ARIBTLV_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ARIBTLV_VERSION_MAJOR 0
#define ARIBTLV_VERSION_MINOR 1
#define ARIBTLV_VERSION_PATCH 1
#define ARIBTLV_VERSION_INT \
    ((ARIBTLV_VERSION_MAJOR << 16) | (ARIBTLV_VERSION_MINOR << 8) | ARIBTLV_VERSION_PATCH)
#define ARIBTLV_C_API_VERSION 1

typedef struct aribtlv_demuxer aribtlv_demuxer;

typedef enum aribtlv_result {
    ARIBTLV_OK = 0,
    ARIBTLV_ERROR_INVALID_ARGUMENT = -1,
    ARIBTLV_ERROR_OUT_OF_MEMORY = -2,
    ARIBTLV_ERROR_DEMUX = -3,
    ARIBTLV_ERROR_INTERNAL = -4
} aribtlv_result;

typedef enum aribtlv_codec {
    ARIBTLV_CODEC_HEVC = 0,
    ARIBTLV_CODEC_AAC_LATM = 1,
    ARIBTLV_CODEC_TTML = 2
} aribtlv_codec;

typedef enum aribtlv_track_kind {
    ARIBTLV_TRACK_VIDEO = 0,
    ARIBTLV_TRACK_AUDIO = 1,
    ARIBTLV_TRACK_SUBTITLE = 2
} aribtlv_track_kind;

typedef enum aribtlv_error_code {
    ARIBTLV_ERROR_MALFORMED_INPUT = 0,
    ARIBTLV_ERROR_UNSUPPORTED_FEATURE = 1,
    ARIBTLV_ERROR_DISCONTINUITY = 2,
    ARIBTLV_ERROR_RESOURCE_LIMIT = 3
} aribtlv_error_code;

typedef struct aribtlv_timestamp {
    int64_t value;
    uint32_t timescale;
} aribtlv_timestamp;

typedef struct aribtlv_service_info {
    uint32_t context_id;
    const uint8_t *package_id;
    size_t package_id_size;
} aribtlv_service_info;

typedef struct aribtlv_track_info {
    uint64_t track_id;
    uint32_t context_id;
    uint16_t packet_id;
    uint16_t component_tag;
    aribtlv_track_kind kind;
    aribtlv_codec codec;
    uint32_t timescale;
    const char *language;
    uint8_t has_audio;
    uint8_t audio_main_component;
    uint32_t audio_sample_rate;
    uint32_t audio_channels;
} aribtlv_track_info;

typedef struct aribtlv_access_unit {
    uint64_t track_id;
    aribtlv_codec codec;
    uint16_t component_tag;
    const uint8_t *data;
    size_t size;
    aribtlv_timestamp pts;
    aribtlv_timestamp dts;
    uint64_t restart_offset;
    uint64_t input_offset;
    uint8_t random_access;
    uint8_t discontinuity;
} aribtlv_access_unit;

typedef struct aribtlv_error {
    aribtlv_error_code code;
    uint64_t input_offset;
    uint8_t recoverable;
    const char *message;
} aribtlv_error;

/* Views passed to callbacks remain valid only until that callback returns. */
typedef struct aribtlv_callbacks {
    size_t struct_size;
    void (*on_service)(void *opaque, const aribtlv_service_info *service);
    void (*on_track)(void *opaque, const aribtlv_track_info *track);
    void (*on_track_removed)(void *opaque, const aribtlv_track_info *track);
    void (*on_access_unit)(void *opaque, const aribtlv_access_unit *unit);
    void (*on_error)(void *opaque, const aribtlv_error *error);
} aribtlv_callbacks;

typedef struct aribtlv_config {
    size_t struct_size;
    uint8_t collect_application_resources;
} aribtlv_config;

ARIBTLV_API uint32_t aribtlv_version(void);
ARIBTLV_API const char *aribtlv_version_string(void);
ARIBTLV_API void aribtlv_callbacks_init(aribtlv_callbacks *callbacks);
ARIBTLV_API void aribtlv_config_init(aribtlv_config *config);

ARIBTLV_API aribtlv_demuxer *aribtlv_demuxer_create(
    const aribtlv_config *config,
    const aribtlv_callbacks *callbacks,
    void *opaque);
ARIBTLV_API void aribtlv_demuxer_destroy(aribtlv_demuxer *demuxer);

ARIBTLV_API int aribtlv_demuxer_push(
    aribtlv_demuxer *demuxer, const uint8_t *data, size_t size);
ARIBTLV_API int aribtlv_demuxer_flush(aribtlv_demuxer *demuxer);
ARIBTLV_API int aribtlv_demuxer_reset(aribtlv_demuxer *demuxer);
ARIBTLV_API int aribtlv_demuxer_reposition(
    aribtlv_demuxer *demuxer, uint64_t input_offset, uint8_t preserve_timeline);
ARIBTLV_API int aribtlv_demuxer_select_service(
    aribtlv_demuxer *demuxer, uint32_t context_id);
ARIBTLV_API int aribtlv_demuxer_clear_service(aribtlv_demuxer *demuxer);
ARIBTLV_API int aribtlv_demuxer_select_track(
    aribtlv_demuxer *demuxer, aribtlv_track_kind kind, uint64_t track_id);
ARIBTLV_API int aribtlv_demuxer_clear_track(
    aribtlv_demuxer *demuxer, aribtlv_track_kind kind);
ARIBTLV_API int aribtlv_demuxer_set_subtitle_passthrough(
    aribtlv_demuxer *demuxer, uint8_t enabled);
ARIBTLV_API const char *aribtlv_demuxer_last_error(const aribtlv_demuxer *demuxer);

#ifdef __cplusplus
}
#endif

#endif

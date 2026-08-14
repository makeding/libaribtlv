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
#define ARIBTLV_VERSION_MINOR 4
#define ARIBTLV_VERSION_PATCH 0
#define ARIBTLV_VERSION_INT \
    ((ARIBTLV_VERSION_MAJOR << 16) | (ARIBTLV_VERSION_MINOR << 8) | ARIBTLV_VERSION_PATCH)
#define ARIBTLV_C_API_VERSION 4

typedef struct aribtlv_demuxer aribtlv_demuxer;
typedef struct aribtlv_duration_probe aribtlv_duration_probe;
typedef struct aribtlv_recording_scanner aribtlv_recording_scanner;

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

typedef enum aribtlv_discontinuity_reason {
    ARIBTLV_DISCONTINUITY_NONE = 0,
    ARIBTLV_DISCONTINUITY_SOURCE_DAMAGE = 1U << 0U,
    ARIBTLV_DISCONTINUITY_TRACK_SELECTION = 1U << 1U,
    ARIBTLV_DISCONTINUITY_REPOSITION = 1U << 2U,
    ARIBTLV_DISCONTINUITY_TIMELINE_NORMALIZATION = 1U << 3U
} aribtlv_discontinuity_reason;

typedef struct aribtlv_timestamp {
    int64_t value;
    uint32_t timescale;
} aribtlv_timestamp;

typedef struct aribtlv_service_info {
    uint32_t context_id;
    const uint8_t *package_id;
    size_t package_id_size;
} aribtlv_service_info;

typedef struct aribtlv_asset_group_info {
    uint8_t group_identification;
    uint8_t selection_level;
} aribtlv_asset_group_info;

typedef struct aribtlv_subtitle_info {
    uint8_t tag;
    uint8_t info_version;
    uint8_t type;
    uint8_t format;
    uint8_t operation_mode;
    uint8_t timing_mode;
    uint8_t display_mode;
    uint8_t resolution;
    uint8_t compression_type;
    uint8_t has_start_mpu_sequence_number;
    uint32_t start_mpu_sequence_number;
    uint8_t has_reference_start_ntp;
    uint64_t reference_start_ntp;
    uint8_t reference_start_time_leap_indicator;
} aribtlv_subtitle_info;

typedef struct aribtlv_subtitle_resource {
    uint8_t subsample_number;
    uint8_t data_type;
    const uint8_t *data;
    size_t size;
} aribtlv_subtitle_resource;

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
    uint8_t has_video;
    uint8_t video_has_hdr_wcg_idc;
    uint8_t video_hdr_wcg_idc;
    uint8_t video_has_transfer_characteristics;
    uint8_t video_transfer_characteristics;
    const aribtlv_asset_group_info *asset_groups;
    size_t asset_group_count;
    const aribtlv_subtitle_info *subtitle;
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
    uint8_t has_subtitle_timing_mode;
    uint8_t subtitle_timing_mode;
    uint8_t has_subtitle_operation_mode;
    uint8_t subtitle_operation_mode;
    uint8_t has_subtitle_display_mode;
    uint8_t subtitle_display_mode;
    uint8_t has_subtitle_compression_type;
    uint8_t subtitle_compression_type;
    uint8_t has_mpu_sequence_number;
    uint32_t mpu_sequence_number;
    uint8_t has_subtitle_reference_start_pts;
    aribtlv_timestamp subtitle_reference_start_pts;
    const aribtlv_subtitle_resource *subtitle_resources;
    size_t subtitle_resource_count;
    uint32_t discontinuity_reasons;
} aribtlv_access_unit;

typedef struct aribtlv_error {
    aribtlv_error_code code;
    uint64_t input_offset;
    uint8_t recoverable;
    const char *message;
} aribtlv_error;

typedef struct aribtlv_damage_span {
    uint64_t track_id;
    aribtlv_track_kind kind;
    aribtlv_codec codec;
    uint8_t has_start_time;
    aribtlv_timestamp start_time;
    aribtlv_timestamp end_time;
    uint8_t has_recovery_time;
    aribtlv_timestamp recovery_time;
    uint64_t start_input_offset;
    uint64_t end_input_offset;
    uint64_t recovery_input_offset;
    uint64_t recovery_restart_offset;
    uint32_t reasons;
    uint8_t recovered;
    uint8_t recovery_random_access;
} aribtlv_damage_span;

/* Views passed to callbacks remain valid only until that callback returns. */
typedef struct aribtlv_callbacks {
    size_t struct_size;
    void (*on_service)(void *opaque, const aribtlv_service_info *service);
    void (*on_track)(void *opaque, const aribtlv_track_info *track);
    void (*on_track_removed)(void *opaque, const aribtlv_track_info *track);
    void (*on_access_unit)(void *opaque, const aribtlv_access_unit *unit);
    void (*on_error)(void *opaque, const aribtlv_error *error);
    void (*on_damage)(void *opaque, const aribtlv_damage_span *damage);
} aribtlv_callbacks;

typedef struct aribtlv_config {
    size_t struct_size;
    uint8_t collect_application_resources;
} aribtlv_config;

typedef enum aribtlv_duration_probe_state {
    ARIBTLV_DURATION_PROBE_IDLE = 0,
    ARIBTLV_DURATION_PROBE_NEED_RANGE = 1,
    ARIBTLV_DURATION_PROBE_COMPLETE = 2,
    ARIBTLV_DURATION_PROBE_UNKNOWN = 3,
    ARIBTLV_DURATION_PROBE_FAILED = 4,
    ARIBTLV_DURATION_PROBE_CANCELLED = 5
} aribtlv_duration_probe_state;

typedef enum aribtlv_duration_probe_failure {
    ARIBTLV_DURATION_PROBE_FAILURE_NONE = 0,
    ARIBTLV_DURATION_PROBE_FAILURE_INVALID_SOURCE = 1,
    ARIBTLV_DURATION_PROBE_FAILURE_INVALID_RESPONSE = 2,
    ARIBTLV_DURATION_PROBE_FAILURE_SOURCE_ERROR = 3,
    ARIBTLV_DURATION_PROBE_FAILURE_NO_VIDEO = 4,
    ARIBTLV_DURATION_PROBE_FAILURE_NO_TAIL_TIMESTAMP = 5,
    ARIBTLV_DURATION_PROBE_FAILURE_RANGE_LIMIT = 6,
    ARIBTLV_DURATION_PROBE_FAILURE_PARSE_ERROR = 7
} aribtlv_duration_probe_failure;

typedef enum aribtlv_duration_status {
    ARIBTLV_DURATION_UNKNOWN = 0,
    ARIBTLV_DURATION_PROVISIONAL = 1,
    ARIBTLV_DURATION_COMPLETE = 2
} aribtlv_duration_status;

typedef struct aribtlv_duration_probe_options {
    size_t struct_size;
    uint64_t initial_range_size;
    uint64_t max_range_size;
    uint8_t has_service_context_id;
    uint32_t service_context_id;
    uint8_t has_video_packet_id;
    uint16_t video_packet_id;
} aribtlv_duration_probe_options;

typedef struct aribtlv_range_request {
    uint64_t generation;
    uint64_t request_id;
    uint64_t offset;
    uint64_t length;
} aribtlv_range_request;

typedef struct aribtlv_duration_info {
    aribtlv_timestamp value;
    aribtlv_duration_status status;
} aribtlv_duration_info;

typedef enum aribtlv_recording_scan_failure {
    ARIBTLV_RECORDING_SCAN_FAILURE_NONE = 0,
    ARIBTLV_RECORDING_SCAN_FAILURE_SOURCE_ERROR = 1,
    ARIBTLV_RECORDING_SCAN_FAILURE_NO_VIDEO = 2,
    ARIBTLV_RECORDING_SCAN_FAILURE_NO_RANDOM_ACCESS_POINT = 3,
    ARIBTLV_RECORDING_SCAN_FAILURE_PARSE_ERROR = 4
} aribtlv_recording_scan_failure;

typedef struct aribtlv_recording_scan_options {
    size_t struct_size;
    uint8_t has_service_context_id;
    uint32_t service_context_id;
    uint8_t has_video_packet_id;
    uint16_t video_packet_id;
} aribtlv_recording_scan_options;

typedef struct aribtlv_seek_point {
    aribtlv_timestamp presentation_time;
    uint64_t signalling_offset;
    uint64_t random_access_offset;
    uint64_t video_track_id;
    uint64_t bootstrap_id;
} aribtlv_seek_point;

/* Pointer fields remain valid until the scanner is destroyed. */
typedef struct aribtlv_recording_scan_result {
    aribtlv_recording_scan_failure failure;
    uint8_t has_error;
    aribtlv_error error;
    uint8_t has_video_track;
    uint64_t video_track_id;
    uint8_t has_video_packet_id;
    uint16_t video_packet_id;
    uint8_t has_first_presentation_time;
    aribtlv_timestamp first_presentation_time;
    uint8_t has_last_presentation_time;
    aribtlv_timestamp last_presentation_time;
    aribtlv_duration_info duration;
    const aribtlv_seek_point *seek_points;
    size_t seek_point_count;
} aribtlv_recording_scan_result;

typedef struct aribtlv_recording_seek_result {
    aribtlv_timestamp target_presentation_time;
    aribtlv_seek_point point;
} aribtlv_recording_seek_result;

ARIBTLV_API uint32_t aribtlv_version(void);
ARIBTLV_API const char *aribtlv_version_string(void);
ARIBTLV_API void aribtlv_callbacks_init(aribtlv_callbacks *callbacks);
ARIBTLV_API void aribtlv_config_init(aribtlv_config *config);
ARIBTLV_API void aribtlv_duration_probe_options_init(
    aribtlv_duration_probe_options *options);
ARIBTLV_API void aribtlv_recording_scan_options_init(
    aribtlv_recording_scan_options *options);

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

ARIBTLV_API aribtlv_duration_probe *aribtlv_duration_probe_create(void);
ARIBTLV_API void aribtlv_duration_probe_destroy(aribtlv_duration_probe *probe);
ARIBTLV_API int aribtlv_duration_probe_begin(
    aribtlv_duration_probe *probe, uint64_t source_size,
    const aribtlv_duration_probe_options *options);
ARIBTLV_API int aribtlv_duration_probe_next_range(
    const aribtlv_duration_probe *probe, aribtlv_range_request *request);
ARIBTLV_API int aribtlv_duration_probe_push_range(
    aribtlv_duration_probe *probe, uint64_t request_id, uint64_t absolute_offset,
    const uint8_t *data, size_t size, uint8_t end_of_range);
ARIBTLV_API int aribtlv_duration_probe_fail_range(
    aribtlv_duration_probe *probe, uint64_t request_id);
ARIBTLV_API void aribtlv_duration_probe_cancel(aribtlv_duration_probe *probe);
ARIBTLV_API aribtlv_duration_probe_state aribtlv_duration_probe_get_state(
    const aribtlv_duration_probe *probe);
ARIBTLV_API aribtlv_duration_probe_failure aribtlv_duration_probe_get_failure(
    const aribtlv_duration_probe *probe);
ARIBTLV_API int aribtlv_duration_probe_get_duration(
    const aribtlv_duration_probe *probe, aribtlv_duration_info *duration);
ARIBTLV_API uint64_t aribtlv_duration_probe_transferred_bytes(
    const aribtlv_duration_probe *probe);

ARIBTLV_API aribtlv_recording_scanner *aribtlv_recording_scanner_create(
    const aribtlv_recording_scan_options *options);
ARIBTLV_API void aribtlv_recording_scanner_destroy(
    aribtlv_recording_scanner *scanner);
ARIBTLV_API int aribtlv_recording_scanner_push(
    aribtlv_recording_scanner *scanner, const uint8_t *data, size_t size);
ARIBTLV_API void aribtlv_recording_scanner_fail_source(
    aribtlv_recording_scanner *scanner);
ARIBTLV_API int aribtlv_recording_scanner_finish(
    aribtlv_recording_scanner *scanner, aribtlv_recording_scan_result *result);
ARIBTLV_API int aribtlv_recording_scanner_seek_from_start(
    const aribtlv_recording_scanner *scanner, aribtlv_timestamp offset,
    aribtlv_recording_seek_result *result);

#ifdef __cplusplus
}
#endif

#endif

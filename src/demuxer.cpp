#include <tlvdemux/demuxer.hpp>

#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include "compressed_ip_parser.hpp"

namespace tlvdemux {

namespace {

class ForwardApplicationResourceSink final : public ApplicationResourceSink {
public:
    explicit ForwardApplicationResourceSink(Sink& sink) : sink_(sink) {}

    void onApplicationState(const ApplicationState& state) override {
        sink_.onApplicationState(state);
    }
    void onApplicationResource(ApplicationResource&& resource) override {
        sink_.onApplicationResource(std::move(resource));
    }
    void onApplicationResourcesReset() override {
        sink_.onApplicationResourcesReset();
    }
    void onApplicationResourceError(const Error& error) override {
        sink_.onError(error);
    }

private:
    Sink& sink_;
};

} // namespace

class Demuxer::Impl {
public:
    Impl(Sink& sink, Limits limits)
        : sink_(sink), limits_(std::move(limits)),
          application_sink_(sink_), application_resources_(application_sink_, limits_),
          ip_(limits_,
              [this](ServiceInfo info) { service(std::move(info)); },
              [this](TrackInfo info) { return track(std::move(info)); },
              [this](detail::TimedAccessUnit unit) { access_unit(std::move(unit)); },
              [this](ApplicationServiceInfo info) { application_service(std::move(info)); },
              [this](DataAssetInfo info) { data_asset(std::move(info)); },
              [this](DataUnit unit) { data_unit(std::move(unit)); },
              [this](SignallingMessage message) { signalling(std::move(message)); },
              [this](EventInfo info) { event_info(std::move(info)); },
              [this](ApplicationInfo info) { application(std::move(info)); },
              [this](DataTransmissionTable table) { data_transmission(std::move(table)); },
              [this](DataDirectoryTable table) { data_directory(std::move(table)); },
              [this](DataAssetManagementTable table) { data_asset_management(std::move(table)); },
              [this](const ErrorCode code, const std::uint64_t offset,
                     const bool recoverable, std::string message) {
                  error(code, offset, recoverable, std::move(message));
              }),
          tlv_(limits_,
               [this](const detail::TlvPacketView& packet) { ip_.consume(packet); },
               [this](const ErrorCode code, const std::uint64_t offset,
                      const bool recoverable, std::string message) {
                   error(code, offset, recoverable, std::move(message));
               }) {}

    void push(const std::uint8_t* data, const std::size_t size) {
        if (data == nullptr && size != 0) {
            error(ErrorCode::MalformedInput, 0, false,
                  "Demuxer::push received a null pointer with non-zero size");
            return;
        }
        tlv_.push(data, size);
        if (size > std::numeric_limits<std::uint64_t>::max() - input_end_offset_) {
            input_end_offset_ = std::numeric_limits<std::uint64_t>::max();
        } else {
            input_end_offset_ += size;
        }
    }

    void flush() {
        tlv_.flush();
        ip_.flush();
    }

    void reset() {
        tlv_.reset();
        ip_.reset();
        if (limits_.collect_application_resources) application_resources_.reset();
        services_.clear();
        current_tracks_.clear();
        application_services_.clear();
        data_assets_.clear();
        events_.clear();
        applications_.clear();
        data_transmission_tables_.clear();
        data_directory_versions_.clear();
        data_asset_management_versions_.clear();
        error_counts_.clear();
        origin_.reset();
        broadcast_clock_.reset();
        last_clock_mpu_sequence_.reset();
        reposition_epoch_ = 0;
        emitted_reposition_epochs_.clear();
        input_end_offset_ = 0;
        for (std::size_t index = 0; index < selected_tracks_.size(); ++index) {
            selection_boundaries_[index] = 0;
            selection_pending_[index] = selected_tracks_[index].has_value();
            selection_wait_for_rap_[index] =
                index == static_cast<std::size_t>(TrackKind::Video) &&
                selected_tracks_[index].has_value();
        }
    }

    void reposition(const RepositionOptions options) {
        tlv_.reset(options.input_offset);
        ip_.reset();
        if (!options.preserve_timeline) origin_.reset();
        if (!options.preserve_timeline) broadcast_clock_.reset();
        last_clock_mpu_sequence_.reset();
        input_end_offset_ = options.input_offset;
        for (std::size_t index = 0; index < selected_tracks_.size(); ++index) {
            selection_boundaries_[index] = options.input_offset;
            selection_pending_[index] = selected_tracks_[index].has_value();
            selection_wait_for_rap_[index] =
                index == static_cast<std::size_t>(TrackKind::Video) &&
                selected_tracks_[index].has_value();
        }
        ++reposition_epoch_;
        if (reposition_epoch_ == 0) ++reposition_epoch_;
    }

    void select_service(std::optional<std::uint32_t> context_id) {
        selected_service_ = context_id;
        ip_.select_service(context_id);
        if (limits_.collect_application_resources) application_resources_.reset();
        services_.clear();
        current_tracks_.clear();
        application_services_.clear();
        data_assets_.clear();
        events_.clear();
        applications_.clear();
        data_transmission_tables_.clear();
        data_directory_versions_.clear();
        data_asset_management_versions_.clear();
        origin_.reset();
        broadcast_clock_.reset();
        last_clock_mpu_sequence_.reset();
    }

    void select_track(const TrackKind kind, std::optional<std::uint64_t> track_id) {
        const auto index = static_cast<std::size_t>(kind);
        if (selected_tracks_[index] == track_id) return;
        selected_tracks_[index] = track_id;
        selection_boundaries_[index] = input_end_offset_;
        selection_pending_[index] = track_id.has_value();
        selection_wait_for_rap_[index] =
            kind == TrackKind::Video && track_id.has_value();
        if (kind == TrackKind::Video) last_clock_mpu_sequence_.reset();
    }

    std::optional<BroadcastClock> broadcast_clock() const { return broadcast_clock_; }

private:
    static bool same_track(const TrackInfo& left, const TrackInfo& right) {
        const auto same_audio = [](const std::optional<AudioInfo>& first,
                                   const std::optional<AudioInfo>& second) {
            if (first.has_value() != second.has_value()) return false;
            if (!first.has_value()) return true;
            return first->stream_content == second->stream_content &&
                   first->component_type == second->component_type &&
                   first->component_tag == second->component_tag &&
                   first->channel_layout == second->channel_layout &&
                   first->stream_type == second->stream_type &&
                   first->simulcast_group_tag == second->simulcast_group_tag &&
                   first->es_multi_lingual == second->es_multi_lingual &&
                   first->main_component == second->main_component &&
                   first->quality_indicator == second->quality_indicator &&
                   first->sampling_rate_code == second->sampling_rate_code &&
                   first->sample_rate == second->sample_rate &&
                   first->secondary_language == second->secondary_language;
        };
        const auto same_subtitle = [](const std::optional<SubtitleInfo>& first,
                                      const std::optional<SubtitleInfo>& second) {
            if (first.has_value() != second.has_value()) return false;
            if (!first.has_value()) return true;
            return first->tag == second->tag &&
                   first->info_version == second->info_version &&
                   first->type == second->type && first->format == second->format &&
                   first->operation_mode == second->operation_mode &&
                   first->timing_mode == second->timing_mode &&
                   first->display_mode == second->display_mode &&
                   first->resolution == second->resolution &&
                   first->compression_type == second->compression_type &&
                   first->start_mpu_sequence_number == second->start_mpu_sequence_number &&
                   first->reference_start_ntp == second->reference_start_ntp;
        };
        return left.track_id == right.track_id && left.context_id == right.context_id &&
               left.packet_id == right.packet_id && left.asset_id == right.asset_id &&
               left.kind == right.kind && left.codec == right.codec &&
               left.language == right.language && left.component_tag == right.component_tag &&
               left.timescale == right.timescale && same_audio(left.audio, right.audio) &&
               same_subtitle(left.subtitle, right.subtitle);
    }

    void service(ServiceInfo info) {
        if (selected_service_.has_value() && *selected_service_ != info.context_id) {
            return;
        }
        const auto found = services_.find(info.context_id);
        if (found != services_.end() && info.package_id.empty() && !found->second.empty()) {
            return;
        }
        if (found != services_.end() && found->second == info.package_id) {
            return;
        }
        services_[info.context_id] = info.package_id;
        sink_.onService(info);
    }

    std::uint64_t track(TrackInfo info) {
        if (selected_service_.has_value() && *selected_service_ != info.context_id) {
            return 0;
        }
        std::string identity;
        identity.reserve(6 + info.asset_id.size());
        for (const auto shift : {24U, 16U, 8U, 0U}) {
            identity.push_back(static_cast<char>((info.context_id >> shift) & 0xffU));
        }
        identity.push_back(static_cast<char>(info.packet_id >> 8U));
        identity.push_back(static_cast<char>(info.packet_id & 0xffU));
        identity.append(reinterpret_cast<const char*>(info.asset_id.data()), info.asset_id.size());

        auto id = track_ids_.find(identity);
        if (id == track_ids_.end()) {
            id = track_ids_.emplace(std::move(identity), next_track_id_++).first;
        }
        info.track_id = id->second;
        const auto current = current_tracks_.find(info.track_id);
        if (current != current_tracks_.end() && same_track(current->second, info)) {
            return info.track_id;
        }
        current_tracks_[info.track_id] = info;
        sink_.onTrack(info);
        return info.track_id;
    }

    void application_service(ApplicationServiceInfo info) {
        if (selected_service_.has_value() && *selected_service_ != info.context_id) return;
        const auto key = std::to_string(info.context_id) + ':' +
            std::to_string(info.ait_packet_id.value_or(0xffffU)) + ':' +
            std::to_string(info.data_transmission_packet_id.value_or(0xffffU));
        const auto found = application_services_.find(key);
        if (found != application_services_.end() &&
            found->second.application_format == info.application_format &&
            found->second.document_resolution == info.document_resolution &&
            found->second.default_ait == info.default_ait &&
            found->second.has_data_transmission_messages == info.has_data_transmission_messages) {
            return;
        }
        application_services_[key] = info;
        sink_.onApplicationService(info);
    }

    void data_asset(DataAssetInfo info) {
        if (selected_service_.has_value() && *selected_service_ != info.context_id) return;
        std::string key = std::to_string(info.context_id) + ':' + std::to_string(info.packet_id) + ':';
        key.append(reinterpret_cast<const char*>(info.asset_id.data()), info.asset_id.size());
        const auto found = data_assets_.find(key);
        if (found != data_assets_.end() && found->second.asset_type == info.asset_type &&
            found->second.component_tag == info.component_tag) {
            return;
        }
        data_assets_[key] = info;
        sink_.onDataAsset(info);
    }

    void data_unit(DataUnit unit) {
        if (selected_service_.has_value() && *selected_service_ != unit.context_id) return;
        application_resources_.onDataUnit(unit);
        sink_.onDataUnit(std::move(unit));
    }

    void signalling(SignallingMessage message) {
        if (selected_service_.has_value() && *selected_service_ != message.context_id) return;
        sink_.onSignallingMessage(std::move(message));
    }

    void event_info(EventInfo info) {
        if (selected_service_.has_value() && *selected_service_ != info.context_id) return;
        const auto key = std::to_string(info.context_id) + ':' +
            std::to_string(info.table_id) + ':' + std::to_string(info.service_id) + ':' +
            std::to_string(info.section_number) + ':' + std::to_string(info.event_id);
        const auto found = events_.find(key);
        if (found != events_.end() && found->second.version == info.version &&
            found->second.start_time_unix_milliseconds == info.start_time_unix_milliseconds &&
            found->second.duration_seconds == info.duration_seconds &&
            found->second.title == info.title && found->second.description == info.description) {
            return;
        }
        events_[key] = info;
        sink_.onEventInfo(info);
    }

    void application(ApplicationInfo info) {
        if (selected_service_.has_value() && *selected_service_ != info.context_id) return;
        const auto key = std::to_string(info.context_id) + ':' +
            std::to_string(info.application_type) + ':' +
            std::to_string(info.organization_id) + ':' + std::to_string(info.application_id);
        const auto found = applications_.find(key);
        if (found != applications_.end() && found->second.version == info.version &&
            found->second.control_code == info.control_code &&
            found->second.entry_path == info.entry_path &&
            found->second.transport_urls == info.transport_urls) {
            return;
        }
        applications_[key] = info;
        sink_.onApplication(info);
        application_resources_.onApplication(info);
    }

    void data_transmission(DataTransmissionTable table) {
        if (selected_service_.has_value() && *selected_service_ != table.context_id) return;
        const auto key = std::to_string(table.context_id) + ':' +
            std::to_string(table.table_id) + ':' + std::to_string(table.session_id) + ':' +
            std::to_string(table.section_number);
        const auto found = data_transmission_tables_.find(key);
        if (found != data_transmission_tables_.end() && found->second.version == table.version &&
            found->second.data == table.data) {
            return;
        }
        data_transmission_tables_[key] = table;
        sink_.onDataTransmissionTable(std::move(table));
    }

    void data_directory(DataDirectoryTable table) {
        if (selected_service_.has_value() && *selected_service_ != table.context_id) return;
        const auto key = std::to_string(table.context_id) + ':' +
            std::to_string(table.session_id) + ':' + std::to_string(table.section_number);
        const auto found = data_directory_versions_.find(key);
        if (found != data_directory_versions_.end() && found->second == table.version) return;
        data_directory_versions_[key] = table.version;
        sink_.onDataDirectoryTable(table);
        application_resources_.onDataDirectoryTable(table);
    }

    void data_asset_management(DataAssetManagementTable table) {
        if (selected_service_.has_value() && *selected_service_ != table.context_id) return;
        const auto key = std::to_string(table.context_id) + ':' +
            std::to_string(table.session_id) + ':' + std::to_string(table.section_number);
        const auto found = data_asset_management_versions_.find(key);
        if (found != data_asset_management_versions_.end() && found->second == table.version) return;
        data_asset_management_versions_[key] = table.version;
        sink_.onDataAssetManagementTable(table);
        application_resources_.onDataAssetManagementTable(table);
    }

    static bool ntp_delta_ticks(const std::uint64_t current, const std::uint64_t origin,
                                const std::uint32_t timescale, std::int64_t& output) {
        const bool negative = current < origin;
        const auto difference = negative ? origin - current : current - origin;
        const auto seconds = difference >> 32U;
        const auto fraction = static_cast<std::uint32_t>(difference);
        if (timescale != 0 && seconds >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / timescale) {
            return false;
        }
        const auto whole_ticks = seconds * timescale;
        const auto fractional_ticks =
            (static_cast<std::uint64_t>(fraction) * timescale) >> 32U;
        if (whole_ticks > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                              fractional_ticks) {
            return false;
        }
        const auto magnitude = static_cast<std::int64_t>(whole_ticks + fractional_ticks);
        output = negative ? -magnitude : magnitude;
        return true;
    }

    static bool rescale_offset(const std::int64_t value, const std::uint32_t source_scale,
                               const std::uint32_t target_scale, std::int64_t& output) {
        if (source_scale == 0) return false;
        const bool negative = value < 0;
        const auto magnitude = negative
            ? static_cast<std::uint64_t>(-(value + 1)) + 1U
            : static_cast<std::uint64_t>(value);
        const auto whole = magnitude / source_scale;
        const auto remainder = magnitude % source_scale;
        if (target_scale != 0 && whole >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / target_scale) {
            return false;
        }
        const auto scaled = whole * target_scale + remainder * target_scale / source_scale;
        if (scaled > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        output = negative ? -static_cast<std::int64_t>(scaled) : static_cast<std::int64_t>(scaled);
        return true;
    }

    static bool add_checked(const std::int64_t left, const std::int64_t right,
                            std::int64_t& output) {
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
            return false;
        }
        output = left + right;
        return true;
    }

    static bool subtract_checked(const std::int64_t left, const std::int64_t right,
                                 std::int64_t& output) {
        if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
            (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
            return false;
        }
        output = left - right;
        return true;
    }

    void access_unit(detail::TimedAccessUnit timed) {
        auto& unit = timed.unit;
        const auto track_info = current_tracks_.find(unit.track_id);
        if (unit.track_id == 0 || track_info == current_tracks_.end()) return;
        const auto kind_index = static_cast<std::size_t>(track_info->second.kind);
        if (selected_tracks_[kind_index].has_value() &&
            *selected_tracks_[kind_index] != unit.track_id) {
            return;
        }
        if (selection_pending_[kind_index]) {
            if (unit.input_offset < selection_boundaries_[kind_index]) return;
            if (selection_wait_for_rap_[kind_index] && !unit.random_access) return;
            unit.discontinuity = true;
            selection_pending_[kind_index] = false;
            selection_wait_for_rap_[kind_index] = false;
        }
        if (unit.pts.timescale == 0 || unit.dts.timescale != unit.pts.timescale) {
            error(ErrorCode::MalformedInput, unit.input_offset, true,
                  "access unit has an invalid or inconsistent timescale");
            return;
        }
        const auto presentation_offset = unit.pts;
        if (!origin_.has_value()) {
            origin_ = TimelineOrigin{timed.source_ntp_raw, unit.pts.value, unit.pts.timescale};
        }

        std::int64_t ntp_ticks = 0;
        std::int64_t origin_offset = 0;
        if (!ntp_delta_ticks(timed.source_ntp_raw, origin_->ntp, unit.pts.timescale, ntp_ticks) ||
            !rescale_offset(origin_->pts_offset, origin_->timescale,
                            unit.pts.timescale, origin_offset)) {
            error(ErrorCode::Discontinuity, unit.input_offset, true,
                  "timestamp normalization overflowed");
            return;
        }
        std::int64_t normalized_pts = 0;
        std::int64_t normalized_dts = 0;
        std::int64_t base = 0;
        if (!subtract_checked(ntp_ticks, origin_offset, base) ||
            !add_checked(base, unit.pts.value, normalized_pts) ||
            !add_checked(base, unit.dts.value, normalized_dts)) {
            error(ErrorCode::Discontinuity, unit.input_offset, true,
                  "normalized access-unit timestamp is out of range");
            return;
        }
        unit.pts.value = normalized_pts;
        unit.dts.value = normalized_dts;

        if (track_info->second.kind == TrackKind::Video && unit.source_ntp.has_value()) {
            std::int64_t offset_us = 0;
            std::int64_t broadcast_us = 0;
            if (rescale_offset(presentation_offset.value, presentation_offset.timescale,
                               1000000, offset_us) &&
                add_checked(unit.source_ntp->value, offset_us, broadcast_us)) {
                broadcast_clock_ = BroadcastClock{
                    unit.pts,
                    Timestamp{broadcast_us, 1000000},
                    unit.input_offset,
                    unit.discontinuity,
                };
                if (!last_clock_mpu_sequence_.has_value() ||
                    last_clock_mpu_sequence_ != unit.mpu_sequence_number ||
                    unit.discontinuity) {
                    last_clock_mpu_sequence_ = unit.mpu_sequence_number;
                    sink_.onBroadcastClock(*broadcast_clock_);
                }
            } else {
                error(ErrorCode::Discontinuity, unit.input_offset, true,
                      "broadcast clock mapping overflowed");
            }
        }
        if (track_info->second.subtitle &&
            track_info->second.subtitle->reference_start_ntp) {
            std::int64_t reference_ticks = 0;
            std::int64_t reference_pts = 0;
            if (ntp_delta_ticks(*track_info->second.subtitle->reference_start_ntp,
                                origin_->ntp, unit.pts.timescale, reference_ticks) &&
                subtract_checked(reference_ticks, origin_offset, reference_pts)) {
                unit.subtitle_reference_start_pts =
                    Timestamp{reference_pts, unit.pts.timescale};
            } else {
                unit.discontinuity = true;
                error(ErrorCode::Discontinuity, unit.input_offset, true,
                      "subtitle reference timestamp normalization overflowed");
            }
        }
        if (reposition_epoch_ != 0 &&
            emitted_reposition_epochs_[unit.track_id] != reposition_epoch_) {
            unit.discontinuity = true;
            emitted_reposition_epochs_[unit.track_id] = reposition_epoch_;
        }
        sink_.onAccessUnit(std::move(unit));
    }

    void error(const ErrorCode code, const std::uint64_t offset, const bool recoverable,
               std::string message) {
        const auto key = std::to_string(static_cast<int>(code)) + ':' + message;
        auto& count = error_counts_[key];
        ++count;
        const bool power_of_two = (count & (count - 1U)) == 0;
        if (power_of_two) {
            sink_.onError(Error{code, offset, recoverable, std::move(message)});
        }
    }

    Sink& sink_;
    Limits limits_;
    ForwardApplicationResourceSink application_sink_;
    ApplicationResourceAssembler application_resources_;
    detail::CompressedIpParser ip_;
    detail::TlvParser tlv_;
    std::optional<std::uint32_t> selected_service_;
    std::array<std::optional<std::uint64_t>, 3> selected_tracks_{};
    std::array<std::uint64_t, 3> selection_boundaries_{};
    std::array<bool, 3> selection_pending_{};
    std::array<bool, 3> selection_wait_for_rap_{};
    std::uint64_t input_end_offset_ = 0;
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> services_;
    std::unordered_map<std::string, std::uint64_t> track_ids_;
    std::unordered_map<std::uint64_t, TrackInfo> current_tracks_;
    std::unordered_map<std::string, ApplicationServiceInfo> application_services_;
    std::unordered_map<std::string, DataAssetInfo> data_assets_;
    std::unordered_map<std::string, EventInfo> events_;
    std::unordered_map<std::string, ApplicationInfo> applications_;
    std::unordered_map<std::string, DataTransmissionTable> data_transmission_tables_;
    std::unordered_map<std::string, std::uint8_t> data_directory_versions_;
    std::unordered_map<std::string, std::uint8_t> data_asset_management_versions_;
    std::uint64_t next_track_id_ = 1;
    std::unordered_map<std::string, std::uint64_t> error_counts_;
    std::uint64_t reposition_epoch_ = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> emitted_reposition_epochs_;
    struct TimelineOrigin {
        std::uint64_t ntp = 0;
        std::int64_t pts_offset = 0;
        std::uint32_t timescale = 1;
    };
    std::optional<TimelineOrigin> origin_;
    std::optional<BroadcastClock> broadcast_clock_;
    std::optional<std::uint32_t> last_clock_mpu_sequence_;
};

Demuxer::Demuxer(Sink& sink) : Demuxer(sink, Limits{}) {}

Demuxer::Demuxer(Sink& sink, Limits limits)
    : impl_(std::make_unique<Impl>(sink, std::move(limits))) {}

Demuxer::~Demuxer() = default;
Demuxer::Demuxer(Demuxer&&) noexcept = default;
Demuxer& Demuxer::operator=(Demuxer&&) noexcept = default;

void Demuxer::push(const std::uint8_t* data, const std::size_t size) {
    impl_->push(data, size);
}

void Demuxer::flush() {
    impl_->flush();
}

void Demuxer::reset() {
    impl_->reset();
}

void Demuxer::reposition(const RepositionOptions options) {
    impl_->reposition(options);
}

void Demuxer::selectService(std::optional<std::uint32_t> context_id) {
    impl_->select_service(context_id);
}

void Demuxer::selectTrack(const TrackKind kind, std::optional<std::uint64_t> track_id) {
    impl_->select_track(kind, track_id);
}

std::optional<BroadcastClock> Demuxer::broadcastClock() const {
    return impl_->broadcast_clock();
}

} // namespace tlvdemux

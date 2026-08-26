#include "demuxer_test_support.hpp"

    add_video(12, false, {0, 0, 0, 2, 0x46, 0x01});
    demuxer.flush();

    check(std::count_if(sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
              return unit.codec == aribtlv::Codec::Hevc;
          }) == 1,
          "track selection retained stale fragmented media or failed to resume at a fresh RAP");
}

void test_fragmented_signalling_restart_offset() {
    const auto pa = discovery_message();
    const auto first_end = pa.size() / 3;
    const auto middle_end = first_end * 2;
    const auto prefix = tlv(0xff, {});
    auto stream = prefix;
    const auto first = signalling_tlv(
        10, 0x40,
        std::vector<std::uint8_t>(pa.begin(),
                                  pa.begin() + static_cast<std::ptrdiff_t>(first_end)));
    const auto middle = signalling_tlv(
        11, 0x80,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(first_end),
                                  pa.begin() + static_cast<std::ptrdiff_t>(middle_end)));
    const auto last = signalling_tlv(
        12, 0xc0,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(middle_end),
                                  pa.end()));
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), middle.begin(), middle.end());
    stream.insert(stream.end(), last.begin(), last.end());

    auto add_video = [&](const std::uint32_t sequence, const bool rap,
                         const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, rap, mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_video(1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_video(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_video(3, false, {0, 0, 0, 2, 0x46, 0x01});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    const auto video = std::find_if(sink.access_units.begin(), sink.access_units.end(),
                                    [](const auto& unit) {
                                        return unit.codec == aribtlv::Codec::Hevc;
                                    });
    check(video != sink.access_units.end() && video->restart_offset == prefix.size() &&
              video->input_offset > video->restart_offset,
          "AU restart offset did not retain the first fragmented signalling packet");
}

void test_reposition_preserves_timeline_and_absolute_offsets() {
    auto initial = discovery_stream();
    append_video_access_unit(initial, 1);

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(initial.data(), initial.size());
    demuxer.flush();
    const auto initial_video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(initial_video != sink.access_units.end() && initial_video->pts.value == 0,
          "initial video did not establish the recording timeline");
    const auto initial_video_index =
        static_cast<std::size_t>(initial_video - sink.access_units.begin());
    const auto original_track_callbacks = sink.tracks.size();

    auto shifted_pa = discovery_message();
    const std::vector<std::uint8_t> timestamp_pattern{0x00, 0x01, 0x0c, 0, 0, 0, 1};
    const auto video_timestamp = std::search(shifted_pa.begin(), shifted_pa.end(),
                                             timestamp_pattern.begin(), timestamp_pattern.end());
    check(video_timestamp != shifted_pa.end(), "shifted fixture has no video timestamp");
    const auto ntp_index = static_cast<std::size_t>(video_timestamp - shifted_pa.begin()) + 7;
    shifted_pa[ntp_index + 0] = 0;
    shifted_pa[ntp_index + 1] = 0;
    shifted_pa[ntp_index + 2] = 0;
    shifted_pa[ntp_index + 3] = 101;
    shifted_pa[ntp_index + 4] = 0;
    shifted_pa[ntp_index + 5] = 0;
    shifted_pa[ntp_index + 6] = 0;
    shifted_pa[ntp_index + 7] = 0;

    auto shifted = signalling_tlv(100, 0, shifted_pa);
    const auto repeated = signalling_tlv(101, 0, shifted_pa);
    const auto latest_checkpoint_offset = static_cast<std::uint64_t>(shifted.size());
    shifted.insert(shifted.end(), repeated.begin(), repeated.end());
    append_video_access_unit(shifted, 1000);

    constexpr std::uint64_t source_offset = 500000;
    demuxer.reposition(aribtlv::RepositionOptions{source_offset, true});
    demuxer.push(shifted.data(), shifted.size());
    demuxer.flush();

    const auto second_video = std::find_if(
        sink.access_units.begin() + static_cast<std::ptrdiff_t>(initial_video_index + 1),
        sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(second_video != sink.access_units.end() && second_video->pts.value == 180000,
          "reposition reset the recording timeline instead of preserving it");
    check(second_video->restart_offset == source_offset + latest_checkpoint_offset &&
              second_video->input_offset > second_video->restart_offset,
          "reposition did not preserve absolute source offsets");
    check(second_video->discontinuity,
          "first access unit after reposition was not marked discontinuous");
    check(second_video->discontinuity_reasons ==
              aribtlv::DiscontinuityReason::Reposition &&
              sink.damage_spans.empty(),
          "reposition was incorrectly reported as damaged source media");
    check(sink.tracks.size() == original_track_callbacks,
          "reposition re-emitted unchanged track metadata");
}

void test_track_selection_preserves_timeline_and_waits_for_rap() {
    auto initial = discovery_stream();
    append_video_access_unit(initial, 1);

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(initial.data(), initial.size());
    demuxer.flush();
    const auto initial_video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(initial_video != sink.access_units.end() && initial_video->pts.value == 0,
          "initial video did not establish the track-switch timeline");
    const auto initial_video_index =
        static_cast<std::size_t>(initial_video - sink.access_units.begin());
    const auto video_track = initial_video->track_id;

    demuxer.selectTrack(aribtlv::TrackKind::Video, video_track);

    auto shifted_pa = discovery_message();
    const std::vector<std::uint8_t> timestamp_pattern{0x00, 0x01, 0x0c, 0, 0, 0, 1};
    const auto video_timestamp = std::search(shifted_pa.begin(), shifted_pa.end(),
                                             timestamp_pattern.begin(), timestamp_pattern.end());
    check(video_timestamp != shifted_pa.end(), "track-switch fixture has no video timestamp");
    const auto ntp_index = static_cast<std::size_t>(video_timestamp - shifted_pa.begin()) + 7;
    shifted_pa[ntp_index + 0] = 0;
    shifted_pa[ntp_index + 1] = 0;
    shifted_pa[ntp_index + 2] = 0;
    shifted_pa[ntp_index + 3] = 101;
    shifted_pa[ntp_index + 4] = 0;
    shifted_pa[ntp_index + 5] = 0;
    shifted_pa[ntp_index + 6] = 0;
    shifted_pa[ntp_index + 7] = 0;

    auto shifted = signalling_tlv(100, 0, shifted_pa);
    append_video_access_unit(shifted, 1000);
    demuxer.push(shifted.data(), shifted.size());
    demuxer.flush();

    const auto selected_video = std::find_if(
        sink.access_units.begin() + static_cast<std::ptrdiff_t>(initial_video_index + 1),
        sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(selected_video != sink.access_units.end() &&
              selected_video->track_id == video_track &&
              selected_video->pts.value == 180000 &&
              selected_video->random_access && selected_video->discontinuity,
          "video track selection reset the timeline or did not resume at a discontinuous RAP");
    check(aribtlv::hasDiscontinuityReason(
              selected_video->discontinuity_reasons,
              aribtlv::DiscontinuityReason::TrackSelection),
          "track selection did not retain its controlled discontinuity reason");
}



} // namespace

int main() {
    test_split_at_every_boundary();
    test_one_byte_input();
    test_garbage_recovery();
    test_service_selection_and_reset();
    test_incomplete_flush();
    test_mode_60_and_resource_limit();
    test_signalling_fragmentation_aggregation_and_m2();
    test_global_packet_state_budget();
    test_track_discovery_and_deduplication();
    test_track_discovery_pq_signal();
    test_truncated_subtitle_reference_start_time_is_rejected();
    test_service_selection_clears_layout_state();
    test_application_and_data_transmission_signalling();
    test_mpt_snapshot_removes_missing_service_state();
    test_mh_ait_snapshot_completion_empty_and_reposition();
    test_service_state_reset_notifications();
    test_independent_m2_sdt_and_tot();
    test_mh_eit_program_events();
    test_mh_eit_hdr_programme_icon();
    test_emt_stream_events();
    test_viewer_participation_notifications();
    test_dynamic_audio_layout_metadata();
    test_authenticated_mmtp_payload_bounds();
    test_codec_output_and_timeline();
    test_recording_scanner_uses_demux_metadata_and_bounds_time();
    test_timestamp_overflow_rejection();
    test_track_selection_clears_incomplete_media();
    test_fragmented_signalling_restart_offset();
    test_reposition_preserves_timeline_and_absolute_offsets();
    test_track_selection_preserves_timeline_and_waits_for_rap();
    std::cout << "all tests passed\n";
    return 0;
}


} // namespace

int main() {
    test_split_at_every_boundary();
    test_one_byte_input();
    test_garbage_recovery();
    test_service_selection_and_reset();
    test_incomplete_flush();
    test_mode_60_and_resource_limit();
    test_signalling_fragmentation_aggregation_and_m2();
    test_global_packet_state_budget();
    test_track_discovery_and_deduplication();
    test_track_discovery_pq_signal();
    test_truncated_subtitle_reference_start_time_is_rejected();
    test_service_selection_clears_layout_state();
    test_application_and_data_transmission_signalling();
    test_mpt_snapshot_removes_missing_service_state();
    test_mh_ait_snapshot_completion_empty_and_reposition();
    test_service_state_reset_notifications();
    test_independent_m2_sdt_and_tot();
    test_mh_eit_program_events();
    test_mh_eit_hdr_programme_icon();
    test_emt_stream_events();
    test_viewer_participation_notifications();
    test_dynamic_audio_layout_metadata();
    test_authenticated_mmtp_payload_bounds();
    test_codec_output_and_timeline();
    test_recording_scanner_uses_demux_metadata_and_bounds_time();
    test_timestamp_overflow_rejection();
    test_track_selection_clears_incomplete_media();
    test_fragmented_signalling_restart_offset();
    test_reposition_preserves_timeline_and_absolute_offsets();
    test_track_selection_preserves_timeline_and_waits_for_rap();
    test_video_presentation_hint();
    std::cout << "all tests passed\n";
    return 0;
}


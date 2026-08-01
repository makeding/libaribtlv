#include <tlvdemux/playback.hpp>
#include <tlvdemux/recording.hpp>
#include <tlvdemux/duration_probe.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void check(const bool condition, const std::string& message) {
    if (!condition) fail(message);
}

tlvdemux::AccessUnit video_unit(const std::int64_t pts_us,
                                const std::uint64_t input_offset,
                                const std::uint64_t restart_offset,
                                const bool random_access) {
    tlvdemux::AccessUnit unit;
    unit.track_id = 7;
    unit.codec = tlvdemux::Codec::Hevc;
    unit.pts = tlvdemux::Timestamp{pts_us, 1000000};
    unit.dts = unit.pts;
    unit.input_offset = input_offset;
    unit.restart_offset = restart_offset;
    unit.random_access = random_access;
    return unit;
}

void test_seek_policy_selection() {
    tlvdemux::SourceCapabilities range;
    range.random_access = true;
    range.size_known = true;
    check(tlvdemux::chooseSeekPolicy(range, true, false) ==
              tlvdemux::SeekPolicy::IndexedRandomAccess,
          "usable random-access index did not select indexed seeking");
    check(tlvdemux::chooseSeekPolicy(range, false, false) ==
              tlvdemux::SeekPolicy::AdaptiveRangeProbe,
          "unindexed random-access source did not select adaptive probing");

    tlvdemux::SourceCapabilities sequential;
    check(tlvdemux::chooseSeekPolicy(sequential, false, true) ==
              tlvdemux::SeekPolicy::BufferedOnly,
          "sequential buffered source did not select buffered seeking");
    check(tlvdemux::chooseSeekPolicy(sequential, false, false) ==
              tlvdemux::SeekPolicy::Unsupported,
          "sequential unbuffered source claimed seek support");
}

void test_playback_state_machine() {
    tlvdemux::SourceCapabilities capabilities;
    capabilities.random_access = true;
    capabilities.size_known = true;
    capabilities.growing = true;
    const auto policy = tlvdemux::chooseSeekPolicy(capabilities, false, false);

    tlvdemux::PlaybackStateMachine state;
    check(state.beginOpen(capabilities, policy) &&
              state.sessionState() == tlvdemux::SessionState::Opening,
          "session did not enter Opening");
    const auto session = state.sessionId();
    check(state.completeOpen() && state.sessionState() == tlvdemux::SessionState::Ready,
          "session did not enter Ready");
    check(state.startIndexBuilding() &&
              state.indexState() == tlvdemux::IndexState::Building,
          "index did not start building");
    check(state.reachIndexEnd(true) &&
              state.indexState() == tlvdemux::IndexState::Following,
          "growing index did not enter Following");
    check(state.reachGrowingEnd() &&
              state.sessionState() == tlvdemux::SessionState::WaitingForData,
          "growing source EOF did not enter WaitingForData");
    check(state.sourceGrew() && state.sessionState() == tlvdemux::SessionState::Ready &&
              state.indexState() == tlvdemux::IndexState::Building,
          "source growth did not resume session and index");
    check(state.reachIndexEnd(true), "following index did not reach readable end again");
    check(state.sourceFinalized(false) &&
              state.indexState() == tlvdemux::IndexState::Complete,
          "source finalization did not complete the following index");
    check(state.reachFinalEnd() && state.sessionState() == tlvdemux::SessionState::Ended,
          "final EOF did not enter Ended");

    const auto first_seek = state.requestSeek();
    check(first_seek.has_value() && state.sessionState() == tlvdemux::SessionState::Ready &&
              state.seekState() == tlvdemux::SeekState::Resolving,
          "seek from Ended did not reopen the session transaction");
    check(state.beginReposition(*first_seek) && state.beginPriming(*first_seek) &&
              state.beginPreroll(*first_seek) && state.beginLanding(*first_seek) &&
              state.completeSeek(*first_seek),
          "exact-seek state path was rejected");
    check(state.accepts(session, *first_seek),
          "current session/generation was not accepted after landing");

    const auto old_seek = state.requestSeek();
    const auto new_seek = state.requestSeek();
    check(old_seek.has_value() && new_seek.has_value() && *new_seek > *old_seek,
          "new seek did not advance the generation");
    check(!state.beginReposition(*old_seek) && state.beginReposition(*new_seek),
          "stale seek generation changed state");
    check(state.failSeek(*new_seek) && state.finishFailedSeek(*new_seek),
          "recoverable seek failure did not return to Idle");
    check(state.close() && !state.accepts(session, *new_seek),
          "close did not invalidate outstanding operations");

    capabilities.growing = false;
    check(state.beginOpen(capabilities, policy) && state.completeOpen(),
          "state machine could not open a second session");
    const auto failed_session = state.sessionId();
    const auto failed_generation = state.seekGeneration();
    check(state.failSession() && state.sessionState() == tlvdemux::SessionState::Failed &&
              !state.accepts(failed_session, failed_generation) &&
              !state.requestSeek().has_value() && !state.startIndexBuilding(),
          "fatal session failure was not sticky");
    check(state.close(), "failed session could not be closed");
}

void test_stream_event_timing() {
    const auto ntp = [](const std::uint64_t seconds) { return seconds << 32U; };
    const auto target_is = [](const tlvdemux::StreamEventTiming& timing,
                              const std::int64_t microseconds) {
        return timing.target_time.has_value() &&
            timing.target_time->value == microseconds &&
            timing.target_time->timescale == 1000000;
    };
    tlvdemux::StreamEvent event;

    event.time_mode = 0;
    auto timing = tlvdemux::resolveStreamEventTiming(event, true);
    check(timing.domain == tlvdemux::StreamEventClockDomain::Immediate &&
              !timing.target_time.has_value(),
          "immediate EMT event acquired a target clock");

    event.time_mode = 1;
    event.time_value = ntp(200);
    timing = tlvdemux::resolveStreamEventTiming(event, true);
    check(timing.domain == tlvdemux::StreamEventClockDomain::PlaybackUtc &&
              target_is(timing, 200000000),
          "mode-1 EMT event did not retain the playback wall-clock domain");

    const tlvdemux::BroadcastClock clock{
        tlvdemux::Timestamp{10000000, 1000000},
        tlvdemux::Timestamp{100000000, 1000000}, 0, false};
    event.time_mode = 5;
    event.time_value = ntp(103);
    timing = tlvdemux::resolveStreamEventTiming(event, true, clock);
    check(timing.domain == tlvdemux::StreamEventClockDomain::MediaTimeline &&
              target_is(timing, 13000000),
          "mode-5 recorded EMT event did not use original broadcast time");
    timing = tlvdemux::resolveStreamEventTiming(event, false, clock);
    check(timing.domain == tlvdemux::StreamEventClockDomain::PlaybackUtc &&
              target_is(timing, 103000000),
          "mode-5 live EMT event did not use the wall clock");

    event.time_mode = 2;
    event.utc_reference = ntp(100);
    event.npt_reference = ntp(20);
    event.time_value = ntp(22);
    timing = tlvdemux::resolveStreamEventTiming(event, true, clock);
    check(timing.domain == tlvdemux::StreamEventClockDomain::MediaTimeline &&
              target_is(timing, 12000000),
          "NPT EMT event was not projected through UTC onto media time");

    event.time_mode = 3;
    event.time_value = ntp(3);
    timing = tlvdemux::resolveStreamEventTiming(
        event, true, std::nullopt, tlvdemux::Timestamp{5000000, 1000000});
    check(timing.domain == tlvdemux::StreamEventClockDomain::MediaTimeline &&
              target_is(timing, 8000000),
          "programme-relative EMT event did not use programme media start");

    event.time_mode = 4;
    timing = tlvdemux::resolveStreamEventTiming(event, true);
    check(timing.domain == tlvdemux::StreamEventClockDomain::Unsupported,
          "reserved EMT time mode was accepted");
}

void test_recording_index() {
    tlvdemux::RecordingIndex index;
    index.begin(false);
    check(index.state() == tlvdemux::IndexState::Building,
          "recording index did not enter Building");

    check(index.observe(video_unit(0, 100, 20, true)), "first video AU was rejected");
    check(index.observe(video_unit(1000000, 1100, 20, false)), "second video AU was rejected");
    check(index.observe(video_unit(2000000, 2100, 60, true)), "second RAP was rejected");
    check(index.observe(video_unit(3000000, 3100, 60, false)), "tail video AU was rejected");

    const auto provisional = index.duration();
    check(provisional.status == tlvdemux::DurationStatus::Provisional &&
              provisional.value.value == 3000000,
          "indexed presentation extent did not produce provisional duration");
    check(index.seekPoints().size() == 2,
          "random-access AUs did not produce two seek points");

    const auto previous = index.previousSync(tlvdemux::Timestamp{2500, 1000});
    check(previous.has_value() && previous->presentation_time.value == 2000000 &&
              previous->signalling_offset == 60 &&
              previous->random_access_offset == 2100,
          "previous-sync lookup selected the wrong RAP");
    const auto surrounding = index.seekPointsFor(tlvdemux::Timestamp{1, 1});
    check(surrounding.has_value() &&
              surrounding->first.presentation_time.value == 0 &&
              surrounding->second.has_value() &&
              surrounding->second->presentation_time.value == 2000000,
          "two-sided seek map lookup did not return surrounding RAPs");

    const auto middle_estimate = index.estimateOffset(tlvdemux::Timestamp{1, 1}, 4100);
    check(middle_estimate.has_value() && *middle_estimate == 1100,
          "piecewise seek interpolation produced the wrong middle offset");

    check(!index.addSeekPoint(tlvdemux::SeekPoint{
              tlvdemux::Timestamp{5, 1}, 5000, 4000, 7, 0}),
          "seek point with signalling after RAP was accepted");
    check(index.finalize(), "recording index did not finalize");
    check(index.duration().status == tlvdemux::DurationStatus::Complete &&
              index.duration().value.value == 4000000,
          "finalized recording duration did not include inferred tail frame duration");
    const auto tail_estimate = index.estimateOffset(tlvdemux::Timestamp{3, 1}, 4100);
    check(tail_estimate.has_value() && *tail_estimate == 3100,
          "tail seek interpolation did not use duration and source size");
    check(!index.updateDuration(tlvdemux::DurationInfo{
              tlvdemux::Timestamp{3, 1}, tlvdemux::DurationStatus::Complete}),
          "complete duration was allowed to decrease");
}

void test_duration_uses_recording_timeline_endpoint() {
    tlvdemux::RecordingIndex index;
    index.begin(false);
    check(index.observe(video_unit(500000, 100, 20, true)) &&
              index.observe(video_unit(1500000, 1100, 20, false)) &&
              index.observe(video_unit(2500000, 2100, 60, true)),
          "non-zero-origin video AUs were rejected");
    check(index.duration().status == tlvdemux::DurationStatus::Provisional &&
              index.duration().value.value == 2500000,
          "provisional duration subtracted the first video PTS from the timeline endpoint");
    check(index.finalize() && index.duration().value.value == 3500000,
          "complete duration did not include the final frame interval on the recording timeline");
}

void test_duration_probe_range_protocol() {
    tlvdemux::DurationProbe probe;
    tlvdemux::DurationProbeOptions options;
    options.initial_range_size = 10;
    options.max_range_size = 40;
    check(probe.begin(100, options), "duration probe rejected a valid source");

    auto request = probe.nextRange();
    check(request.has_value() && request->offset == 0 && request->length == 10,
          "duration probe did not request its initial head range");
    const auto generation = request->generation;
    const std::vector<std::uint8_t> garbage(40, 0x55);
    check(!probe.pushRange(request->request_id + 1, 0, garbage.data(), 10, true) &&
              probe.state() == tlvdemux::DurationProbeState::NeedRange,
          "stale duration response changed the active request");
    check(probe.pushRange(request->request_id, 0, garbage.data(), 4, false) &&
              probe.pushRange(request->request_id, 4, garbage.data() + 4, 6, true),
          "chunked duration response was rejected");

    request = probe.nextRange();
    check(request.has_value() && request->generation == generation &&
              request->offset == 10 && request->length == 10,
          "duration probe did not double its head window");
    check(probe.pushRange(request->request_id, 10, garbage.data(), 10, true),
          "second duration range was rejected");
    request = probe.nextRange();
    check(request.has_value() && request->offset == 20 && request->length == 20,
          "duration probe did not reach its configured maximum head window");
    check(probe.pushRange(request->request_id, 20, garbage.data(), 20, true) &&
              probe.state() == tlvdemux::DurationProbeState::Unknown &&
              probe.failure() == tlvdemux::DurationProbeFailure::NoVideo &&
              probe.transferredBytes() == 40,
          "duration probe did not stop after its no-video byte budget");

    check(probe.begin(100, options), "duration probe could not restart");
    request = probe.nextRange();
    check(request.has_value(), "restarted duration probe has no range request");
    const auto old_request = *request;
    probe.cancel();
    check(probe.state() == tlvdemux::DurationProbeState::Cancelled &&
              !probe.pushRange(old_request.request_id, old_request.offset,
                               garbage.data(), 10, true),
          "cancelled duration probe accepted a late response");

    check(probe.begin(100, options), "duration probe could not restart after cancel");
    request = probe.nextRange();
    check(request.has_value() && probe.failRange(request->request_id) &&
              probe.state() == tlvdemux::DurationProbeState::Failed &&
              probe.failure() == tlvdemux::DurationProbeFailure::SourceError,
          "duration probe did not preserve a source failure");

    check(probe.begin(100, options), "duration probe could not restart after failure");
    request = probe.nextRange();
    check(request.has_value() &&
              !probe.pushRange(request->request_id, request->offset,
                               garbage.data(), 5, true) &&
              probe.failure() == tlvdemux::DurationProbeFailure::InvalidResponse,
          "duration probe accepted a short completed range");

    check(!probe.begin(0, options) &&
              probe.state() == tlvdemux::DurationProbeState::Unknown &&
              probe.failure() == tlvdemux::DurationProbeFailure::InvalidSource,
          "duration probe accepted a zero-sized source");
}

} // namespace

int main() {
    test_seek_policy_selection();
    test_playback_state_machine();
    test_stream_event_timing();
    test_recording_index();
    test_duration_uses_recording_timeline_endpoint();
    test_duration_probe_range_protocol();
    std::cout << "all recording tests passed\n";
    return 0;
}

#include <tlvdemux/playback.hpp>

#include <limits>

namespace tlvdemux {

namespace {

constexpr std::uint32_t clock_timescale = 1000000;

std::optional<std::int64_t> timestamp_us(const Timestamp value) noexcept {
    if (value.timescale == 0) return std::nullopt;
    const auto scale = static_cast<std::int64_t>(value.timescale);
    const auto whole = value.value / scale;
    const auto remainder = value.value % scale;
    if (whole > std::numeric_limits<std::int64_t>::max() / clock_timescale ||
        whole < std::numeric_limits<std::int64_t>::min() / clock_timescale) {
        return std::nullopt;
    }
    return whole * clock_timescale + remainder * clock_timescale / scale;
}

std::optional<std::int64_t> ntp_us(const std::uint64_t value) noexcept {
    const auto seconds = value >> 32U;
    const auto fraction = static_cast<std::uint32_t>(value);
    if (seconds > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max() / clock_timescale)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(seconds * clock_timescale +
        (static_cast<std::uint64_t>(fraction) * clock_timescale >> 32U));
}

std::optional<std::int64_t> ntp_delta_us(const std::uint64_t value,
                                         const std::uint64_t reference) noexcept {
    const auto raw = value - reference;
    const bool negative = (raw & (1ULL << 63U)) != 0;
    const auto magnitude = negative ? (~raw + 1U) : raw;
    const auto converted = ntp_us(magnitude);
    if (!converted.has_value()) return std::nullopt;
    return negative ? -*converted : *converted;
}

std::optional<Timestamp> add_us(const Timestamp base, const std::int64_t delta) noexcept {
    const auto converted = timestamp_us(base);
    if (!converted.has_value() ||
        (delta > 0 && *converted > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && *converted < std::numeric_limits<std::int64_t>::min() - delta)) {
        return std::nullopt;
    }
    return Timestamp{*converted + delta, clock_timescale};
}

StreamEventTiming wall_clock(const std::uint64_t raw) noexcept {
    const auto target = ntp_us(raw);
    if (!target.has_value()) return {StreamEventClockDomain::Unsupported, std::nullopt};
    return {StreamEventClockDomain::PlaybackUtc,
            Timestamp{*target, clock_timescale}};
}

StreamEventTiming original_utc(const std::uint64_t raw, const bool recorded,
                               const std::optional<BroadcastClock> clock) noexcept {
    if (!recorded) return wall_clock(raw);
    if (!clock.has_value()) {
        return {StreamEventClockDomain::AwaitingReference, std::nullopt};
    }
    const auto target = ntp_us(raw);
    const auto anchor = timestamp_us(clock->broadcast_time);
    if (!target.has_value() || !anchor.has_value()) {
        return {StreamEventClockDomain::Unsupported, std::nullopt};
    }
    const auto projected = add_us(clock->media_time, *target - *anchor);
    if (!projected.has_value()) {
        return {StreamEventClockDomain::Unsupported, std::nullopt};
    }
    return {StreamEventClockDomain::MediaTimeline, projected};
}

} // namespace

SeekPolicy chooseSeekPolicy(const SourceCapabilities& capabilities,
                            const bool usable_index,
                            const bool buffered_seek_available) noexcept {
    if (capabilities.random_access && usable_index) {
        return SeekPolicy::IndexedRandomAccess;
    }
    if (capabilities.random_access && capabilities.size_known) {
        return SeekPolicy::AdaptiveRangeProbe;
    }
    if (buffered_seek_available) {
        return SeekPolicy::BufferedOnly;
    }
    return SeekPolicy::Unsupported;
}

StreamEventTiming resolveStreamEventTiming(
    const StreamEvent& event, const bool recorded,
    const std::optional<BroadcastClock> broadcast_clock,
    const std::optional<Timestamp> program_start_media_time) noexcept {
    switch (event.time_mode) {
    case 0:
        return {StreamEventClockDomain::Immediate, std::nullopt};
    case 1:
        return wall_clock(event.time_value);
    case 2: {
        if (!event.utc_reference.has_value() || !event.npt_reference.has_value()) {
            return {StreamEventClockDomain::AwaitingReference, std::nullopt};
        }
        const auto delta = ntp_delta_us(event.time_value, *event.npt_reference);
        const auto utc = ntp_us(*event.utc_reference);
        if (!delta.has_value() || !utc.has_value() ||
            (*delta > 0 && *utc > std::numeric_limits<std::int64_t>::max() - *delta) ||
            (*delta < 0 && *utc < std::numeric_limits<std::int64_t>::min() - *delta)) {
            return {StreamEventClockDomain::Unsupported, std::nullopt};
        }
        const auto target_utc = *utc + *delta;
        if (!recorded) {
            return {StreamEventClockDomain::PlaybackUtc,
                    Timestamp{target_utc, clock_timescale}};
        }
        if (!broadcast_clock.has_value()) {
            return {StreamEventClockDomain::AwaitingReference, std::nullopt};
        }
        const auto anchor = timestamp_us(broadcast_clock->broadcast_time);
        if (!anchor.has_value()) {
            return {StreamEventClockDomain::Unsupported, std::nullopt};
        }
        const auto projected = add_us(broadcast_clock->media_time, target_utc - *anchor);
        return projected.has_value()
            ? StreamEventTiming{StreamEventClockDomain::MediaTimeline, projected}
            : StreamEventTiming{StreamEventClockDomain::Unsupported, std::nullopt};
    }
    case 3: {
        if (!program_start_media_time.has_value()) {
            return {StreamEventClockDomain::AwaitingReference, std::nullopt};
        }
        const auto relative = ntp_us(event.time_value);
        if (!relative.has_value()) {
            return {StreamEventClockDomain::Unsupported, std::nullopt};
        }
        const auto target = add_us(*program_start_media_time, *relative);
        return target.has_value()
            ? StreamEventTiming{StreamEventClockDomain::MediaTimeline, target}
            : StreamEventTiming{StreamEventClockDomain::Unsupported, std::nullopt};
    }
    case 5:
        return original_utc(event.time_value, recorded, broadcast_clock);
    default:
        return {StreamEventClockDomain::Unsupported, std::nullopt};
    }
}

bool PlaybackStateMachine::active_session() const noexcept {
    return session_state_ != SessionState::Closed && session_state_ != SessionState::Failed;
}

bool PlaybackStateMachine::current_seek(const std::uint64_t generation) const noexcept {
    return active_session() && generation == seek_generation_;
}

bool PlaybackStateMachine::beginOpen(SourceCapabilities capabilities,
                                     const SeekPolicy policy) {
    if (session_state_ != SessionState::Closed) return false;
    ++session_id_;
    ++seek_generation_;
    capabilities_ = capabilities;
    seek_policy_ = policy;
    session_state_ = SessionState::Opening;
    index_state_ = IndexState::Absent;
    seek_state_ = SeekState::Idle;
    return true;
}

bool PlaybackStateMachine::completeOpen() {
    if (session_state_ != SessionState::Opening) return false;
    session_state_ = SessionState::Ready;
    return true;
}

bool PlaybackStateMachine::close() {
    if (session_state_ == SessionState::Closed) return false;
    ++seek_generation_;
    session_state_ = SessionState::Closed;
    index_state_ = IndexState::Absent;
    seek_state_ = SeekState::Idle;
    seek_policy_ = SeekPolicy::Unsupported;
    capabilities_ = {};
    return true;
}

bool PlaybackStateMachine::failSession() {
    if (!active_session() || session_state_ == SessionState::Failed) return false;
    ++seek_generation_;
    session_state_ = SessionState::Failed;
    seek_state_ = SeekState::Idle;
    return true;
}

bool PlaybackStateMachine::reachGrowingEnd() {
    if (session_state_ != SessionState::Ready || !capabilities_.growing) return false;
    session_state_ = SessionState::WaitingForData;
    return true;
}

bool PlaybackStateMachine::reachFinalEnd() {
    if (session_state_ != SessionState::Ready || capabilities_.growing) return false;
    session_state_ = SessionState::Ended;
    return true;
}

bool PlaybackStateMachine::sourceGrew() {
    if (!capabilities_.growing ||
        (session_state_ != SessionState::Ready &&
         session_state_ != SessionState::WaitingForData)) {
        return false;
    }
    if (session_state_ == SessionState::WaitingForData) session_state_ = SessionState::Ready;
    if (index_state_ == IndexState::Following) index_state_ = IndexState::Building;
    return true;
}

bool PlaybackStateMachine::sourceFinalized(const bool unread_data) {
    if (!active_session() || !capabilities_.growing) return false;
    capabilities_.growing = false;
    if (index_state_ == IndexState::Following) {
        index_state_ = unread_data ? IndexState::Building : IndexState::Complete;
    }
    if (session_state_ == SessionState::WaitingForData) {
        session_state_ = unread_data ? SessionState::Ready : SessionState::Ended;
    }
    return true;
}

bool PlaybackStateMachine::startIndexLoading() {
    if (!active_session() || index_state_ != IndexState::Absent) return false;
    index_state_ = IndexState::Loading;
    return true;
}

bool PlaybackStateMachine::startIndexBuilding() {
    if (!active_session()) return false;
    switch (index_state_) {
    case IndexState::Absent:
    case IndexState::Partial:
    case IndexState::Following:
    case IndexState::Stale:
    case IndexState::Failed:
        index_state_ = IndexState::Building;
        return true;
    default:
        return false;
    }
}

bool PlaybackStateMachine::loadedIndex(const bool complete) {
    if (index_state_ != IndexState::Loading) return false;
    index_state_ = complete ? IndexState::Complete : IndexState::Partial;
    return true;
}

bool PlaybackStateMachine::markIndexStale() {
    if (index_state_ != IndexState::Loading && index_state_ != IndexState::Complete &&
        index_state_ != IndexState::Partial) {
        return false;
    }
    index_state_ = IndexState::Stale;
    return true;
}

bool PlaybackStateMachine::failIndex() {
    if (!active_session() || index_state_ == IndexState::Absent ||
        index_state_ == IndexState::Failed) {
        return false;
    }
    index_state_ = IndexState::Failed;
    return true;
}

bool PlaybackStateMachine::pauseIndex() {
    if (index_state_ != IndexState::Building) return false;
    index_state_ = IndexState::Partial;
    return true;
}

bool PlaybackStateMachine::reachIndexEnd(const bool growing) {
    if (index_state_ != IndexState::Building) return false;
    index_state_ = growing ? IndexState::Following : IndexState::Complete;
    return true;
}

std::optional<std::uint64_t> PlaybackStateMachine::requestSeek() {
    if (session_state_ != SessionState::Ready &&
        session_state_ != SessionState::WaitingForData &&
        session_state_ != SessionState::Ended) {
        return std::nullopt;
    }
    if (session_state_ == SessionState::Ended ||
        session_state_ == SessionState::WaitingForData) {
        session_state_ = SessionState::Ready;
    }
    ++seek_generation_;
    seek_state_ = SeekState::Resolving;
    return seek_generation_;
}

bool PlaybackStateMachine::awaitIndex(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::Resolving) return false;
    seek_state_ = SeekState::AwaitingIndex;
    return true;
}

bool PlaybackStateMachine::resumeResolving(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::AwaitingIndex) return false;
    seek_state_ = SeekState::Resolving;
    return true;
}

bool PlaybackStateMachine::beginReposition(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::Resolving) return false;
    seek_state_ = SeekState::Repositioning;
    return true;
}

bool PlaybackStateMachine::beginPriming(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::Repositioning) return false;
    seek_state_ = SeekState::Priming;
    return true;
}

bool PlaybackStateMachine::beginPreroll(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::Priming) return false;
    seek_state_ = SeekState::Prerolling;
    return true;
}

bool PlaybackStateMachine::beginLanding(const std::uint64_t generation) {
    if (!current_seek(generation) ||
        (seek_state_ != SeekState::Priming && seek_state_ != SeekState::Prerolling)) {
        return false;
    }
    seek_state_ = SeekState::Landing;
    return true;
}

bool PlaybackStateMachine::completeSeek(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::Landing) return false;
    seek_state_ = SeekState::Idle;
    return true;
}

bool PlaybackStateMachine::failSeek(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ == SeekState::Idle ||
        seek_state_ == SeekState::Failed) {
        return false;
    }
    seek_state_ = SeekState::Failed;
    return true;
}

bool PlaybackStateMachine::finishFailedSeek(const std::uint64_t generation) {
    if (!current_seek(generation) || seek_state_ != SeekState::Failed) return false;
    seek_state_ = SeekState::Idle;
    return true;
}

bool PlaybackStateMachine::cancelSeek() {
    if (seek_state_ == SeekState::Idle) return false;
    ++seek_generation_;
    seek_state_ = SeekState::Idle;
    return true;
}

bool PlaybackStateMachine::accepts(const std::uint64_t session_id,
                                   const std::uint64_t seek_generation) const noexcept {
    return active_session() && session_id == session_id_ && seek_generation == seek_generation_;
}

} // namespace tlvdemux

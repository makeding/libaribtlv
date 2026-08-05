#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <aribtlv/types.hpp>

namespace aribtlv {

enum class IndexState {
    Absent,
    Loading,
    Building,
    Partial,
    Following,
    Complete,
    Stale,
    Failed,
};

enum class DurationStatus { Unknown, Provisional, Complete };
enum class SeekMode { PreviousSync, ExactFrame };

struct DurationInfo {
    Timestamp value{0, 1000000};
    DurationStatus status = DurationStatus::Unknown;
};

struct SeekPoint {
    Timestamp presentation_time{0, 1000000};
    std::uint64_t signalling_offset = 0;
    std::uint64_t random_access_offset = 0;
    std::uint64_t video_track_id = 0;
    std::uint64_t bootstrap_id = 0;
};

struct SeekPoints {
    SeekPoint first;
    std::optional<SeekPoint> second;
};

class RecordingIndex {
public:
    void reset();
    void begin(bool growing);
    void selectVideoTrack(std::optional<std::uint64_t> track_id);
    bool observe(const AccessUnit&);
    bool addSeekPoint(SeekPoint);
    bool updateDuration(DurationInfo);
    bool pause();
    bool resume();
    bool reachReadableEnd(bool growing);
    bool finalize();
    void markStale();
    void fail();

    std::optional<SeekPoint> previousSync(Timestamp target) const;
    std::optional<SeekPoints> seekPointsFor(Timestamp target) const;
    std::optional<std::uint64_t> estimateOffset(Timestamp target,
                                                std::uint64_t source_size) const;

    IndexState state() const noexcept { return state_; }
    DurationInfo duration() const noexcept;
    const std::vector<SeekPoint>& seekPoints() const noexcept { return seek_points_; }
    std::optional<std::uint64_t> selectedVideoTrack() const noexcept {
        return selected_video_track_;
    }
    bool growing() const noexcept { return growing_; }

private:
    void update_duration_status();

    IndexState state_ = IndexState::Absent;
    std::optional<std::uint64_t> selected_video_track_;
    std::vector<SeekPoint> seek_points_;
    std::optional<std::int64_t> minimum_pts_us_;
    std::optional<std::int64_t> maximum_pts_us_;
    std::optional<std::int64_t> previous_pts_us_;
    std::int64_t inferred_frame_duration_us_ = 0;
    std::optional<std::int64_t> duration_us_;
    DurationStatus duration_status_ = DurationStatus::Unknown;
    bool growing_ = false;
};

} // namespace aribtlv

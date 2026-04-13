// accel_data.cpp — Movement tracking with circular buffer
#include "accel_data.hpp"

#include <cstdlib>
#include <cstring>

namespace beacon {

void MovementTracker::record_reading(const int16_t samples[][3], int count, int threshold,
                                     uint32_t uptime_sec) {
    // Determine starting index for comparison.
    // If we have no previous reading, skip the first comparison
    // (start from index 2 instead of 1).
    int first = has_previous_ ? 1 : 2;

    // The original C code stores the previous last sample in samples[0]
    // and reads new data into samples[1..32]. We replicate that logic:
    // compare using last_sample_ as the "previous" for samples[1],
    // then consecutive pairs within samples[].

    int movements = 0;
    for (int i = first; i < count; i++) {
        const int16_t* prev = (i == 1) ? last_sample_ : samples[i - 1]; // NOLINT
        if (std::abs(prev[0] - samples[i][0]) > threshold ||
            std::abs(prev[1] - samples[i][1]) > threshold ||
            std::abs(prev[2] - samples[i][2]) > threshold) {
            movements++;
        }
    }

    // Shift 20-second array right by 1
    std::memmove(moves20_ + 1, moves20_, static_cast<size_t>(kMoves20Size - 1));
    moves20_[0] = (movements > 1) ? 1 : 0;

    // Check if we need to shift the 300-second (5-minute) array
    if (uptime_sec > last_shift_time_ + 300) {
        std::memmove(moves300_ + 1, moves300_, static_cast<size_t>(kMoves300Size - 1));
        last_shift_time_ = uptime_sec;
    }

    // Update first two elements of moves300_ from moves20_
    moves300_[0] = 0;
    moves300_[1] = 0;
    for (int i = 0; i < 15; i++) {
        if (moves20_[i] != 0) {
            moves300_[0] = 1;
            break;
        }
    }
    for (int i = 15; i < kMoves20Size; i++) {
        if (moves20_[i] != 0) {
            moves300_[1] = 1;
            break;
        }
    }

    // Save last sample for next call
    if (count > 0) {
        last_sample_[0] = samples[count - 1][0];
        last_sample_[1] = samples[count - 1][1];
        last_sample_[2] = samples[count - 1][2];
        has_previous_ = true;
    }
}

bool MovementTracker::has_movement(int start_min, int end_min) const {
    int start = start_min / 5;
    int end = (end_min + 4) / 5;

    if (start < 0 || start >= kMoves300Size) {
        start = 0;
    }
    if (end < 0 || end > kMoves300Size) {
        end = kMoves300Size;
    }

    for (int i = start; i < end; i++) {
        if (moves300_[i] > 0) {
            return true;
        }
    }
    return false;
}

uint8_t MovementTracker::compute_accel_byte() const {
    uint8_t res = 0;

    // bit 0: movements in 0..10 minutes
    if (has_movement(0, 10)) {
        res |= 0x01;
    }
    // bit 1: 10..30 minutes
    if (has_movement(10, 30)) {
        res |= 0x02;
    }
    // bit 2: 30..60 minutes
    if (has_movement(30, 60)) {
        res |= 0x04;
    }
    // bit 3: 1..3 hours
    if (has_movement(60, 180)) {
        res |= 0x08;
    }
    // bit 4: 3..6 hours
    if (has_movement(180, 360)) {
        res |= 0x10;
    }
    // bit 5: 6..12 hours
    if (has_movement(360, 720)) {
        res |= 0x20;
    }
    // bit 6: 12..24 hours
    if (has_movement(720, 1440)) {
        res |= 0x40;
    }

    return res;
}

} // namespace beacon

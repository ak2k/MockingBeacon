// accel_data.hpp — Movement tracking with circular buffer
#ifndef ACCEL_DATA_HPP
#define ACCEL_DATA_HPP

#include <cstdint>

namespace beacon {

class MovementTracker {
  public:
    /// Process a batch of accelerometer FIFO samples.
    /// @param samples   Nx3 array of int16_t (X,Y,Z per sample)
    /// @param count     number of samples (typically 33)
    /// @param threshold per-axis movement threshold
    /// @param uptime_sec current uptime in seconds
    void record_reading(const int16_t samples[][3], int count, int threshold, uint32_t uptime_sec);

    /// Check whether movement was detected in a time window.
    /// @param start_min  start of window in minutes from now
    /// @param end_min    end of window in minutes from now
    /// @return true if any 5-minute slot in the range recorded movement
    bool has_movement(int start_min, int end_min) const;

    /// Compute a 7-bit movement summary byte (from calc_accel_byte in main.c).
    /// Each bit covers a progressively longer time window:
    ///   bit 0: movement in  0..10 minutes
    ///   bit 1: movement in 10..30 minutes
    ///   bit 2: movement in 30..60 minutes
    ///   bit 3: movement in  1..3 hours
    ///   bit 4: movement in  3..6 hours
    ///   bit 5: movement in  6..12 hours
    ///   bit 6: movement in 12..24 hours
    uint8_t compute_accel_byte() const;

    /// Get the most recent accelerometer temperature reading.
    int16_t temperature() const { return temperature_; }

    /// Set the accelerometer temperature (read separately via I2C).
    void set_temperature(int16_t t) { temperature_ = t; }

    // Expose internal state for testing
    const uint8_t* moves20() const { return moves20_; }
    const uint8_t* moves300() const { return moves300_; }

    static constexpr int kMoves20Size = 30;
    static constexpr int kMoves300Size = 288;

  private:
    uint8_t moves20_[kMoves20Size] = {};
    uint8_t moves300_[kMoves300Size] = {};
    int16_t last_sample_[3] = {};
    bool has_previous_ = false;
    int16_t temperature_ = 0;
    uint32_t last_shift_time_ = 0;
};

} // namespace beacon

#endif // ACCEL_DATA_HPP

#pragma once
#include <cstdint>
#include <stdexcept>

namespace mouse {
// Monotonic microseconds, supplied by the caller so deadline behavior is testable.
class ClickSpeedTest {
public:
    enum class State { Ready, Running, Finished };
    void Reset(int seconds, int button) {
        if ((seconds != 1 && seconds != 3) || (button != 0 && button != 1))
            throw std::invalid_argument("Invalid click speed test options");
        durationUs_ = static_cast<uint64_t>(seconds) * 1000000;
        button_ = button; clicks_ = 0; startedUs_ = 0; state_ = State::Ready;
    }
    bool Press(int button, uint64_t nowUs) {
        Advance(nowUs);
        if (button != button_ || state_ == State::Finished) return false;
        if (state_ == State::Ready) { startedUs_ = nowUs; state_ = State::Running; }
        ++clicks_;
        return true;
    }
    void Advance(uint64_t nowUs) {
        if (state_ == State::Running && nowUs >= startedUs_ && nowUs - startedUs_ >= durationUs_)
            state_ = State::Finished;
    }
    uint64_t RemainingUs(uint64_t nowUs) const {
        if (state_ == State::Finished) return 0;
        const auto elapsed = state_ == State::Running && nowUs >= startedUs_ ? nowUs - startedUs_ : 0;
        return elapsed >= durationUs_ ? 0 : durationUs_ - elapsed;
    }
    State Status() const { return state_; }
    int Button() const { return button_; }
    int64_t Clicks() const { return clicks_; }
    double Cps() const { return clicks_ * 1000000.0 / durationUs_; }
    double Apm() const { return Cps() * 60; }
private:
    State state_ = State::Ready;
    uint64_t durationUs_ = 1000000, startedUs_ = 0;
    int button_ = 0;
    int64_t clicks_ = 0;
};
}

/**
 * @file src/step_timer.h
 * @brief Named step durations for teardown timing logs.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace util {

  /**
   * @brief Records how long each named step of a multi-step operation took and formats them for one log line.
   *
   * Steps keep the order in which they were first recorded. Recording a step again adds to it,
   * so a step that runs several times reports its total. A skipped step is kept as `skipped`
   * so the log shows it was ruled out rather than forgotten.
   *
   * @tparam Clock A clock with a static `now()`; tests pass a fake one.
   */
  template<class Clock>
  class basic_step_timer_t {
  public:
    using time_point = typename Clock::time_point;

    basic_step_timer_t():
        _started(Clock::now()) {
    }

    /**
     * @brief Start the timer at @p started, so that elapsed() also counts time spent before construction.
     */
    explicit basic_step_timer_t(time_point started):
        _started(started) {
    }

    /**
     * @brief Read the timer's clock, to mark the start of a step.
     */
    time_point now() const {
      return Clock::now();
    }

    /**
     * @brief Time since the timer was created, in whole milliseconds.
     */
    std::chrono::milliseconds elapsed() const {
      return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - _started);
    }

    /**
     * @brief Add a duration to a step, creating it if needed. A skipped step becomes a timed one.
     */
    void record(std::string_view name, std::chrono::milliseconds duration) {
      auto &step = find_or_add(name);
      step.duration = step.duration.value_or(std::chrono::milliseconds::zero()) + duration;
    }

    /**
     * @brief Record the time from @p started to now as a step.
     * @return The duration added, in whole milliseconds.
     */
    std::chrono::milliseconds record_since(std::string_view name, time_point started) {
      const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
      record(name, duration);
      return duration;
    }

    /**
     * @brief Mark a step as skipped. A step that already has a duration keeps it.
     */
    void skip(std::string_view name) {
      (void) find_or_add(name);
    }

    /**
     * @brief Format the steps as `name=Nms` or `name=skipped`, separated by spaces.
     */
    std::string format() const {
      std::string out;
      for (const auto &step : _steps) {
        if (!out.empty()) {
          out += ' ';
        }
        out += step.name;
        out += '=';
        out += step.duration ? std::to_string(step.duration->count()) + "ms" : std::string {"skipped"};
      }
      return out;
    }

  private:
    struct step_t {
      std::string name;
      std::optional<std::chrono::milliseconds> duration;  ///< Empty while the step is only skipped.
    };

    step_t &find_or_add(std::string_view name) {
      const auto it = std::ranges::find(_steps, name, &step_t::name);
      if (it != _steps.end()) {
        return *it;
      }
      return _steps.emplace_back(step_t {std::string {name}, std::nullopt});
    }

    time_point _started;
    std::vector<step_t> _steps;
  };

  using step_timer_t = basic_step_timer_t<std::chrono::steady_clock>;

}  // namespace util

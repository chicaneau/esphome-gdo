#include "gdo_cover.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace gdo {

static const char *const TAG = "gdo.cover";

const float UNKNOWN_POSITION = 0.5f;
// While an endstop is configured but has not confirmed the travel yet, the
// time-based estimate is held just short of the limit. Reporting a flat 100%
// (or 0%) makes the cover look finished while the door is still moving.
const float ALMOST_OPEN = 0.99f;
const float ALMOST_CLOSED = 0.01f;

using namespace esphome::cover;

static const char *op_name(CoverOperation op) {
  switch (op) {
    case COVER_OPERATION_IDLE:
      return "IDLE";
    case COVER_OPERATION_OPENING:
      return "OPENING";
    case COVER_OPERATION_CLOSING:
      return "CLOSING";
  }
  return "?";
}

// How long to wait for an endstop before giving up on it. The travel-time
// estimate is never exact, so allow a margin over the configured duration.
// Timing out at exactly the duration means a door that takes 12.8s when
// configured for 12.6s glitches to an unknown position on every single cycle.
static uint32_t endstop_timeout(uint32_t duration) { return duration + duration / 4 + 2000; }

void GdoCover::dump_config() {
  LOG_COVER("", "GDO Cover", this);
  LOG_BINARY_SENSOR("  ", "Open Endstop", this->open_endstop_);
  ESP_LOGCONFIG(TAG, "  Open Duration: %.1fs", this->open_duration_ / 1e3f);
  LOG_BINARY_SENSOR("  ", "Close Endstop", this->close_endstop_);
  ESP_LOGCONFIG(TAG, "  Close Duration: %.1fs", this->close_duration_ / 1e3f);
  ESP_LOGCONFIG(TAG, "  Start Delay: %.1fs", this->start_delay_ / 1e3f);
  ESP_LOGCONFIG(TAG, "  Press Interval: %.1fs", this->press_interval_ / 1e3f);
}

void GdoCover::setup() {
  // Restore state after restart
  auto restore = this->restore_state_();
  if (restore.has_value()) {
    restore->apply(this);
  } else {
    this->position = UNKNOWN_POSITION;
  }
  this->reconcile_endstop_state_();
  if (this->open_endstop_ != nullptr) {
    this->open_endstop_->add_on_state_callback([this](bool value) {
      if (value) {
        if (this->current_operation == COVER_OPERATION_CLOSING) {
          // A reed at the top of travel cannot genuinely trigger while the
          // door is moving away from it. This is contact chatter; trusting it
          // would rewrite the position and abort the move.
          ESP_LOGI(TAG, "Open endstop reported reached while closing - chatter, ignoring.");
          return;
        }
        // Reached the open endstop. Update state
        float dur = (millis() - this->start_dir_time_) / 1e3f;
        ESP_LOGI(TAG, "Open endstop reached. Took %.1fs.", dur);
        this->position = COVER_OPEN;
        this->target_position_ = COVER_OPEN;
        this->current_operation = COVER_OPERATION_IDLE;
        this->publish_state();
      } else {
        // Moved away from the open endstop.
        // If this was triggered by an external control assume target position is fully closed
        // and start updating state without triggering a press.
        ESP_LOGI(TAG, "Open endstop released.");
        if (this->current_operation == COVER_OPERATION_IDLE) {
          this->target_position_ = COVER_CLOSED;
          this->start_direction_(COVER_OPERATION_CLOSING, false);
        }
      }
    });
  }
  if (this->close_endstop_ != nullptr) {
    this->close_endstop_->add_on_state_callback([this](bool value) {
      if (value) {
        if (this->current_operation == COVER_OPERATION_OPENING) {
          // A reed at the bottom of travel cannot genuinely trigger while the
          // door is moving away from it. Reed switches chatter for a few
          // hundred ms when leaving the magnet; without this guard the event
          // rewrites the position to fully closed, aborts the partial move,
          // and the following release is misread as an external full open.
          ESP_LOGI(TAG, "Closed endstop reported reached while opening - chatter, ignoring.");
          return;
        }
        // Reached the closed endstop. Update state
        float dur = (millis() - this->start_dir_time_) / 1e3f;
        ESP_LOGI(TAG, "Closed endstop reached. Took %.1fs.", dur);
        this->position = COVER_CLOSED;
        this->target_position_ = COVER_CLOSED;
        this->current_operation = COVER_OPERATION_IDLE;
        this->publish_state();
      } else {
        // Moved away from the closed endstop.
        // If this was triggered by an external control assume target position is fully open
        // and start updating state without triggering a press.
        ESP_LOGI(TAG, "Closed endstop released.");
        if (this->current_operation == COVER_OPERATION_IDLE) {
          this->target_position_ = COVER_OPEN;
          this->start_direction_(COVER_OPERATION_OPENING, false);
        }
      }
    });
  }
}

void GdoCover::reconcile_endstop_state_() {
  // Don't trust a one-shot read at boot: after a power loss the GPIO sensor
  // and its debounce filter may not have settled yet. Wait until both endstops
  // (that are configured) have reported a state, then fix the restored position.
  auto ready = [](binary_sensor::BinarySensor *s) { return s == nullptr || s->has_state(); };
  if (!ready(this->open_endstop_) || !ready(this->close_endstop_)) {
    return;
  }

  if (this->open_endstop_ != nullptr) {
    // Fix restored state based on open endstop
    if (this->open_endstop_->state) {
      this->position = COVER_OPEN;
    } else if (this->position == COVER_OPEN) {
      this->position = UNKNOWN_POSITION;
    }
  }
  if (this->close_endstop_ != nullptr) {
    // Fix restored state based on closed endstop
    if (this->close_endstop_->state) {
      this->position = COVER_CLOSED;
    } else if (this->position == COVER_CLOSED) {
      this->position = UNKNOWN_POSITION;
    }
  }
  if (!this->endstop_reconciled_) {
    this->endstop_reconciled_ = true;
  }
}

void GdoCover::loop() {
  if (!this->endstop_reconciled_) {
    this->reconcile_endstop_state_();
    if (this->endstop_reconciled_) {
      this->publish_state();
    }
  }

  if (this->current_operation == COVER_OPERATION_IDLE) {
    return;
  }

  const uint32_t now = millis();

  // Recompute position every loop cycle
  this->recompute_position_();

  if (this->is_at_target_()) {
    bool stop_press_pending = false;
    if (this->target_position_ == COVER_OPEN || this->target_position_ == COVER_CLOSED) {
      // Don't trigger stop, let the cover stop by itself.
      this->current_operation = COVER_OPERATION_IDLE;
    } else if ((int32_t)(now - this->stop_press_earliest_) < 0) {
      // The time-based estimate has reached the target, but the door has not
      // physically started moving yet (click-sequence time plus opener reaction
      // time). A stop press sent now reaches an opener that still sees an idle
      // door and gets treated as a new start press, leaving the door running
      // to its limit while the cover already reports idle. Hold off until the
      // door is definitely moving, then a single press stops it reliably.
      stop_press_pending = true;
    } else {
      ESP_LOGI(TAG, "Reached target %.0f%% after %.1fs. Sending stop press.", this->target_position_ * 100.0f,
               (now - this->start_dir_time_) / 1e3f);
      this->start_direction_(COVER_OPERATION_IDLE);
    }
    // While holding off, skip the state push; the every-second publish below
    // already reports the moving position without flooding the API.
    if (!stop_press_pending) {
      this->publish_state();
    }
  } else if (this->stop_press_deadline_ != 0 && (int32_t) (now - this->stop_press_deadline_) >= 0) {
    // Backstop: the estimate never registered the partial target within 1.5x
    // of its expected travel time, yet the door was commanded to stop there.
    // Send the stop press anyway rather than let the door run to its limit.
    ESP_LOGI(TAG, "Estimate failed to reach target %.0f%% within deadline - forcing stop press. Current %.0f%%.",
             this->target_position_ * 100.0f, this->position * 100.0f);
    this->stop_press_deadline_ = 0;
    this->position = this->target_position_;
    this->start_direction_(COVER_OPERATION_IDLE);
    this->publish_state();
  } else if ((this->current_operation == COVER_OPERATION_OPENING && this->open_endstop_ != nullptr &&
              now - this->start_dir_time_ > endstop_timeout(this->open_duration_)) ||
             (this->current_operation == COVER_OPERATION_CLOSING && this->close_endstop_ != nullptr &&
              now - this->start_dir_time_ > endstop_timeout(this->close_duration_))) {
    ESP_LOGI(TAG, "Failed to reach endstop. Likely stopped externally.");
    this->position = UNKNOWN_POSITION;
    this->current_operation = COVER_OPERATION_IDLE;
    this->publish_state();
  }

  // Send current position every second
  if (now - this->last_publish_time_ > 1000) {
    this->publish_state(false);
    this->last_publish_time_ = now;
  }
}

float GdoCover::get_setup_priority() const { return setup_priority::DATA; }

CoverTraits GdoCover::get_traits() {
  auto traits = CoverTraits();
  traits.set_supports_stop(true);
  traits.set_supports_position(true);
  return traits;
}

void GdoCover::control(const CoverCall &call) {
  if (call.get_stop()) {
    ESP_LOGI(TAG, "Received stop command.");
    this->start_direction_(COVER_OPERATION_IDLE);
    this->publish_state();
  }
  if (call.get_position().has_value()) {
    auto pos = *call.get_position();
    ESP_LOGI(TAG, "Received position command: %.0f%% (current %.0f%%, operation %s).", pos * 100.0f,
             this->position * 100.0f, op_name(this->current_operation));
    if (pos == this->position) {
      ESP_LOGI(TAG, "Nothing to do. Already at target position.");
    } else {
      auto op = pos < this->position ? COVER_OPERATION_CLOSING : COVER_OPERATION_OPENING;
      this->target_position_ = pos;
      this->start_direction_(op);
    }
  }
}

void GdoCover::stop_prev_trigger_() {
  if (this->prev_command_trigger_ != nullptr) {
    this->prev_command_trigger_->stop_action();
    this->prev_command_trigger_ = nullptr;
  }
}

bool GdoCover::is_at_target_() const {
  switch (this->current_operation) {
    case COVER_OPERATION_OPENING:
      if (this->target_position_ == COVER_OPEN && this->open_endstop_ != nullptr) {
        return this->open_endstop_->state;
      }
      return this->position >= this->target_position_;
    case COVER_OPERATION_CLOSING:
      if (this->target_position_ == COVER_CLOSED && this->close_endstop_ != nullptr) {
        return this->close_endstop_->state;
      }
      return this->position <= this->target_position_;
    case COVER_OPERATION_IDLE:
    default:
      return true;
  }
}

void GdoCover::start_direction_(CoverOperation dir, bool perform_trigger) {
  if (dir == this->current_operation) {
    ESP_LOGI(TAG, "Nothing to do. CoverOperation %d didn't change.", dir);
    return;
  }

  this->recompute_position_();
  Trigger<> *trig;

  switch (dir) {
    case COVER_OPERATION_IDLE:
      switch (this->current_operation) {
        case COVER_OPERATION_OPENING:
          ESP_LOGI(TAG, "Door is opening. Asked to stop.");
          this->last_direction_ = COVER_OPERATION_OPENING;
          trig = &this->single_press_trigger_;
          break;
        case COVER_OPERATION_CLOSING:
          ESP_LOGI(TAG, "Door is closing. Asked to stop.");
          this->last_direction_ = COVER_OPERATION_CLOSING;
          trig = &this->single_press_trigger_;
          break;
        default:
          return;
      }
      break;

    case COVER_OPERATION_OPENING:
      switch (this->current_operation) {
        case COVER_OPERATION_IDLE:
          if (this->position == COVER_CLOSED) {
            ESP_LOGI(TAG, "Door is fully closed. Asked to open.");
            trig = &this->single_press_trigger_;
          } else if (this->position == COVER_OPEN) {
            ESP_LOGW(TAG, "Door is fully open. Cannot open more.");
            return;
          } else if (this->last_direction_ == COVER_OPERATION_OPENING) {
            // A single press would resume closing (opposite of last motion).
            // Cycle stop -> close -> stop to land back on opening.
            ESP_LOGI(TAG, "Door is partially open (was opening). Asked to open more.");
            trig = &this->triple_press_trigger_;
          } else {
            ESP_LOGI(TAG, "Door is partially open (was closing). Asked to open.");
            trig = &this->single_press_trigger_;
          }
          break;
        case COVER_OPERATION_CLOSING:
          ESP_LOGI(TAG, "Door is closing. Asked to open.");
          trig = &this->double_press_trigger_;  // stop, then opens (opposite of closing)
          break;
        default:
          return;
      }
      break;

    case COVER_OPERATION_CLOSING:
      switch (this->current_operation) {
        case COVER_OPERATION_IDLE:
          if (this->position == COVER_CLOSED) {
            ESP_LOGI(TAG, "Door is fully closed. Cannot close more.");
            return;
          } else if (this->position == COVER_OPEN) {
            ESP_LOGI(TAG, "Door is fully open. Asked to close.");
            trig = &this->single_press_trigger_;
          } else if (this->last_direction_ == COVER_OPERATION_CLOSING) {
            ESP_LOGI(TAG, "Door is partially open (was closing). Asked to close more.");
            trig = &this->triple_press_trigger_;
          } else {
            ESP_LOGI(TAG, "Door is partially open (was opening). Asked to close.");
            trig = &this->single_press_trigger_;
          }
          break;
        case COVER_OPERATION_OPENING:
          ESP_LOGI(TAG, "Door is opening. Asked to close.");
          trig = &this->double_press_trigger_;  // stop, then closes (opposite of opening)
          break;
        default:
          return;
      }
      break;

    default:
      return;
  }

  this->current_operation = dir;

  // Record the direction of every commanded motion, not just of stops. The
  // press logic below (and the opposite-direction rule it implements) relies
  // on it, and a stale value picks a wrong press sequence, moving the door the
  // other way when asked for a partial position.
  if (dir == COVER_OPERATION_OPENING || dir == COVER_OPERATION_CLOSING) {
    this->last_direction_ = dir;
  }

  const uint32_t now = millis();
  this->start_dir_time_ = now;
  this->last_recompute_time_ = now;

  // Earliest time an auto stop press for a partial target may be sent. The
  // press automation runs asynchronously, so the relay is not even clicked
  // until the whole sequence has elapsed, and the opener then needs
  // start_delay_ to physically start moving.
  uint32_t pulses = 1;
  if (trig == &this->double_press_trigger_) {
    pulses = 2;
  } else if (trig == &this->triple_press_trigger_) {
    pulses = 3;
  }
  float start_pos = this->position;
  float target = this->target_position_;
  bool partial_target = (target > COVER_CLOSED) && (target < COVER_OPEN);
  if (perform_trigger) {
    this->stop_press_earliest_ = now + this->start_delay_ + (pulses - 1) * this->press_interval_;
    ESP_LOGI(TAG, "Starting %s from %.0f%% to %.0f%%; %u-click press; stop press allowed after %lu ms.",
             op_name(dir), start_pos * 100.0f, target * 100.0f, pulses,
             (unsigned long) (this->stop_press_earliest_ - now));
    // Deadline backstop: 1.5x the estimated travel time for the requested
    // partial move. Whatever confuses the estimate, the door still gets a
    // stop press rather than running into the endstop.
    if (partial_target && (dir == COVER_OPERATION_OPENING || dir == COVER_OPERATION_CLOSING)) {
      float dur = (dir == COVER_OPERATION_OPENING) ? this->open_duration_ : this->close_duration_;
      float delta = (target > start_pos) ? (target - start_pos) : (start_pos - target);
      this->stop_press_deadline_ = this->stop_press_earliest_ + (uint32_t) (delta * dur * 1.5f);
    } else {
      this->stop_press_deadline_ = 0;
    }
    this->stop_prev_trigger_();
    trig->trigger();
    this->prev_command_trigger_ = trig;
  } else {
    // A takeover (perform_trigger false) means the door is already moving on
    // its own, so a stop press can be sent as soon as the estimate allows it.
    this->stop_press_earliest_ = now;
    this->stop_press_deadline_ = 0;
    ESP_LOGI(TAG, "External %s: from %.0f%% to %.0f%% (no press).", op_name(dir), start_pos * 100.0f,
             target * 100.0f);
  }
}

void GdoCover::recompute_position_() {
  float dir;
  float action_dur;
  float min_pos = COVER_CLOSED;
  float max_pos = COVER_OPEN;
  switch (this->current_operation) {
    case COVER_OPERATION_OPENING:
      dir = 1.0f;
      action_dur = this->open_duration_;
      // Only claim fully open once the endstop says so. Gate on the target
      // being COVER_OPEN, so a partial target of e.g. 0.995 stays reachable.
      if (this->open_endstop_ != nullptr && this->target_position_ == COVER_OPEN && !this->open_endstop_->state) {
        max_pos = ALMOST_OPEN;
      }
      break;
    case COVER_OPERATION_CLOSING:
      dir = -1.0f;
      action_dur = this->close_duration_;
      if (this->close_endstop_ != nullptr && this->target_position_ == COVER_CLOSED && !this->close_endstop_->state) {
        min_pos = ALMOST_CLOSED;
      }
      break;
    case COVER_OPERATION_IDLE:
    default:
      return;
  }
  const uint32_t now = millis();
  this->position += dir * (now - this->last_recompute_time_) / action_dur;
  this->position = clamp(this->position, min_pos, max_pos);
  this->last_recompute_time_ = now;
}

}  // namespace gdo
}  // namespace esphome

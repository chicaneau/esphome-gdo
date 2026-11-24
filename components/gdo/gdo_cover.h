#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/cover/cover.h"

namespace esphome {
namespace gdo {

class GdoCover : public cover::Cover, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  Trigger<> *get_single_press_trigger() { return &this->single_press_trigger_; }
  Trigger<> *get_double_press_trigger() { return &this->double_press_trigger_; }
  Trigger<> *get_triple_press_trigger() { return &this->triple_press_trigger_; }
  void set_open_endstop(binary_sensor::BinarySensor *open_endstop) { this->open_endstop_ = open_endstop; }
  void set_close_endstop(binary_sensor::BinarySensor *close_endstop) { this->close_endstop_ = close_endstop; }
  void set_open_duration(uint32_t open_duration) { this->open_duration_ = open_duration; }
  void set_close_duration(uint32_t close_duration) { this->close_duration_ = close_duration; }
  void set_start_delay(uint32_t start_delay) { this->start_delay_ = start_delay; }
  void set_press_interval(uint32_t press_interval) { this->press_interval_ = press_interval; }

  cover::CoverTraits get_traits() override;

 protected:
  void control(const cover::CoverCall &call) override;
  void stop_prev_trigger_();
  bool is_at_target_() const;

  void start_direction_(cover::CoverOperation dir, bool perform_trigger = true);

  void recompute_position_();

  void reconcile_endstop_state_();

  binary_sensor::BinarySensor *open_endstop_{nullptr};
  binary_sensor::BinarySensor *close_endstop_{nullptr};
  uint32_t open_duration_{0};
  uint32_t close_duration_{0};
  // Time between the last relay click of a start sequence and the moment the
  // opener has physically started moving. An auto stop press must not be sent
  // before that, or the opener (which still sees an idle door) treats it as a
  // new start press instead of a stop.
  uint32_t start_delay_{1000};
  // Interval between clicks of the double/triple press actions; the C++ side
  // cannot see the automation, so it must be configured to match the YAML.
  uint32_t press_interval_{160};
  uint32_t stop_press_earliest_{0};
  // Backstop for partial targets: the time by which the stop press must be
  // sent even if the position estimate never registers the target. 0 = none.
  uint32_t stop_press_deadline_{0};
  Trigger<> single_press_trigger_;
  Trigger<> double_press_trigger_;
  Trigger<> triple_press_trigger_;
  Trigger<> *prev_command_trigger_{nullptr};
  uint32_t last_recompute_time_{0};
  uint32_t start_dir_time_{0};
  uint32_t last_publish_time_{0};
  float target_position_{0};

  cover::CoverOperation last_direction_{cover::COVER_OPERATION_IDLE};
  // True while the move currently being tracked was started by one of our own
  // relay presses, false when it is an external takeover (no press). Used to
  // decide whether a contradictory endstop event can be chatter (door is
  // provably leaving that reed) or must be trusted (nothing proves the door
  // left; the reed sitting solidly ON means the door is really there).
  bool move_was_pressed_{true};
  // millis() of the last direction change, to bound how long a pressed move's
  // departure-chatter grace window stays in effect.
  uint32_t move_started_ms_{0};
  bool endstop_reconciled_{false};
};

}  // namespace gdo
}  // namespace esphome

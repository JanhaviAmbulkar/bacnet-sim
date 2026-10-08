// device.hpp - thread-safe model of a BACnet device with Analog-Input points.
// Readers use a shared lock; writers take a unique lock. Observers (Observer
// pattern) are notified *after* the lock is released so they can never deadlock
// against the model.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

struct ChangeObserver {
  virtual ~ChangeObserver() = default;
  virtual void on_point_changed(std::uint32_t ai_instance, float new_value) = 0;
};

struct Stats {
  std::atomic<std::uint64_t> udp_rx{0}, udp_tx{0}, tcp_cmds{0}, errors{0};
};

class DeviceModel {
 public:
  explicit DeviceModel(std::uint32_t device_instance) : device_(device_instance) {}

  std::uint32_t instance() const { return device_; }
  void add_point(std::uint32_t ai, float initial);
  std::optional<float> read(std::uint32_t ai) const;
  bool write(std::uint32_t ai, float value);  // false if the point does not exist
  std::vector<std::pair<std::uint32_t, float>> snapshot() const;
  void add_observer(std::shared_ptr<ChangeObserver> o);

  Stats stats;

 private:
  void notify(std::uint32_t ai, float v);

  const std::uint32_t device_;
  mutable std::shared_mutex points_mu_;
  std::map<std::uint32_t, float> points_;
  mutable std::mutex obs_mu_;
  std::vector<std::shared_ptr<ChangeObserver>> observers_;
};

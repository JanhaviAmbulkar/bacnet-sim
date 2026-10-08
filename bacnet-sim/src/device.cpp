#include "device.hpp"

void DeviceModel::add_point(std::uint32_t ai, float initial) {
  std::unique_lock<std::shared_mutex> lk(points_mu_);
  points_[ai] = initial;
}

std::optional<float> DeviceModel::read(std::uint32_t ai) const {
  std::shared_lock<std::shared_mutex> lk(points_mu_);
  const auto it = points_.find(ai);
  if (it == points_.end()) return std::nullopt;
  return it->second;
}

bool DeviceModel::write(std::uint32_t ai, float value) {
  {
    std::unique_lock<std::shared_mutex> lk(points_mu_);
    const auto it = points_.find(ai);
    if (it == points_.end()) return false;
    it->second = value;
  }
  notify(ai, value);  // lock released first
  return true;
}

std::vector<std::pair<std::uint32_t, float>> DeviceModel::snapshot() const {
  std::shared_lock<std::shared_mutex> lk(points_mu_);
  return {points_.begin(), points_.end()};
}

void DeviceModel::add_observer(std::shared_ptr<ChangeObserver> o) {
  std::lock_guard<std::mutex> lk(obs_mu_);
  observers_.push_back(std::move(o));
}

void DeviceModel::notify(std::uint32_t ai, float v) {
  std::vector<std::shared_ptr<ChangeObserver>> copy;
  {
    std::lock_guard<std::mutex> lk(obs_mu_);
    copy = observers_;
  }
  for (auto& o : copy) o->on_point_changed(ai, v);
}

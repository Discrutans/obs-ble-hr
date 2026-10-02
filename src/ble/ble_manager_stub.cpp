#include "ble_manager.hpp"

#include <obs-module.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace ble_hr {

struct BleHeartRateManager::Impl {
	mutable std::mutex mutex;
	BpmCallback bpm_cb;
	StateCallback state_cb;
	ConnectionState conn_state = ConnectionState::Disconnected;
	std::string device_id;
	int bpm = 0;
	std::chrono::steady_clock::time_point last_bpm_time{};
};

BleHeartRateManager::BleHeartRateManager() : impl_(new Impl) {}

BleHeartRateManager::~BleHeartRateManager()
{
	delete impl_;
	impl_ = nullptr;
}

std::vector<DeviceInfo> BleHeartRateManager::scan(std::chrono::milliseconds)
{
	blog(LOG_WARNING, "[ble-heart-rate] BLE Heart Rate is not supported on this platform");
	return {};
}

void BleHeartRateManager::connect(const std::string &)
{
	blog(LOG_WARNING, "[ble-heart-rate] BLE Heart Rate is not supported on this platform");
}

void BleHeartRateManager::disconnect() {}

void BleHeartRateManager::set_bpm_callback(BpmCallback cb)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->bpm_cb = std::move(cb);
}

void BleHeartRateManager::set_state_callback(StateCallback cb)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->state_cb = std::move(cb);
}

ConnectionState BleHeartRateManager::state() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->conn_state;
}

std::string BleHeartRateManager::last_device_id() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->device_id;
}

bool BleHeartRateManager::has_recent_bpm(std::chrono::milliseconds) const
{
	return false;
}

int BleHeartRateManager::last_bpm() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->bpm;
}

const char *state_to_string(ConnectionState state)
{
	switch (state) {
	case ConnectionState::Disconnected:
		return "disconnected";
	case ConnectionState::Connecting:
		return "connecting";
	case ConnectionState::Connected:
		return "connected";
	case ConnectionState::Reconnecting:
		return "reconnecting";
	}
	return "unknown";
}

} /* namespace ble_hr */

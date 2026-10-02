#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ble_hr {

struct DeviceInfo {
	std::string id;
	std::string name;
	int rssi = 0;
};

enum class ConnectionState {
	Disconnected,
	Connecting,
	Connected,
	Reconnecting,
};

class BleHeartRateManager {
public:
	using BpmCallback = std::function<void(int bpm)>;
	using StateCallback = std::function<void(ConnectionState state, const std::string &message)>;

	BleHeartRateManager();
	~BleHeartRateManager();

	BleHeartRateManager(const BleHeartRateManager &) = delete;
	BleHeartRateManager &operator=(const BleHeartRateManager &) = delete;

	/* Blocking scan (runs on internal worker). Safe to call from OBS UI thread. */
	std::vector<DeviceInfo> scan(std::chrono::milliseconds duration);

	void connect(const std::string &device_id);
	void disconnect();

	void set_bpm_callback(BpmCallback cb);
	void set_state_callback(StateCallback cb);

	ConnectionState state() const;
	std::string last_device_id() const;
	bool has_recent_bpm(std::chrono::milliseconds max_age) const;
	int last_bpm() const;

private:
	struct Impl;
	Impl *impl_;
};

const char *state_to_string(ConnectionState state);

} /* namespace ble_hr */

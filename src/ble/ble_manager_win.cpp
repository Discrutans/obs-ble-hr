#include "ble_manager.hpp"

#include <obs-module.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <queue>
#include <thread>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

namespace ble_hr {
namespace {

int parse_heart_rate_measurement(const DataReader &reader, uint32_t length)
{
	if (length < 2)
		return -1;

	const uint8_t flags = reader.ReadByte();
	const bool bpm_uint16 = (flags & 0x01) != 0;

	if (bpm_uint16) {
		if (length < 3)
			return -1;
		return static_cast<int>(reader.ReadUInt16());
	}

	return static_cast<int>(reader.ReadByte());
}

std::string hstring_to_utf8(const hstring &value)
{
	const std::wstring wide(value.c_str());
	if (wide.empty())
		return {};

	const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};

	std::string out(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), out.data(), size, nullptr, nullptr);
	return out;
}

std::string address_to_id(uint64_t address)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%012llX", static_cast<unsigned long long>(address));
	return buf;
}

} /* namespace */

struct BleHeartRateManager::Impl {
	enum class CommandType { Scan, Connect, Disconnect, Shutdown };

	struct Command {
		CommandType type = CommandType::Shutdown;
		std::string device_id;
		std::chrono::milliseconds scan_duration{0};
	};

	mutable std::mutex mutex;
	std::condition_variable cv;
	std::queue<Command> commands;
	std::thread worker;
	bool running = false;

	BpmCallback bpm_cb;
	StateCallback state_cb;

	ConnectionState conn_state = ConnectionState::Disconnected;
	std::string device_id;
	int bpm = 0;
	std::chrono::steady_clock::time_point last_bpm_time{};

	std::mutex scan_mutex;
	std::condition_variable scan_cv;
	bool scan_done = false;
	std::vector<DeviceInfo> scan_results;

	BluetoothLEDevice device{nullptr};
	GattDeviceService hr_service{nullptr};
	GattCharacteristic hr_char{nullptr};
	event_token value_changed_token{};
	event_token connection_status_token{};
	bool auto_reconnect = true;
	std::chrono::steady_clock::time_point next_reconnect_attempt{};

	void set_state(ConnectionState state, const std::string &message)
	{
		StateCallback cb;
		{
			std::lock_guard<std::mutex> lock(mutex);
			conn_state = state;
			cb = state_cb;
		}
		if (cb)
			cb(state, message);
		blog(LOG_INFO, "[ble-heart-rate] BLE state: %s (%s)", state_to_string(state), message.c_str());
	}

	void publish_bpm(int value)
	{
		BpmCallback cb;
		{
			std::lock_guard<std::mutex> lock(mutex);
			bpm = value;
			last_bpm_time = std::chrono::steady_clock::now();
			cb = bpm_cb;
		}
		if (cb)
			cb(value);
	}

	void clear_gatt()
	{
		if (hr_char && value_changed_token) {
			try {
				hr_char.ValueChanged(value_changed_token);
			} catch (...) {
			}
			value_changed_token = {};
		}
		hr_char = nullptr;

		if (hr_service) {
			try {
				hr_service.Close();
			} catch (...) {
			}
			hr_service = nullptr;
		}

		if (device && connection_status_token) {
			try {
				device.ConnectionStatusChanged(connection_status_token);
			} catch (...) {
			}
			connection_status_token = {};
		}

		if (device) {
			try {
				device.Close();
			} catch (...) {
			}
			device = nullptr;
		}
	}

	void handle_disconnect(const std::string &reason)
	{
		clear_gatt();
		bool should_reconnect = false;
		std::string id;
		{
			std::lock_guard<std::mutex> lock(mutex);
			should_reconnect = auto_reconnect && !device_id.empty();
			id = device_id;
		}

		if (should_reconnect) {
			set_state(ConnectionState::Reconnecting, reason);
			next_reconnect_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			enqueue({CommandType::Connect, id, {}});
		} else {
			set_state(ConnectionState::Disconnected, reason);
		}
	}

	void enqueue(Command cmd)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!running && cmd.type != CommandType::Shutdown)
				return;
			commands.push(std::move(cmd));
		}
		cv.notify_one();
	}

	std::vector<DeviceInfo> do_scan(std::chrono::milliseconds duration)
	{
		std::map<uint64_t, DeviceInfo> found;

		BluetoothLEAdvertisementWatcher watcher;
		watcher.ScanningMode(BluetoothLEScanningMode::Active);

		auto token = watcher.Received([&](const BluetoothLEAdvertisementWatcher &,
						  const BluetoothLEAdvertisementReceivedEventArgs &args) {
			const auto adv = args.Advertisement();
			std::string name = hstring_to_utf8(adv.LocalName());

			bool has_hr = false;
			const auto hr_uuid = GattServiceUuids::HeartRate();
			for (const auto &uuid : adv.ServiceUuids()) {
				if (uuid == hr_uuid) {
					has_hr = true;
					break;
				}
			}

			/* Keep unnamed devices only if they advertise HR service. */
			if (name.empty() && !has_hr)
				return;

			if (name.empty())
				name = address_to_id(args.BluetoothAddress());

			DeviceInfo info;
			info.id = address_to_id(args.BluetoothAddress());
			info.name = name;
			info.rssi = args.RawSignalStrengthInDBm();

			found[args.BluetoothAddress()] = info;
		});

		watcher.Start();
		std::this_thread::sleep_for(duration);
		watcher.Stop();
		watcher.Received(token);

		std::vector<DeviceInfo> results;
		results.reserve(found.size());
		for (auto &pair : found)
			results.push_back(std::move(pair.second));

		std::sort(results.begin(), results.end(), [](const DeviceInfo &a, const DeviceInfo &b) {
			return a.rssi > b.rssi;
		});
		return results;
	}

	bool subscribe_hr()
	{
		const auto status =
			hr_char.WriteClientCharacteristicConfigurationDescriptorAsync(
					GattClientCharacteristicConfigurationDescriptorValue::Notify)
				.get();
		if (status != GattCommunicationStatus::Success) {
			blog(LOG_WARNING, "[ble-heart-rate] Failed to enable HR notifications (%d)", (int)status);
			return false;
		}

		value_changed_token = hr_char.ValueChanged(
			[this](const GattCharacteristic &, const GattValueChangedEventArgs &args) {
				try {
					DataReader reader = DataReader::FromBuffer(args.CharacteristicValue());
					reader.ByteOrder(ByteOrder::LittleEndian);
					const int value =
						parse_heart_rate_measurement(reader, args.CharacteristicValue().Length());
					if (value >= 0)
						publish_bpm(value);
				} catch (const hresult_error &ex) {
					blog(LOG_WARNING, "[ble-heart-rate] HR parse error: %ls", ex.message().c_str());
				}
			});

		return true;
	}

	bool do_connect(const std::string &id)
	{
		clear_gatt();
		set_state(ConnectionState::Connecting, id);

		uint64_t address = 0;
		try {
			address = std::stoull(id, nullptr, 16);
		} catch (...) {
			set_state(ConnectionState::Disconnected, "Invalid device id");
			return false;
		}

		try {
			device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
			if (!device) {
				set_state(ConnectionState::Disconnected, "Device not found");
				return false;
			}

			{
				std::lock_guard<std::mutex> lock(mutex);
				device_id = id;
				auto_reconnect = true;
			}

			connection_status_token = device.ConnectionStatusChanged(
				[this](const BluetoothLEDevice &sender, const IInspectable &) {
					if (sender.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
						handle_disconnect("Link lost");
				});

			auto services_result =
				device.GetGattServicesForUuidAsync(GattServiceUuids::HeartRate()).get();
			if (services_result.Status() != GattCommunicationStatus::Success ||
			    services_result.Services().Size() == 0) {
				clear_gatt();
				set_state(ConnectionState::Disconnected, "Heart Rate service missing");
				return false;
			}

			hr_service = services_result.Services().GetAt(0);
			auto chars_result =
				hr_service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::HeartRateMeasurement())
					.get();
			if (chars_result.Status() != GattCommunicationStatus::Success ||
			    chars_result.Characteristics().Size() == 0) {
				clear_gatt();
				set_state(ConnectionState::Disconnected, "HR Measurement missing");
				return false;
			}

			hr_char = chars_result.Characteristics().GetAt(0);
			if (!subscribe_hr()) {
				clear_gatt();
				set_state(ConnectionState::Disconnected, "Notify subscribe failed");
				return false;
			}

			set_state(ConnectionState::Connected, id);
			return true;
		} catch (const hresult_error &ex) {
			clear_gatt();
			const std::string msg = hstring_to_utf8(ex.message());
			set_state(ConnectionState::Disconnected, msg.empty() ? "Connect failed" : msg);
			return false;
		}
	}

	void do_disconnect(bool allow_reconnect)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto_reconnect = allow_reconnect;
			if (!allow_reconnect)
				device_id.clear();
		}
		clear_gatt();
		set_state(ConnectionState::Disconnected, allow_reconnect ? "Temporary disconnect" : "Disconnected");
	}

	void worker_main()
	{
		winrt::init_apartment(apartment_type::multi_threaded);

		while (true) {
			Command cmd;
			{
				std::unique_lock<std::mutex> lock(mutex);
				cv.wait_for(lock, std::chrono::milliseconds(500),
					    [this] { return !commands.empty() || !running; });

				if (!running && commands.empty())
					break;

				if (commands.empty()) {
					/* Reconnect watchdog */
					const bool need_reconnect =
						auto_reconnect && !device_id.empty() &&
						(conn_state == ConnectionState::Reconnecting ||
						 conn_state == ConnectionState::Disconnected) &&
						std::chrono::steady_clock::now() >= next_reconnect_attempt;
					if (need_reconnect) {
						cmd = {CommandType::Connect, device_id, {}};
					} else {
						continue;
					}
				} else {
					cmd = std::move(commands.front());
					commands.pop();
				}
			}

			switch (cmd.type) {
			case CommandType::Scan: {
				auto results = do_scan(cmd.scan_duration);
				{
					std::lock_guard<std::mutex> lock(scan_mutex);
					scan_results = std::move(results);
					scan_done = true;
				}
				scan_cv.notify_all();
				break;
			}
			case CommandType::Connect:
				do_connect(cmd.device_id);
				break;
			case CommandType::Disconnect:
				do_disconnect(false);
				break;
			case CommandType::Shutdown:
				do_disconnect(false);
				return;
			}
		}
	}
};

BleHeartRateManager::BleHeartRateManager() : impl_(new Impl)
{
	impl_->running = true;
	impl_->worker = std::thread([this] { impl_->worker_main(); });
}

BleHeartRateManager::~BleHeartRateManager()
{
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->running = false;
		impl_->auto_reconnect = false;
		impl_->commands.push({Impl::CommandType::Shutdown, {}, {}});
	}
	impl_->cv.notify_all();
	{
		std::lock_guard<std::mutex> lock(impl_->scan_mutex);
		impl_->scan_done = true;
	}
	impl_->scan_cv.notify_all();

	if (impl_->worker.joinable())
		impl_->worker.join();

	delete impl_;
	impl_ = nullptr;
}

std::vector<DeviceInfo> BleHeartRateManager::scan(std::chrono::milliseconds duration)
{
	{
		std::lock_guard<std::mutex> lock(impl_->scan_mutex);
		impl_->scan_done = false;
		impl_->scan_results.clear();
	}

	impl_->enqueue({Impl::CommandType::Scan, {}, duration});

	std::unique_lock<std::mutex> lock(impl_->scan_mutex);
	impl_->scan_cv.wait(lock, [this] { return impl_->scan_done; });
	return impl_->scan_results;
}

void BleHeartRateManager::connect(const std::string &device_id)
{
	if (device_id.empty())
		return;
	impl_->enqueue({Impl::CommandType::Connect, device_id, {}});
}

void BleHeartRateManager::disconnect()
{
	impl_->enqueue({Impl::CommandType::Disconnect, {}, {}});
}

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

bool BleHeartRateManager::has_recent_bpm(std::chrono::milliseconds max_age) const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (impl_->last_bpm_time.time_since_epoch().count() == 0)
		return false;
	return (std::chrono::steady_clock::now() - impl_->last_bpm_time) <= max_age;
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

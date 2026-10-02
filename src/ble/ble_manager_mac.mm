#include "ble_manager.hpp"

#include <obs-module.h>

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

@interface BleHrDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, assign) void *manager;
@end

namespace ble_hr {

static std::string ns_to_utf8(NSString *value)
{
	if (!value)
		return {};
	const char *utf8 = [value UTF8String];
	return utf8 ? std::string(utf8) : std::string();
}

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
	bool auto_reconnect = true;
	std::chrono::steady_clock::time_point next_reconnect_attempt{};

	std::mutex scan_mutex;
	std::condition_variable scan_cv;
	bool scan_done = false;
	std::vector<DeviceInfo> scan_results;
	std::map<std::string, DeviceInfo> scan_map;

	CFRunLoopRef run_loop = nullptr;
	CBCentralManager *central = nil;
	CBPeripheral *peripheral = nil;
	BleHrDelegate *delegate = nil;
	bool central_ready = false;
	std::condition_variable ready_cv;

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

	void enqueue(Command cmd)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!running && cmd.type != CommandType::Shutdown)
				return;
			commands.push(std::move(cmd));
		}
		cv.notify_one();
		if (run_loop)
			CFRunLoopWakeUp(run_loop);
	}

	void clear_peripheral()
	{
		if (central && peripheral)
			[central cancelPeripheralConnection:peripheral];
		peripheral = nil;
	}

	void handle_disconnect(const std::string &reason)
	{
		clear_peripheral();
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

	void start_scan(std::chrono::milliseconds duration)
	{
		{
			std::lock_guard<std::mutex> lock(scan_mutex);
			scan_map.clear();
			scan_results.clear();
			scan_done = false;
		}

		CBUUID *hr = [CBUUID UUIDWithString:@"180D"];
		[central scanForPeripheralsWithServices:@[hr]
						options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @NO}];

		const auto deadline = std::chrono::steady_clock::now() + duration;
		while (std::chrono::steady_clock::now() < deadline) {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
		}

		[central stopScan];

		{
			std::lock_guard<std::mutex> lock(scan_mutex);
			scan_results.clear();
			for (auto &pair : scan_map)
				scan_results.push_back(pair.second);
			std::sort(scan_results.begin(), scan_results.end(),
				  [](const DeviceInfo &a, const DeviceInfo &b) { return a.rssi > b.rssi; });
			scan_done = true;
		}
		scan_cv.notify_all();
	}

	void do_connect(const std::string &id)
	{
		set_state(ConnectionState::Connecting, id);
		NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:[NSString stringWithUTF8String:id.c_str()]];
		if (!uuid) {
			set_state(ConnectionState::Disconnected, "Invalid device id");
			return;
		}

		NSArray<CBPeripheral *> *known = [central retrievePeripheralsWithIdentifiers:@[uuid]];
		if (known.count == 0) {
			set_state(ConnectionState::Disconnected, "Device not found");
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex);
			device_id = id;
			auto_reconnect = true;
		}

		peripheral = known[0];
		peripheral.delegate = delegate;
		[central connectPeripheral:peripheral options:nil];
	}

	void do_disconnect(bool allow_reconnect)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto_reconnect = allow_reconnect;
			if (!allow_reconnect)
				device_id.clear();
		}
		clear_peripheral();
		set_state(ConnectionState::Disconnected, allow_reconnect ? "Temporary disconnect" : "Disconnected");
	}

	void worker_main()
	{
		@autoreleasepool {
			run_loop = CFRunLoopGetCurrent();
			delegate = [[BleHrDelegate alloc] init];
			delegate.manager = this;
			central = [[CBCentralManager alloc] initWithDelegate:delegate queue:nil];

			while (true) {
				Command cmd;
				bool have_cmd = false;
				{
					std::unique_lock<std::mutex> lock(mutex);
					if (commands.empty()) {
						lock.unlock();
						CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
						lock.lock();
					}

					if (!running && commands.empty())
						break;

					if (commands.empty()) {
						const bool need_reconnect =
							auto_reconnect && !device_id.empty() &&
							(conn_state == ConnectionState::Reconnecting ||
							 conn_state == ConnectionState::Disconnected) &&
							std::chrono::steady_clock::now() >= next_reconnect_attempt;
						if (need_reconnect)
							cmd = {CommandType::Connect, device_id, {}};
						else
							continue;
					} else {
						cmd = std::move(commands.front());
						commands.pop();
					}
					have_cmd = true;
				}

				if (!have_cmd)
					continue;

				switch (cmd.type) {
				case CommandType::Scan:
					start_scan(cmd.scan_duration);
					break;
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
	}

	void on_peripheral_discovered(CBPeripheral *p, NSDictionary *adv, NSNumber *rssi)
	{
		DeviceInfo info;
		info.id = ns_to_utf8(p.identifier.UUIDString);
		NSString *local = adv[CBAdvertisementDataLocalNameKey];
		info.name = ns_to_utf8(local && local.length ? local : p.name);
		if (info.name.empty())
			info.name = info.id;
		info.rssi = rssi ? rssi.intValue : 0;

		std::lock_guard<std::mutex> lock(scan_mutex);
		scan_map[info.id] = info;
	}

	void on_connected(CBPeripheral *p)
	{
		CBUUID *hr = [CBUUID UUIDWithString:@"180D"];
		[p discoverServices:@[hr]];
	}

	void on_services(CBPeripheral *p)
	{
		for (CBService *service in p.services) {
			if ([service.UUID isEqual:[CBUUID UUIDWithString:@"180D"]]) {
				[p discoverCharacteristics:@[[CBUUID UUIDWithString:@"2A37"]] forService:service];
				return;
			}
		}
		handle_disconnect("Heart Rate service missing");
	}

	void on_characteristics(CBPeripheral *p, CBService *service)
	{
		for (CBCharacteristic *ch in service.characteristics) {
			if ([ch.UUID isEqual:[CBUUID UUIDWithString:@"2A37"]]) {
				[p setNotifyValue:YES forCharacteristic:ch];
				set_state(ConnectionState::Connected, device_id);
				return;
			}
		}
		handle_disconnect("HR Measurement missing");
	}

	void on_value(CBCharacteristic *ch)
	{
		NSData *data = ch.value;
		if (!data || data.length < 2)
			return;

		const uint8_t *bytes = static_cast<const uint8_t *>(data.bytes);
		const uint8_t flags = bytes[0];
		int value = -1;
		if (flags & 0x01) {
			if (data.length >= 3)
				value = bytes[1] | (bytes[2] << 8);
		} else {
			value = bytes[1];
		}
		if (value >= 0)
			publish_bpm(value);
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
	if (impl_->run_loop)
		CFRunLoopWakeUp(impl_->run_loop);
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

@implementation BleHrDelegate
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (!impl)
		return;
	if (central.state == CBManagerStatePoweredOn) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->central_ready = true;
		impl->ready_cv.notify_all();
	}
}

- (void)centralManager:(CBCentralManager *)central
	didDiscoverPeripheral:(CBPeripheral *)peripheral
	    advertisementData:(NSDictionary<NSString *, id> *)advertisementData
			 RSSI:(NSNumber *)RSSI
{
	UNUSED_PARAMETER(central);
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (impl)
		impl->on_peripheral_discovered(peripheral, advertisementData, RSSI);
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
	UNUSED_PARAMETER(central);
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (impl)
		impl->on_connected(peripheral);
}

- (void)centralManager:(CBCentralManager *)central
	didFailToConnectPeripheral:(CBPeripheral *)peripheral
			     error:(NSError *)error
{
	UNUSED_PARAMETER(central);
	UNUSED_PARAMETER(peripheral);
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (impl)
		impl->handle_disconnect(error ? ble_hr::ns_to_utf8(error.localizedDescription) : "Connect failed");
}

- (void)centralManager:(CBCentralManager *)central
	didDisconnectPeripheral:(CBPeripheral *)peripheral
			  error:(NSError *)error
{
	UNUSED_PARAMETER(central);
	UNUSED_PARAMETER(peripheral);
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (impl)
		impl->handle_disconnect(error ? ble_hr::ns_to_utf8(error.localizedDescription) : "Link lost");
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (!impl)
		return;
	if (error) {
		impl->handle_disconnect(ble_hr::ns_to_utf8(error.localizedDescription));
		return;
	}
	impl->on_services(peripheral);
}

- (void)peripheral:(CBPeripheral *)peripheral
	didDiscoverCharacteristicsForService:(CBService *)service
				       error:(NSError *)error
{
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (!impl)
		return;
	if (error) {
		impl->handle_disconnect(ble_hr::ns_to_utf8(error.localizedDescription));
		return;
	}
	impl->on_characteristics(peripheral, service);
}

- (void)peripheral:(CBPeripheral *)peripheral
	didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
				  error:(NSError *)error
{
	UNUSED_PARAMETER(peripheral);
	auto *impl = static_cast<ble_hr::BleHeartRateManager::Impl *>(self.manager);
	if (!impl)
		return;
	if (error)
		return;
	impl->on_value(characteristic);
}
@end

#include <obs-module.h>
#include <plugin-support.h>

#include "hr-ble-source.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "BLE Heart Rate sensor overlay for OBS Studio";
}

bool obs_module_load(void)
{
	obs_register_source(&hr_ble_source_info);
	obs_log(LOG_INFO, "BLE Heart Rate loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "BLE Heart Rate unloaded");
}

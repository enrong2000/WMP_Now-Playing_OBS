#include <obs-module.h>

#include "wmp_source.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-wmp-smtc", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows Media Player Legacy now-playing source via COM automation";
}

bool obs_module_load(void)
{
	obs_register_source(&obs_wmp::wmp_source_info);
	blog(LOG_INFO, "[obs-wmp-smtc] plugin loaded (WMP COM backend)");
	return true;
}

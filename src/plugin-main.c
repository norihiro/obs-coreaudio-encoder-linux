/*
 * OBS CoreAudio Encoder Plugin for Linux
 * Copyright (C) 2024 Norihiro Kamae <norihiro@nagater.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <obs-module.h>
#include "plugin-macros.generated.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

void register_aac_info();
obs_properties_t *aac_properties(void *data);

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Apple CoreAudio based encoder for Linux";
}

bool obs_module_load(void)
{
	obs_properties_t *prop = aac_properties(NULL);
	if (!prop) {
		blog(LOG_ERROR, "CoreAudio AAC encoder not installed on the system or couldn't be loaded");
		return false;
	}
	obs_properties_destroy(prop);

	register_aac_info();

	blog(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

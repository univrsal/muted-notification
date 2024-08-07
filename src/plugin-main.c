/**
 ** This file is part of urmuted.
 ** Copyright 2023 Alex <uni@vrsal.xyz>.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **/

/**
 * Most of the logic is directly taken from the obs noise gate filter:
 * https://github.com/obsproject/obs-studio/blob/master/plugins/obs-filters/noise-gate-filter.c
 */
#include <media-io/audio-math.h>
#include <obs-module.h>
#include <util/platform.h>

#include "miniaudio.h"

#include "indicator.h"

#define write_log(log_level, format, ...) \
	blog(log_level, "[muted-notification] " format, ##__VA_ARGS__)

#if defined(_DEBUG)
#define bdebug(format, ...)       \
	write_log(                \
		LOG_INFO, format, \
		##__VA_ARGS__) // apparently debug level isn't logged by default, so we just use info level for debug builds
#else
#define bdebug(format, ...) write_log(LOG_DEBUG, format, ##__VA_ARGS__)
#endif
#define binfo(format, ...) write_log(LOG_INFO, format, ##__VA_ARGS__)
#define bwarn(format, ...) write_log(LOG_WARNING, format, ##__VA_ARGS__)
#define berr(format, ...) write_log(LOG_ERROR, format, ##__VA_ARGS__)

/* clang-format off */
#define S_COOLDOWN              "cooldown"
#define S_OPEN_THRESHOLD        "open_threshold"
#define S_CLOSE_THRESHOLD       "close_threshold"
#define S_ATTACK_TIME           "attack_time"
#define S_HOLD_TIME             "hold_time"
#define S_RELEASE_TIME          "release_time"
#define S_FILE                  "file"
#define S_DEVICE                "device"
#define S_AUDIO_INDICATOR       "audio_indicator"
#define S_VISUAL_INDICATOR      "visual_indicator"
#define S_VISUAL_INDICATOR_SIZE "visual_indicator_size"
#define S_NOISE_GATE_GROUP      "ng_group"
#define S_VISUAL_GROUP          "visual_group"
#define S_AUDIO_GROUP           "audio_group"

#define MT_                            obs_module_text
#define TEXT_OPEN_THRESHOLD            MT_("NoiseGate.OpenThreshold")
#define TEXT_CLOSE_THRESHOLD           MT_("NoiseGate.CloseThreshold")
#define TEXT_ATTACK_TIME               MT_("NoiseGate.AttackTime")
#define TEXT_HOLD_TIME                 MT_("NoiseGate.HoldTime")
#define TEXT_RELEASE_TIME              MT_("NoiseGate.ReleaseTime")
#define TEXT_COOLDOWN                  MT_("Cooldown")
#define TEXT_FILE                      MT_("File")
#define TEXT_DEVICE                    MT_("Device")
#define TEXT_AUDIO_INDICATOR           MT_("Enabled")
#define TEXT_VISUAL_INDICATOR          MT_("Enabled")
#define TEXT_VISUAL_INDICATOR_SIZE     MT_("VisualIndicatorSize")
#define TEXT_NOISE_GATE_GROUP          MT_("NoiseGate")
#define TEXT_VISUAL_GROUP              MT_("Group.VisualIndicator")
#define TEXT_AUDIO_GROUP               MT_("Group.AudioIndicator")

#define VOL_MIN -96.0
#define VOL_MAX 0.0

/* clang-format on */

struct muted_data {
    obs_source_t *context;
    char *file_path;
    char *device;
    bool ma_initialized;
    ma_context ma_context;
    ma_device_config ma_config;
    ma_device ma_device;
    ma_decoder ma_decoder;
    ma_log ma_log;

    float sample_rate_i;
    size_t channels;

    float open_threshold;
    float close_threshold;
    float decay_rate;
    float attack_rate;
    float release_rate;
    float hold_time;

    bool is_open;
    float attenuation;
    float level;
    float held_time;

    uint64_t cooldown;
    uint64_t last_play_time;
    uint64_t file_length;

    bool audio_indicator;
    bool visual_indicator;

    int indicator_size;
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("muted_notification", "en-US")

static void log_callback(void *ctx, ma_uint32 level, const char *message)
{
    struct muted_data *data = ctx;
    const char *filter_name = obs_source_get_name(data->context);
    obs_source_t *parent = obs_filter_get_parent(data->context);
    const char *filter_parent_name = "";
    if (parent)
        filter_parent_name = obs_source_get_name(parent);

    switch (level) {
    case MA_LOG_LEVEL_INFO:
        blog(LOG_DEBUG, "[%s %s] miniaudio: %s", filter_name, filter_parent_name, message);
        break;
    case MA_LOG_LEVEL_DEBUG:
        blog(LOG_DEBUG, "[%s %s] miniaudio: %s", filter_name, filter_parent_name, message);
        break;
    case MA_LOG_LEVEL_WARNING:
        blog(LOG_WARNING, "[%s %s] miniaudio: %s", filter_name, filter_parent_name, message);
        break;
    case MA_LOG_LEVEL_ERROR:
        blog(LOG_ERROR, "[%s %s] miniaudio: %s", filter_name, filter_parent_name, message);
        break;
    }
}

static void populate_list(struct muted_data *d, obs_property_t *list)
{
    ma_result result;
    ma_device_info *playack_devices;
    ma_uint32 playback_device_count;
    obs_property_list_clear(list);

    result = ma_context_get_devices(&d->ma_context, &playack_devices, &playback_device_count, NULL, NULL);
    if (result == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < playback_device_count; ++i) {
            obs_property_list_add_string(list, playack_devices[i].name, playack_devices[i].name);
        }
    } else {
        blog(LOG_ERROR, "ma_context_get_devices failed");
    }
}

static void play_audio(struct muted_data *data)
{
    ma_result result = ma_device_start(&data->ma_device);
    ma_decoder_seek_to_pcm_frame(&data->ma_decoder, 0);
    blog(LOG_DEBUG, "Playing audio");
    if (result != MA_SUCCESS) {
        blog(LOG_ERROR, "Failed to start playback.");
    }
}

static const char *muted_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Muted notification";
}

static void free_device(struct muted_data *d)
{
    ma_device_uninit(&d->ma_device);
    bfree(d->device);
    d->device = NULL;
    memset(&d->ma_config, 0, sizeof(ma_device_config));
}

static void free_wav(struct muted_data *d)
{
    ma_decoder_uninit(&d->ma_decoder);
    bfree(d->file_path);
    d->file_path = NULL;
}

static void muted_destroy(void *data)
{
    struct muted_data *ng = data;
    free_wav(data);
    free_device(data);
    ma_context_uninit(&ng->ma_context);
    ma_log_uninit(&ng->ma_log);
    bfree(ng->file_path);
    bfree(ng->device);
    bfree(ng);
}

static inline float ms_to_secf(int ms)
{
    return (float)ms / 1000.0f;
}

static void load_wav(struct muted_data *d, const char *path)
{
    ma_result result = ma_decoder_init_file(path, NULL, &d->ma_decoder);
    if (result == MA_SUCCESS) {
        bfree(d->file_path);
        d->file_path = bstrdup(path);

        ma_uint64 frameCount;
        result = ma_decoder_get_length_in_pcm_frames(&d->ma_decoder, &frameCount);
        if (result == MA_SUCCESS) {
            d->file_length = (frameCount / d->ma_decoder.outputSampleRate) * 1000;
            blog(LOG_DEBUG, "'%s' is %i ms long", path, (int)d->file_length); // macos won't use %llu so screw it
        } else {
            blog(LOG_ERROR, "ma_decoder_get_length_in_pcm_frames failed for '%s'", path);
        }

    } else {
        blog(LOG_ERROR, "Failed to open '%s'", path);
    }
}

static void playback_cb(ma_device *dev, void *output, const void *input, ma_uint32 frame_count)
{
    UNUSED_PARAMETER(input);
    struct muted_data *data = dev->pUserData;
    ma_uint64 read = 0;
    while (frame_count > 0) {
        ma_result res = ma_decoder_read_pcm_frames(&data->ma_decoder, output, frame_count, &read);
        if (res != MA_SUCCESS) {
            if (res == MA_AT_END) {
                break;
            }
            blog(LOG_ERROR, "Error in playback callback");
            break;
        }
        frame_count -= (ma_uint32)read;
    }
}

static void open_device(struct muted_data *d, const char *device)
{
    ma_device_info *pPlaybackDevices;
    ma_uint32 playbackDeviceCount;
    ma_result result = ma_context_get_devices(&d->ma_context, &pPlaybackDevices, &playbackDeviceCount, NULL, NULL);
    ma_device_config deviceConfig;
    ma_device_info *pPlaybackDevice = NULL;

    if (result != MA_SUCCESS) {
        blog(LOG_ERROR, "Failed to enumerate audio devices.");
        return;
    }

    // Find the device with the specified name
    for (ma_uint32 i = 0; i < playbackDeviceCount; ++i) {
        if (strcmp(pPlaybackDevices[i].name, device) == 0) {
            pPlaybackDevice = &pPlaybackDevices[i];
            break;
        }
    }

    if (pPlaybackDevice == NULL) {
        blog(LOG_ERROR, "Failed to find playback device with name '%s'", device);
        return;
    }

    // open device using the info in ma_device_info
    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = d->ma_decoder.outputFormat;
    deviceConfig.playback.channels = d->ma_decoder.outputChannels;
    deviceConfig.sampleRate = d->ma_decoder.outputSampleRate;
    deviceConfig.dataCallback = playback_cb;
    deviceConfig.pUserData = d;
    deviceConfig.playback.pDeviceID = &pPlaybackDevice->id;

    result = ma_device_init(&d->ma_context, &deviceConfig, &d->ma_device);

    if (result == MA_SUCCESS) {
        blog(LOG_INFO, "Opened '%s'", device);
        bfree(d->device);
        d->device = bstrdup(device);
    } else {
        blog(LOG_ERROR, "Failed to open playback device '%s'", device);
    }
}

static void muted_update(void *data, obs_data_t *s)
{
    struct muted_data *ng = data;
    float open_threshold_db;
    float close_threshold_db;
    float sample_rate;
    int attack_time_ms;
    int hold_time_ms;
    int release_time_ms;
    const char *device;
    const char *path;

    path = obs_data_get_string(s, S_FILE);
    device = obs_data_get_string(s, S_DEVICE);

    open_threshold_db = (float)obs_data_get_double(s, S_OPEN_THRESHOLD);
    close_threshold_db = (float)obs_data_get_double(s, S_CLOSE_THRESHOLD);
    attack_time_ms = (int)obs_data_get_int(s, S_ATTACK_TIME);
    hold_time_ms = (int)obs_data_get_int(s, S_HOLD_TIME);
    release_time_ms = (int)obs_data_get_int(s, S_RELEASE_TIME);
    sample_rate = (float)audio_output_get_sample_rate(obs_get_audio());

    ng->cooldown = (int)obs_data_get_int(s, S_COOLDOWN);

    ng->audio_indicator = obs_data_get_bool(s, S_AUDIO_INDICATOR);
    ng->visual_indicator = obs_data_get_bool(s, S_VISUAL_INDICATOR);
    ng->indicator_size = (int)obs_data_get_int(s, S_VISUAL_INDICATOR_SIZE);


    ng->sample_rate_i = 1.0f / sample_rate;
    ng->channels = audio_output_get_channels(obs_get_audio());
    ng->open_threshold = db_to_mul(open_threshold_db);
    ng->close_threshold = db_to_mul(close_threshold_db);
    ng->attack_rate = 1.0f / (ms_to_secf(attack_time_ms) * sample_rate);
    ng->release_rate = 1.0f / (ms_to_secf(release_time_ms) * sample_rate);

    const float threshold_diff = ng->open_threshold - ng->close_threshold;
    const float min_decay_period = (1.0f / 75.0f) * sample_rate;

    ng->decay_rate = threshold_diff / min_decay_period;
    ng->hold_time = ms_to_secf(hold_time_ms);
    ng->is_open = false;
    ng->attenuation = 0.0f;
    ng->level = 0.0f;
    ng->held_time = 0.0f;

    if (!ng->file_path || strcmp(path, ng->file_path) != 0) {
        free_wav(data);
        load_wav(data, path);
        free_device(ng);
        open_device(data, device);
    }

    if (!ng->device || strcmp(device, ng->device) != 0) {
        free_device(ng);
        open_device(data, device);
    }
}

static void *muted_create(obs_data_t *settings, obs_source_t *filter)
{
    struct muted_data *ng = bzalloc(sizeof(*ng));
    ng->context = filter;
    ma_context_config cfg = ma_context_config_init();
    ma_log_callback cb = ma_log_callback_init(&log_callback, ng);

    if (ma_log_init(NULL, &ng->ma_log) != MA_SUCCESS) {
        blog(LOG_ERROR, "Failed to init ma_log");
        goto end;
    }

    if (ma_log_register_callback(&ng->ma_log, cb) != MA_SUCCESS) {
        blog(LOG_ERROR, "Failed to register log callback");
        goto end;
    }

    cfg.pUserData = ng;
    cfg.pLog = &ng->ma_log;

    if (ma_context_init(NULL, 0, &cfg, &ng->ma_context) != MA_SUCCESS) {
        blog(LOG_ERROR, "Failed to initialize context.");
    } else {
        ng->ma_initialized = true;
        muted_update(ng, settings);
    }

end:
    return ng;
}

static struct obs_audio_data *muted_filter_audio(void *data, struct obs_audio_data *audio)
{
    struct muted_data *ng = data;
    obs_source_t *parent = obs_filter_get_parent(ng->context);
    if (!obs_source_muted(parent)) {
        ng->is_open = false;
        return audio;
    }

    float **adata = (float **)audio->data;
    const float close_threshold = ng->close_threshold;
    const float open_threshold = ng->open_threshold;
    const float sample_rate_i = ng->sample_rate_i;
    const float release_rate = ng->release_rate;
    const float attack_rate = ng->attack_rate;
    const float decay_rate = ng->decay_rate;
    const float hold_time = ng->hold_time;
    const size_t channels = ng->channels;

    for (size_t i = 0; i < audio->frames; i++) {
        float cur_level = fabsf(adata[0][i]);
        for (size_t j = 0; j < channels; j++) {
            cur_level = fmaxf(cur_level, fabsf(adata[j][i]));
        }

        if (cur_level > open_threshold && !ng->is_open) {
            ng->is_open = true;
        }
        if (ng->level < close_threshold && ng->is_open) {
            ng->held_time = 0.0f;
            ng->is_open = false;
        }

        ng->level = fmaxf(ng->level, cur_level) - decay_rate;

        if (ng->is_open) {
            ng->attenuation = fminf(1.0f, ng->attenuation + attack_rate);
        } else {
            ng->held_time += sample_rate_i;
            if (ng->held_time > hold_time) {
                ng->attenuation = fmaxf(0.0f, ng->attenuation - release_rate);
            }
        }
    }

    uint64_t time = (uint64_t)(os_gettime_ns() / ((uint64_t)1e6));
    if (ng->is_open && (time - ng->last_play_time) > (ng->file_length + ng->cooldown)) {
        ng->last_play_time = time;
        if (ng->audio_indicator)
            play_audio(data);
	    // hide the visual indicator a bit early so it blinks if there's continous audio on the source
        if (ng->visual_indicator)
	        indicator_show(ng->cooldown * 0.7f, ng->indicator_size);
    }

    return audio;
}

static void muted_defaults(obs_data_t *s)
{
    obs_data_set_default_double(s, S_OPEN_THRESHOLD, -26.0);
    obs_data_set_default_double(s, S_CLOSE_THRESHOLD, -32.0);
    obs_data_set_default_int(s, S_ATTACK_TIME, 25);
    obs_data_set_default_int(s, S_HOLD_TIME, 200);
    obs_data_set_default_int(s, S_RELEASE_TIME, 150);
    obs_data_set_default_int(s, S_COOLDOWN, 1500);
    obs_data_set_default_int(s, S_DEVICE, 0);
    obs_data_set_default_bool(s, S_AUDIO_INDICATOR, false);
    obs_data_set_default_bool(s, S_VISUAL_INDICATOR, true);
    obs_data_set_default_int(s, S_VISUAL_INDICATOR_SIZE, 45);

    char *path = obs_module_file("urmuted.wav");
    obs_data_set_default_string(s, S_FILE, path);
    bfree(path);
}

static obs_properties_t *muted_properties(void *data)
{
    obs_properties_t *ppts = obs_properties_create();
    obs_property_t *p;
    struct muted_data *d = data;

    obs_properties_t *ng_group = obs_properties_create();
    obs_properties_t *audio_group = obs_properties_create();
    obs_properties_t *visual_group = obs_properties_create();

    /* Noise gate settings */
    p = obs_properties_add_float_slider(ng_group, S_CLOSE_THRESHOLD,
					TEXT_CLOSE_THRESHOLD, VOL_MIN, VOL_MAX, 1.0);
    obs_property_float_set_suffix(p, " dB");
    p = obs_properties_add_float_slider(ng_group, S_OPEN_THRESHOLD,
					TEXT_OPEN_THRESHOLD, VOL_MIN, VOL_MAX, 1.0);
    obs_property_float_set_suffix(p, " dB");
    p = obs_properties_add_int(ng_group, S_ATTACK_TIME, TEXT_ATTACK_TIME, 0, 10000, 1);
    obs_property_int_set_suffix(p, " ms");
    p = obs_properties_add_int(ng_group, S_HOLD_TIME, TEXT_HOLD_TIME, 0, 10000, 1);
    obs_property_int_set_suffix(p, " ms");
    p = obs_properties_add_int(ng_group, S_RELEASE_TIME, TEXT_RELEASE_TIME, 0, 10000, 1);
    obs_property_int_set_suffix(p, " ms");
    p = obs_properties_add_int(ng_group, S_COOLDOWN, TEXT_COOLDOWN, 0, 10000, 500);
    obs_property_int_set_suffix(p, " ms");

    obs_properties_add_group(ppts, S_NOISE_GATE_GROUP, TEXT_NOISE_GATE_GROUP, OBS_GROUP_NORMAL, ng_group);

    /* Visual indicator settings */
    p = obs_properties_add_bool(visual_group, S_VISUAL_INDICATOR,
				TEXT_VISUAL_INDICATOR);
    p = obs_properties_add_int(visual_group, S_VISUAL_INDICATOR_SIZE,
			       TEXT_VISUAL_INDICATOR_SIZE, 5, 500, 1);
    obs_properties_add_group(ppts, S_VISUAL_GROUP, TEXT_VISUAL_GROUP, OBS_GROUP_NORMAL, visual_group);


    /* Audio indicator settings */
    p = obs_properties_add_bool(audio_group, S_AUDIO_INDICATOR, TEXT_AUDIO_INDICATOR);
    p = obs_properties_add_list(audio_group, S_DEVICE, TEXT_DEVICE,
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    populate_list(d, p);
    char *path = obs_module_file("urmuted.wav");
    obs_properties_add_path(audio_group, S_FILE, TEXT_FILE, OBS_PATH_FILE,
			    "WAV file (*.wav)", path);
    bfree(path);

    obs_properties_add_group(ppts, S_AUDIO_GROUP, TEXT_AUDIO_GROUP, OBS_GROUP_NORMAL, audio_group);

    return ppts;
}

struct obs_source_info muted_filter = {
    .id = "muted_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = muted_name,
    .create = muted_create,
    .destroy = muted_destroy,
    .update = muted_update,
    .filter_audio = muted_filter_audio,
    .get_defaults = muted_defaults,
    .get_properties = muted_properties,
};

OBS_MODULE_AUTHOR("univrsal")

const char *obs_module_name(void)
{
    return "Muted notification";
}

const char *obs_module_description(void)
{
    return MT_("Description");
}

bool obs_module_load(void)
{
    obs_register_source(&muted_filter);
	binfo("loaded v%s, %s@%s, compile time: %s", PLUGIN_VERSION,
	      GIT_COMMIT_HASH, GIT_BRANCH, BUILD_TIME);

    indicator_init();
    return true;
}

void obs_module_unload()
{
}

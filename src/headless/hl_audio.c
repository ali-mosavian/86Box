/* Audio sink for the headless build: accepts every buffer, plays nothing. */
#include <stddef.h>
#include <stdint.h>

#include <86box/86box.h>
#include <86box/sound.h>
#include <86box/plat_unused.h>

void
closeal(void)
{
}

void
inital(void)
{
}

const char *
sound_get_output_devices(void)
{
    return NULL;
}

const char *
sound_get_input_devices(void)
{
    return NULL;
}

int
sound_get_device_sample_rate(UNUSED(const char *device_name))
{
    return 0;
}

int
sound_get_device_supported_rates(UNUSED(const char *device_name), UNUSED(int *rates_out), UNUSED(int max_rates))
{
    return 0;
}

int
al_capture_get_rate(void)
{
    return 0;
}

int
al_capture_available(void)
{
    return 0;
}

void
al_capture_open(void)
{
}

void
al_capture_close(void)
{
}

void
al_capture_start(void)
{
}

void
al_capture_stop(void)
{
}

void
al_capture_get_data(UNUSED(int16_t *buf), size_t *len)
{
    *len = 0;
}

void
givealbuffer_common(UNUSED(const void *buf), UNUSED(const uint8_t src), UNUSED(const int size))
{
}

void
al_set_midi(UNUSED(int freq), UNUSED(int buf_size))
{
}

/* Audio sink for the headless build: accepts every buffer, plays nothing. */
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

void
givealbuffer(UNUSED(const void *buf))
{
}

void
givealbuffer_music(UNUSED(const void *buf))
{
}

void
givealbuffer_wt(UNUSED(const void *buf))
{
}

void
givealbuffer_cd(UNUSED(const void *buf))
{
}

void
givealbuffer_fdd(UNUSED(const void *buf), UNUSED(uint32_t size))
{
}

void
givealbuffer_hdd(UNUSED(const void *buf), UNUSED(uint32_t size))
{
}

void
givealbuffer_midi(UNUSED(void *buf), UNUSED(uint32_t size))
{
}

void
al_set_midi(UNUSED(int freq), UNUSED(int buf_size))
{
}

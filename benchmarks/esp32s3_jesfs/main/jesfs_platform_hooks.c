#include <stdint.h>

#include "esp_timer.h"

uint32_t _time_get(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

int16_t _supply_voltage_check(void)
{
    return 0;
}


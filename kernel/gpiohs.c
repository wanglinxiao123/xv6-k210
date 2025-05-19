// GPIOHS Protocol Implementation

#include "include/types.h"
#include "include/gpiohs.h"
#include "include/fpioa.h"
#include "include/utils.h"
#include "include/memlayout.h"

#define GPIOHS_MAX_PINNO 32

volatile gpiohs_t *const gpiohs = (volatile gpiohs_t *)GPIOHS_V;

void gpiohs_set_drive_mode(uint8 pin, gpio_drive_mode_t mode)
{
    int io_number = fpioa_get_io_by_function(FUNC_GPIOHS0 + pin);

    fpioa_pull_t pull = FPIOA_PULL_NONE;
    uint32 dir = 0;

    switch (mode)
    {
    case GPIO_DM_INPUT:
        pull = FPIOA_PULL_NONE;
        dir = 0;
        break;
    case GPIO_DM_INPUT_PULL_DOWN:
        pull = FPIOA_PULL_DOWN;
        dir = 0;
        break;
    case GPIO_DM_INPUT_PULL_UP:
        pull = FPIOA_PULL_UP;
        dir = 0;
        break;
    case GPIO_DM_OUTPUT:
        pull = FPIOA_PULL_DOWN;
        dir = 1;
        break;
    default:
        break;
    }

    fpioa_set_io_pull(io_number, pull);
    volatile uint32 *reg = dir ? gpiohs->output_en.u32 : gpiohs->input_en.u32;
    volatile uint32 *reg_d = !dir ? gpiohs->output_en.u32 : gpiohs->input_en.u32;
    set_gpio_bit(reg_d, pin, 0);
    set_gpio_bit(reg, pin, 1);
}

void gpiohs_set_pin(uint8 pin, gpio_pin_value_t value)
{
    set_gpio_bit(gpiohs->output_val.u32, pin, value);
}

/*
 * ArgonOS port: ESP-IDF - MCPWM complementary pair with dead-time.
 *
 * The timer counts up at 10 MHz, so the period is 10e6/hz ticks and the
 * comparator that ends the A pulse sits at period*duty.  A goes high when the
 * timer wraps to empty and low at the comparator; B is derived from A by the
 * dead-time stage, inverted and delayed, so the pair is complementary with a
 * gap on both edges.  Everything is kept in one static handle set because there
 * is one motor output on this port, opened by pair() and freed by stop().
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_MCPWM) && CONFIG_ARGON_MCPWM

#include <string.h>

#include <argon/port/mcpwm.h>

#include "driver/mcpwm_prelude.h"

#define MCPWM_RES_HZ 10000000u /* 0.1 us per tick */

static struct {
    mcpwm_timer_handle_t      timer;
    mcpwm_oper_handle_t       oper;
    mcpwm_cmpr_handle_t       cmp;
    mcpwm_gen_handle_t        gen_a;
    mcpwm_gen_handle_t        gen_b;
    bool                      up;
} s_mc;

void ag_port_mcpwm_stop(void)
{
    if (!s_mc.up) {
        return;
    }
    mcpwm_timer_start_stop(s_mc.timer, MCPWM_TIMER_STOP_EMPTY);
    mcpwm_timer_disable(s_mc.timer);
    mcpwm_del_generator(s_mc.gen_a);
    mcpwm_del_generator(s_mc.gen_b);
    mcpwm_del_comparator(s_mc.cmp);
    mcpwm_del_operator(s_mc.oper);
    mcpwm_del_timer(s_mc.timer);
    memset(&s_mc, 0, sizeof(s_mc));
}

int ag_port_mcpwm_pair(int gpio_a, int gpio_b, uint32_t hz,
                       uint32_t duty_permille, uint32_t deadtime_ns)
{
    if (s_mc.up) {
        ag_port_mcpwm_stop();
    }
    if (hz == 0u || hz > MCPWM_RES_HZ || duty_permille > 1000u) {
        return -1;
    }
    const uint32_t period = MCPWM_RES_HZ / hz;
    if (period < 2u) {
        return -1;
    }
    const uint32_t dead = (uint32_t)(((uint64_t)deadtime_ns * MCPWM_RES_HZ) /
                                     1000000000ull);

    mcpwm_timer_config_t tcfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = MCPWM_RES_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = period,
    };
    if (mcpwm_new_timer(&tcfg, &s_mc.timer) != ESP_OK) {
        return -1;
    }
    mcpwm_operator_config_t ocfg = {.group_id = 0};
    if (mcpwm_new_operator(&ocfg, &s_mc.oper) != ESP_OK ||
        mcpwm_operator_connect_timer(s_mc.oper, s_mc.timer) != ESP_OK) {
        goto fail;
    }
    mcpwm_comparator_config_t ccfg = {.flags = {.update_cmp_on_tez = true}};
    if (mcpwm_new_comparator(s_mc.oper, &ccfg, &s_mc.cmp) != ESP_OK) {
        goto fail;
    }
    mcpwm_comparator_set_compare_value(s_mc.cmp,
                                       (uint32_t)((uint64_t)period *
                                                  duty_permille / 1000u));

    mcpwm_generator_config_t gacfg = {.gen_gpio_num = gpio_a};
    mcpwm_generator_config_t gbcfg = {.gen_gpio_num = gpio_b};
    if (mcpwm_new_generator(s_mc.oper, &gacfg, &s_mc.gen_a) != ESP_OK ||
        mcpwm_new_generator(s_mc.oper, &gbcfg, &s_mc.gen_b) != ESP_OK) {
        goto fail;
    }

    /* A: high when the timer wraps to empty, low at the comparator. */
    mcpwm_generator_set_actions_on_timer_event(
        s_mc.gen_a,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                     MCPWM_TIMER_EVENT_EMPTY,
                                     MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_TIMER_EVENT_ACTION_END());
    mcpwm_generator_set_actions_on_compare_event(
        s_mc.gen_a,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_mc.cmp,
                                       MCPWM_GEN_ACTION_LOW),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END());

    /* Dead-time: A delayed on its rising edge; B is A inverted, delayed on
     * its falling edge - the two never overlap. */
    mcpwm_dead_time_config_t dta = {.posedge_delay_ticks = dead};
    mcpwm_dead_time_config_t dtb = {.negedge_delay_ticks = dead,
                                    .flags = {.invert_output = true}};
    if (mcpwm_generator_set_dead_time(s_mc.gen_a, s_mc.gen_a, &dta) != ESP_OK ||
        mcpwm_generator_set_dead_time(s_mc.gen_a, s_mc.gen_b, &dtb) != ESP_OK) {
        goto fail;
    }

    if (mcpwm_timer_enable(s_mc.timer) != ESP_OK ||
        mcpwm_timer_start_stop(s_mc.timer, MCPWM_TIMER_START_NO_STOP) !=
            ESP_OK) {
        goto fail;
    }
    s_mc.up = true;
    return 0;

fail:
    /* Undo whatever came up; s_mc.up is still false so stop() would skip it. */
    if (s_mc.gen_a) mcpwm_del_generator(s_mc.gen_a);
    if (s_mc.gen_b) mcpwm_del_generator(s_mc.gen_b);
    if (s_mc.cmp)   mcpwm_del_comparator(s_mc.cmp);
    if (s_mc.oper)  mcpwm_del_operator(s_mc.oper);
    if (s_mc.timer) mcpwm_del_timer(s_mc.timer);
    memset(&s_mc, 0, sizeof(s_mc));
    return -1;
}

#endif /* CONFIG_ARGON_MCPWM */

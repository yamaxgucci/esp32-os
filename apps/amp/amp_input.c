/*
 * Keyboard map for AMP → shared cmd_*.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "amp_app.h"

#include <argon/keys.h>

void amp_handle_key(amp_player_t *p, int key, int down, uint32_t mods)
{
    int shift = (mods & AG_MOD_SHIFT) != 0;
    int ctrl = (mods & AG_MOD_CTRL) != 0;

    if (p == NULL || !down) {
        return;
    }

    /* Global transport */
    if (key == AG_KEY_ESC) {
        if (p->picker) {
            p->picker = 0;
            p->dirty = 1;
            return;
        }
        p->quit = 1;
        return;
    }
    if (p->picker) {
        if (key == AG_KEY_UP) {
            if (p->pick_i > 0) {
                p->pick_i--;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_DOWN) {
            if (p->pick_i + 1 < p->pick_n) {
                p->pick_i++;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_ENTER) {
            amp_cmd_picker_choose(p);
            return;
        }
        return;
    }
    if (key == AG_KEY_TAB) {
        amp_cmd_focus_next(p, shift ? -1 : 1);
        return;
    }
    if (key == AG_KEY_SPACE) {
        amp_cmd_play_pause(p);
        return;
    }
    if (key == AG_KEY_V) {
        amp_cmd_stop(p);
        return;
    }
    if (key == AG_KEY_B || key == AG_KEY_N) {
        amp_cmd_next(p);
        return;
    }
    if (key == AG_KEY_Z) {
        amp_cmd_prev(p);
        return;
    }
    if (key == AG_KEY_P && p->focus != AMP_PANEL_EQ) {
        amp_cmd_prev(p);
        return;
    }
    if (key == AG_KEY_R) {
        p->repeat = !p->repeat;
        p->dirty = 1;
        return;
    }
    if (key == AG_KEY_S && !ctrl) {
        p->shuffle = !p->shuffle;
        p->dirty = 1;
        return;
    }
    if (key == AG_KEY_A || key == AG_KEY_O) {
        amp_cmd_open_picker(p);
        return;
    }
    if (key == AG_KEY_L) {
        amp_cmd_load_m3u(p);
        return;
    }
    if (key == AG_KEY_W || (key == AG_KEY_S && ctrl)) {
        amp_cmd_save_m3u(p);
        return;
    }
    if (key == AG_KEY_E) {
        p->eq.enabled = !p->eq.enabled;
        p->dirty = 1;
        return;
    }
    if (key == AG_KEY_Q) {
        amp_eq_reset(&p->eq);
        p->dirty = 1;
        return;
    }
    if (key == AG_KEY_EQUAL || key == AG_KEY_KP_PLUS) {
        amp_cmd_volume(p, p->volume + 4);
        return;
    }
    if (key == AG_KEY_MINUS || key == AG_KEY_KP_MINUS) {
        amp_cmd_volume(p, p->volume - 4);
        return;
    }
    if (key == AG_KEY_COMMA) {
        amp_cmd_balance(p, p->balance - 10);
        return;
    }
    if (key == AG_KEY_PERIOD) {
        amp_cmd_balance(p, p->balance + 10);
        return;
    }

    /* Digits → EQ band */
    if (key >= AG_KEY_1 && key <= AG_KEY_0) {
        int band = (key == AG_KEY_0) ? 10 : (key - AG_KEY_1 + 1);
        p->eq.band_sel = band;
        p->focus = AMP_PANEL_EQ;
        p->dirty = 1;
        return;
    }
    if (key == AG_KEY_GRAVE) {
        p->eq.band_sel = 0;
        p->focus = AMP_PANEL_EQ;
        p->dirty = 1;
        return;
    }

    if (p->focus == AMP_PANEL_PL) {
        if (key == AG_KEY_UP) {
            if (p->pl.sel > 0) {
                p->pl.sel--;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_DOWN) {
            if (p->pl.sel + 1 < p->pl.count) {
                p->pl.sel++;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_PAGEUP) {
            p->pl.sel -= 8;
            if (p->pl.sel < 0) {
                p->pl.sel = 0;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_PAGEDOWN) {
            p->pl.sel += 8;
            if (p->pl.sel >= p->pl.count && p->pl.count > 0) {
                p->pl.sel = p->pl.count - 1;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_HOME) {
            p->pl.sel = 0;
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_END) {
            p->pl.sel = p->pl.count > 0 ? p->pl.count - 1 : 0;
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_ENTER) {
            amp_cmd_open_sel(p);
            return;
        }
        if (key == AG_KEY_DELETE || key == AG_KEY_BACKSPACE) {
            amp_pl_remove_sel(&p->pl);
            p->dirty = 1;
            return;
        }
    }

    if (p->focus == AMP_PANEL_EQ) {
        if (key == AG_KEY_LEFT) {
            if (p->eq.band_sel > 0) {
                p->eq.band_sel--;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_RIGHT) {
            if (p->eq.band_sel < AMP_EQ_BANDS) {
                p->eq.band_sel++;
            }
            p->dirty = 1;
            return;
        }
        if (key == AG_KEY_UP) {
            amp_cmd_eq_adjust(p, 1);
            return;
        }
        if (key == AG_KEY_DOWN) {
            amp_cmd_eq_adjust(p, -1);
            return;
        }
        if (key == AG_KEY_P) {
            p->eq.band_sel = 0;
            p->dirty = 1;
            return;
        }
    }

    /* Default arrows: seek / volume (also when focus Main) */
    if (key == AG_KEY_LEFT) {
        if (shift) {
            amp_cmd_balance(p, p->balance - 10);
        } else {
            amp_cmd_seek_delta(p, -5000);
        }
        return;
    }
    if (key == AG_KEY_RIGHT) {
        if (shift) {
            amp_cmd_balance(p, p->balance + 10);
        } else {
            amp_cmd_seek_delta(p, 5000);
        }
        return;
    }
    if (key == AG_KEY_UP) {
        amp_cmd_volume(p, p->volume + 4);
        return;
    }
    if (key == AG_KEY_DOWN) {
        amp_cmd_volume(p, p->volume - 4);
        return;
    }
}

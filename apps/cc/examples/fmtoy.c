/*
 * fmtoy — Argon CC audio demo: 2-op integer FM (not DX7).
 * Full DX7-shaped synth is the host-built DX7.AXE.
 *
 * Pad: B1 (Z) note on/off, Esc/quit via btn 7.
 * Requires CC with ag_audio_* builtins.
 */
int sin_tab[64];
int phase_m;
int phase_c;
int step_m;
int step_c;
int gate;
int pcm[64];

int isin(int ph)
{
    int i;
    i = ph / 67108864;
    if (i < 0) {
        i = 0 - i;
    }
    while (i >= 64) {
        i = i - 64;
    }
    return sin_tab[i];
}

int fill_sin(void)
{
    int i;
    int v;
    /* 64-point triangle-ish wave, peak 12000 */
    i = 0;
    while (i < 64) {
        if (i < 16) {
            v = i * 750;
        } else {
            if (i < 32) {
                v = (32 - i) * 750;
            } else {
                if (i < 48) {
                    v = 0 - (i - 32) * 750;
                } else {
                    v = 0 - (64 - i) * 750;
                }
            }
        }
        sin_tab[i] = v;
        i = i + 1;
    }
    return 0;
}

int render(int n)
{
    int i;
    int mod;
    int car;
    int s;
    i = 0;
    while (i < n) {
        if (gate) {
            phase_m = phase_m + step_m;
            phase_c = phase_c + step_c;
            mod = isin(phase_m) / 4;
            car = isin(phase_c + mod * 65536);
            s = car / 2;
        } else {
            s = 0;
        }
        /* int32 store: low int16 = sample (left); high ≈ 0 or sign-extend */
        pcm[i] = s;
        i = i + 1;
    }
    return n;
}

int ag_main(void)
{
    int n;
    int frames;
    fill_sin();
    phase_m = 0;
    phase_c = 0;
    /* ~220 Hz carrier / ~440 Hz mod at 22050 Hz (32-bit phase) */
    step_c = 42000000;
    step_m = 84000000;
    gate = 0;
    if (ag_audio_present() == 0) {
        return 1;
    }
    if (ag_audio_open() != 0) {
        return 2;
    }
    frames = 0;
    while (ag_btn(7) == 0) {
        if (ag_btn(4) != 0) {
            gate = 1;
        } else {
            gate = 0;
        }
        n = 32;
        render(n);
        ag_audio_write(pcm, n);
        frames = frames + n;
        ag_delay(2);
        if (frames > 22050 * 30) {
            /* safety stop after ~30s */
            frames = 0;
        }
    }
    ag_audio_close();
    return 0;
}

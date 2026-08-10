/*
 * Phase D smoke: write and read a file through ag_open/ag_read/ag_write,
 * allocate a PCM buffer with ag_malloc, and write a short mute chunk through
 * ag_audio_write.
 *
 *   run t:\cc.axe t:\fsmem.c t:\fsmem.axe
 *   run t:\fsmem.axe
 *   errorlevel
 *
 * Exit code 0 on success; non-zero is which check failed.
 */

#define AG_O_RDONLY 0
#define AG_O_WRONLY 1
#define AG_O_CREATE 4
#define AG_O_TRUNC  8

int ag_main(void)
{
    int   h;
    int   n;
    int   i;
    char  msg[8];
    char  head[8];
    char *pcm;

    msg[0] = 'o';
    msg[1] = 'k';
    msg[2] = ' ';
    msg[3] = 'h';
    msg[4] = 'e';
    msg[5] = 'l';
    msg[6] = 'l';
    msg[7] = 'o';

    h = ag_open("t:\\fsmem.txt", AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        ag_print("fsmem: create failed\n");
        return 1;
    }
    n = ag_write(h, msg, 8);
    ag_close(h);
    if (n != 8) {
        ag_print("fsmem: write failed\n");
        return 2;
    }

    h = ag_open("t:\\fsmem.txt", AG_O_RDONLY);
    if (h < 0) {
        ag_print("fsmem: open failed\n");
        return 3;
    }
    n = ag_read(h, head, 8);
    ag_close(h);
    if (n < 4) {
        ag_print("fsmem: short read\n");
        return 4;
    }
    if (head[0] != 'o' || head[1] != 'k') {
        ag_print("fsmem: bad content\n");
        return 5;
    }
    ag_print("fsmem: read ok\n");

    /* 64 stereo s16 frames = 256 bytes of silence. */
    pcm = ag_malloc(256);
    if (pcm == 0) {
        ag_print("fsmem: malloc failed\n");
        return 6;
    }
    for (i = 0; i < 256; i = i + 1) {
        pcm[i] = 0;
    }

    if (ag_audio_present() == 0) {
        ag_free(pcm);
        ag_print("fsmem: no audio\n");
        return 7;
    }
    if (ag_audio_open() != 0) {
        ag_free(pcm);
        ag_print("fsmem: audio open failed\n");
        return 8;
    }
    n = ag_audio_write(pcm, 64);
    ag_audio_close();
    ag_free(pcm);
    if (n < 0) {
        ag_print("fsmem: audio write failed\n");
        return 9;
    }

    ag_print("fsmem: pcm ok\n");
    return 0;
}

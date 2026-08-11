/*
 * Guest-built echo .SYS — same idea as apps/echo/echo.c, in the Mini-C dialect.
 *
 *   run h:\cc.axe h:\echo.c h:\echo.sys
 *   drv load h:\echo.sys
 *   copy t:\note.txt d:\echo
 *   type d:\echo
 *   drv unload ECHO
 *
 * Layout of `struct ops` / `struct desc` matches ag_dev_ops_t / ag_dev_add_t
 * on Xtensa (word-sized fields in declaration order).  Kernel callbacks pass
 * uint64_t off as two windowed words (off_lo, off_hi).
 */

#pragma drv "ECHO" "1.0" "cc"

#define AG_DEV_CHAR 3
#define ECHO_CAP    64

char echo_buf[64];
int  echo_len;

struct ops {
    int open;
    int close;
    int read;
    int write;
    int ioctl;
    int size;
};

struct desc {
    int name;
    int driver;
    int cls;
    int flags;
    int ops;
    int class_ops;
    int priv;
};

struct ops  g_ops;
struct desc g_desc;

int echo_read(int dev, char *dst, int n, int off_lo, int off_hi)
{
    int i;
    int off;

    off = off_lo;
    if (off_hi != 0) {
        return 0;
    }
    if (off < 0 || off >= echo_len) {
        return 0;
    }
    if (n > echo_len - off) {
        n = echo_len - off;
    }
    i = 0;
    while (i < n) {
        dst[i] = echo_buf[off + i];
        i = i + 1;
    }
    return n;
}

int echo_write(int dev, char *src, int n, int off_lo, int off_hi)
{
    int i;

    if (off_lo != 0 || off_hi != 0) {
        return 0;
    }
    if (n > ECHO_CAP) {
        n = ECHO_CAP;
    }
    i = 0;
    while (i < n) {
        echo_buf[i] = src[i];
        i = i + 1;
    }
    echo_len = n;
    return n;
}

int ag_driver_init(void)
{
    echo_len = 0;

    g_ops.open = 0;
    g_ops.close = 0;
    g_ops.read = &echo_read;
    g_ops.write = &echo_write;
    g_ops.ioctl = 0;
    g_ops.size = 0;

    g_desc.name = "echo";
    g_desc.driver = "ECHO";
    g_desc.cls = AG_DEV_CHAR;
    g_desc.flags = 0;
    g_desc.ops = &g_ops;
    g_desc.class_ops = 0;
    g_desc.priv = 0;

    return ag_dev_add(&g_desc);
}

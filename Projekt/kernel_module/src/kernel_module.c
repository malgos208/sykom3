#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <asm/io.h>
#include <linux/math64.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student SYKOM");
MODULE_DESCRIPTION("FP64 multiplier (simple PROCFS)");

#define BASE_ADDR   0x00100000
#define BIAS        67108864ULL   // 2^26

static void __iomem *base;
static void __iomem *arg1_h, *arg1_l, *arg2_h, *arg2_l;
static void __iomem *ctrl, *status, *res_h, *res_l;

/* Uproszczone parsowanie liczby w notacji naukowej (np. "2.5e0") */
static int parse_fp(const char *buf, u64 *val)
{
    const char *p = buf;
    u8 sign = 0;
    u64 int_part = 0, frac_part = 0;
    int frac_digits = 0;
    int exp = 0;

    /* znak */
    if (*p == '-') { sign = 1; p++; }
    else if (*p == '+') p++;

    /* część całkowita */
    while (isdigit(*p))
        int_part = int_part * 10 + (*p++ - '0');

    /* część ułamkowa */
    if (*p == '.') {
        p++;
        while (isdigit(*p) && frac_digits < 9) {
            frac_part = frac_part * 10 + (*p++ - '0');
            frac_digits++;
        }
    }

    /* wykładnik 'e' */
    if (*p == 'e' || *p == 'E') {
        p++;
        int e_sign = 1;
        if (*p == '-') { e_sign = -1; p++; }
        else if (*p == '+') p++;
        int e = 0;
        while (isdigit(*p)) e = e * 10 + (*p++ - '0');
        exp = e_sign * e;
    }

    /* białe znaki i koniec napisu */
    while (isspace(*p)) p++;
    if (*p != '\0') return -EINVAL;

    if (int_part == 0 && frac_part == 0) {
        *val = 0;
        return 0;
    }

    /* Tworzymy liczbę całkowitą v = int_part * 10^frac_digits + frac_part */
    u64 v = int_part;
    if (frac_digits > 0) {
        u64 mul = 1;
        for (int i = 0; i < frac_digits; i++) mul *= 10;
        v = v * mul + frac_part;
        exp -= frac_digits;
    }

    /* Skalowanie przez 10^exp (ograniczone, dla testów wystarczy) */
    if (exp > 0) {
        for (int i = 0; i < exp; i++) {
            if (v > (U64_MAX / 10)) return -EINVAL;
            v *= 10;
        }
    } else if (exp < 0) {
        for (int i = 0; i < -exp; i++) do_div(v, 10);
    }

    /* Normalizacja binarna do przedziału [2^36, 2^37) */
    int bin_exp = 0;
    while (v >= (1ULL << 37)) { v >>= 1; bin_exp++; }
    while (v <  (1ULL << 36)) { v <<= 1; bin_exp--; }

    u64 mantissa = v - (1ULL << 36);           // 36 bitów
    int fin_exp = bin_exp + BIAS;
    if (fin_exp < 0 || fin_exp > 0x7FFFFFF) return -EINVAL;

    *val = (mantissa << 28) | ((u64)fin_exp << 1) | sign;
    return 0;
}

/* ---------- PROCFS ---------- */
static ssize_t arg_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *off,
                         void __iomem *high, void __iomem *low)
{
    char buf[64];
    u64 val;
    if (cnt >= sizeof(buf)) return -ENOSPC;
    if (copy_from_user(buf, ubuf, cnt)) return -EFAULT;
    buf[cnt] = '\0';
    if (parse_fp(buf, &val)) return -EINVAL;
    iowrite32(val >> 32, high);
    iowrite32(val & 0xFFFFFFFF, low);
    *off += cnt;
    return cnt;
}

static ssize_t a1_write(struct file *f, const char __user *b, size_t c, loff_t *o)
    { return arg_write(f, b, c, o, arg1_h, arg1_l); }
static ssize_t a2_write(struct file *f, const char __user *b, size_t c, loff_t *o)
    { return arg_write(f, b, c, o, arg2_h, arg2_l); }

static ssize_t ctrl_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *off)
{
    char buf[16];
    long cmd;
    if (cnt >= sizeof(buf)) return -ENOSPC;
    if (copy_from_user(buf, ubuf, cnt)) return -EFAULT;
    buf[cnt] = '\0';
    if (kstrtol(buf, 10, &cmd)) return -EINVAL;
    if (cmd == 0 || cmd == 1) iowrite32(cmd, ctrl);
    else return -EINVAL;
    *off += cnt;
    return cnt;
}

static ssize_t status_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *off)
{
    if (*off) return 0;
    u32 st = ioread32(status);
    const char *msg = st == 0 ? "idle\n" : (st == 1 ? "busy\n" : "done\n");
    size_t len = strlen(msg);
    if (copy_to_user(ubuf, msg, len)) return -EFAULT;
    *off = len;
    return len;
}

static ssize_t result_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *off)
{
    if (*off) return 0;
    if (ioread32(status) != 2) return -EAGAIN;
    u64 v = ((u64)ioread32(res_h) << 32) | ioread32(res_l);
    char buf[64];
    if (v == 0) {
        snprintf(buf, sizeof(buf), "0.0e0\n");
    } else {
        int sign = (v & 1) ? -1 : 1;
        int exp = ((v >> 1) & 0x7FFFFFF) - BIAS;
        u64 mant = ((v >> 28) & 0xFFFFFFFFFULL) | (1ULL << 36);
        snprintf(buf, sizeof(buf), "%s%llu.%06llue%d\n",
                 sign < 0 ? "-" : "", mant >> 36,
                 ((mant & ((1ULL << 36) - 1)) * 1000000ULL) >> 36, exp);
    }
    size_t len = strlen(buf);
    if (copy_to_user(ubuf, buf, len)) return -EFAULT;
    *off = len;
    return len;
}

static const struct file_operations a1_fops = { .write = a1_write };
static const struct file_operations a2_fops = { .write = a2_write };
static const struct file_operations ctrl_fops = { .write = ctrl_write };
static const struct file_operations stat_fops = { .read = status_read };
static const struct file_operations res_fops = { .read = result_read };

static struct proc_dir_entry *proc_dir;

static int __init init(void)
{
    printk(KERN_INFO "FP multiplier: init\n");
    base = ioremap(BASE_ADDR, 0x8000);
    if (!base) return -ENOMEM;

    arg1_h = base + 0x100; arg1_l = base + 0x108;
    arg2_h = base + 0x0F0; arg2_l = base + 0x0F8;
    ctrl   = base + 0x0D0; status = base + 0x0E8;
    res_h  = base + 0x0D8; res_l  = base + 0x0E0;

    proc_dir = proc_mkdir("sykom", NULL);
    if (!proc_dir) goto fail;

    if (!proc_create("a1stma", 0220, proc_dir, &a1_fops) ||
        !proc_create("a2stma", 0220, proc_dir, &a2_fops) ||
        !proc_create("ctstma", 0220, proc_dir, &ctrl_fops) ||
        !proc_create("ststma", 0444, proc_dir, &stat_fops) ||
        !proc_create("restma", 0444, proc_dir, &res_fops))
        goto fail_rm;

    printk(KERN_INFO "FP multiplier: ready\n");
    return 0;

fail_rm: remove_proc_entry("sykom", NULL);
fail:   iounmap(base);
        return -ENOMEM;
}

static void __exit fini(void)
{
    printk(KERN_INFO "FP multiplier: cleanup\n");
    iowrite32(0x3333 | (0x7F << 16), base);   // zakończenie emulacji
    remove_proc_entry("sykom", NULL);
    iounmap(base);
}

module_init(init);
module_exit(fini);
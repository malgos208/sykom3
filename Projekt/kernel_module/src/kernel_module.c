#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <asm/io.h>
#include <linux/math64.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student SYKOM");
MODULE_DESCRIPTION("FP64 multiplier (PROCFS)");

#define BASE_ADDR   0x00100000
#define BIAS        67108864ULL   // 2^26

static void __iomem *base;
static void __iomem *arg1_h, *arg1_l, *arg2_h, *arg2_l;
static void __iomem *ctrl, *status, *res_h, *res_l;

// Bezpieczne dzielenie 64-bitowej zmiennej przez 10 (jak u kolegi)
static u32 div_u64_by_10(u64 *val)
{
    u64 q = 0, rem = *val;
    int i;
    for (i = 60; i >= 0; i--) {
        if ((rem >> i) >= 10) {
            q |= (1ULL << i);
            rem -= (10ULL << i);
        }
    }
    *val = q;
    return (u32)rem;
}

// ---------- parsowanie notacji dziesiętnej naukowej na format 64‑bit ----------
// Format: bit[0] znak, [27:1] wykładnik (27 bitów), [63:28] mantysa (36 bitów)
static int parse_fp(const char *buf, u64 *val)
{
    int sign = 0;
    u64 int_part = 0, frac_part = 0;
    int frac_digits = 0;
    int exp_e = 0;               // wykładnik po 'e'

    const char *p = buf;
    if (*p == '-') { sign = 1; p++; }
    else if (*p == '+') p++;

    while (isdigit(*p)) {
        int_part = int_part * 10 + (*p - '0');
        p++;
    }

    if (*p == '.') {
        p++;
        while (isdigit(*p) && frac_digits < 9) {
            frac_part = frac_part * 10 + (*p - '0');
            frac_digits++;
            p++;
        }
        while (isdigit(*p)) p++; // pomijamy nadmiarowe cyfry
    }

    if (*p == 'e' || *p == 'E') {
        p++;
        int e_sign = 1;
        if (*p == '-') { e_sign = -1; p++; }
        else if (*p == '+') p++;
        while (isdigit(*p)) {
            exp_e = exp_e * 10 + (*p - '0');
            p++;
        }
        exp_e *= e_sign;
    }

    while (isspace(*p)) p++;
    if (*p != '\0') return -EINVAL;

    if (int_part == 0 && frac_part == 0) {
        *val = 0;
        return 0;
    }

    // Łączymy w jedną liczbę całkowitą: value = int_part * 10^frac_digits + frac_part
    u64 value = int_part;
    int i;
    for (i = 0; i < frac_digits; i++) {
        if (value > (U64_MAX / 10)) return -EINVAL;
        value *= 10;
    }
    value += frac_part;

    int exp10 = exp_e - frac_digits;   // całkowity wykładnik dziesiętny

    // Wstępna normalizacja binarna z zapasem (do zakresu [2^60, 2^61))
    int binary_exp = 0;
    while (value > 0 && value < (1ULL << 60)) {
        value <<= 1;
        binary_exp--;
    }
    // doprowadzamy, by value było >= 2^60 (jeśli value != 0)
    if (value >= (1ULL << 61)) {
        value >>= 1;
        binary_exp++;
    }

    // Mnożymy/dzielimy przez 10^|exp10| z zachowaniem precyzji
    while (exp10 > 0) {
        // pomnóż value przez 10 = (value << 3) + (value << 1)
        u64 new_val = (value << 3) + (value << 1);
        while (new_val >= (1ULL << 61)) {
            new_val >>= 1;
            binary_exp++;
        }
        value = new_val;
        exp10--;
    }
    while (exp10 < 0) {
        // dzielenie value przez 10
        u64 tmp = value;
        div_u64_by_10(&tmp);
        value = tmp;
        // po dzieleniu wartość zmalała – trzeba dosunąć w lewo, aby utrzymać precyzję
        while (value > 0 && value < (1ULL << 60)) {
            value <<= 1;
            binary_exp--;
        }
        exp10++;
    }

    // Ostateczna normalizacja do przedziału [2^36, 2^37)
    while (value >= (1ULL << 37)) { value >>= 1; binary_exp++; }
    while (value <  (1ULL << 36)) { value <<= 1; binary_exp--; }

    // Składanie formatu 64-bitowego
    int fin_exp = binary_exp + (int)BIAS;
    if (fin_exp < 0 || fin_exp > 0x7FFFFFF) return -EINVAL;

    u64 mantissa = value - (1ULL << 36);
    *val = (mantissa << 28) | ((u64)fin_exp << 1) | (u64)sign;
    return 0;
}

// ---------- konwersja 64‑bit -> notacja dziesiętna naukowa ----------
static int format_fp(u64 val, char *buf, size_t size)
{
    if (val == 0)
        return snprintf(buf, size, "0.0e0\n");

    int sign = val & 1;
    u32 exp_field = (val >> 1) & 0x7FFFFFF;
    u64 mant_field = (val >> 28) & 0xFFFFFFFFFULL;  // 36 bitów mantysy

    u64 mant_full = (1ULL << 36) | mant_field;      // 37-bitowa wartość 1.mantysa
    int real_exp = (int)exp_field - (int)BIAS;      // wykładnik w systemie bin.
    int bin_exp_adj = real_exp - 36;                // mant_full ma wagę 2^36

    u64 dec_val = mant_full;
    int dec_exp = 0;

    // Przelicz 2^bin_exp_adj na potęgi dziesiątki
    while (bin_exp_adj > 0) {
        if (dec_val > (U64_MAX / 2)) {
            div_u64_by_10(&dec_val);
            dec_exp++;
        }
        dec_val <<= 1;
        bin_exp_adj--;
    }
    while (bin_exp_adj < 0) {
        if (dec_val > (U64_MAX / 5)) {
            div_u64_by_10(&dec_val);
            dec_exp++;
        }
        dec_val *= 5;
        div_u64_by_10(&dec_val);
        bin_exp_adj++;
    }

    // Zamiana na łańcuch i normalizacja do postaci z jedną cyfrą przed kropką
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%llu", dec_val);
    if (len <= 0) return -EINVAL;

    int sci_exp = (len - 1) + dec_exp;
    char mant_buf[16];
    mant_buf[0] = tmp[0];
    mant_buf[1] = '.';
    for (int i = 2; i < 2 + 6; i++) {
        mant_buf[i] = (i - 1 < len) ? tmp[i - 1] : '0';
    }
    mant_buf[2 + 6] = '\0';

    return snprintf(buf, size, "%s%se%d\n",
                    sign ? "-" : "", mant_buf, sci_exp);
}

// ---------- PROCFS ----------
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
    if (cmd != 0 && cmd != 1) return -EINVAL;
        iowrite32(cmd, ctrl);
    *off += cnt;
    return cnt;
}

static ssize_t status_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *off)
{
    if (*off) return 0;
    u32 st = ioread32(status);
    const char *msg = (st == 0) ? "idle\n" : ((st == 1) ? "busy\n" : "done\n");
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
    int len = format_fp(v, buf, sizeof(buf));
    if (len < 0) return -EINVAL;
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
    iowrite32(0x3333 | (0x7F << 16), base);
    remove_proc_entry("sykom", NULL);
    iounmap(base);
}

module_init(init);
module_exit(fini);
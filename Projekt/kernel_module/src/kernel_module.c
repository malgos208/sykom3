#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <asm/io.h>
#include <linux/types.h>

MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student SYKOM");
MODULE_DESCRIPTION("FP64 multiplier (PROCFS, simple conversion)");
MODULE_VERSION("0.4");

#define SYKT_GPIO_BASE_ADDR  (0x00100000)
#define SYKT_GPIO_SIZE       (0x8000)
#define SYKT_EXIT            (0x3333)
#define SYKT_EXIT_CODE       (0x7F)
#define BIAS                 67108864ULL   // 2^26

static void __iomem *baseptr;
static void __iomem *arg1_h, *arg1_l, *arg2_h, *arg2_l;
static void __iomem *ctrl, *status, *res_h, *res_l;

/* Konwersja napisu naukowego na reprezentację 64-bitową */
static int parse_fp(const char *buf, u64 *val)
{
    const char *p = buf;
    int sign = 0;
    u64 int_part = 0, frac_part = 0;
    int frac_digits = 0;
    int exp10 = 0;
    u64 v;
    int bin_exp = 0;
    u64 mantissa;
    int fin_exp;

    /* znak */
    if (*p == '-') { sign = 1; p++; } 
    else if (*p == '+') p++;

    /* część całkowita */
    while (isdigit(*p)) {
        int_part = int_part * 10 + (*p - '0');
        p++;
    }

    /* część ułamkowa */
    if (*p == '.') {
        p++;
        while (isdigit(*p)) {
            if (frac_digits < 9) {  // 9 cyfr znaczących (wystarczy dla 36-bit mantysy)
                frac_part = frac_part * 10 + (*p - '0');
                frac_digits++;
            }
            p++;
        }
    }

    /* wykładnik 'e' */
    if (*p == 'e' || *p == 'E') {
        p++;
        int e_sign = 1;
        if (*p == '-') { e_sign = -1; p++; }
        else if (*p == '+') p++;
        long e_val = 0;
        while (isdigit(*p)) {
            e_val = e_val * 10 + (*p - '0');
            p++;
        }
        exp10 = e_sign * e_val;
    }

    while (isspace(*p)) p++;
    if (*p != '\0') return -EINVAL;

    /* tworzymy v = int_part * 10^frac_digits + frac_part */
    v = int_part;
    if (frac_digits > 0) {
        u64 mult = 1;
        for (int i = 0; i < frac_digits; i++) mult *= 10;
        v = v * mult + frac_part;
        exp10 -= frac_digits;
    }

    if (v == 0) {
        *val = 0;
        return 0;
    }

    /* skalujemy przez 10^exp10 (ograniczamy zakres) */
    if (exp10 > 0) {
        for (int i = 0; i < exp10; i++) {
            if (v > (U64_MAX / 10)) return -EINVAL;
            v *= 10;
        }
    } else if (exp10 < 0) {
        for (int i = 0; i < -exp10; i++) {
            v /= 10;   // straty akceptowalne dla testów
        }
    }

    /* normalizacja binarna do [2^36, 2^37) */
    while (v >= (1ULL << 37)) {
        v >>= 1;
        bin_exp++;
    }
    while (v < (1ULL << 36)) {
        v <<= 1;
        bin_exp--;
    }

    mantissa = v - (1ULL << 36);   // ukryta jedynka usunięta
    fin_exp = bin_exp + BIAS;
    if (fin_exp < 0 || fin_exp > 0x7FFFFFF) return -EINVAL;

    *val = ((u64)mantissa << 28) | ((u64)fin_exp << 1) | sign;
    return 0;
}

/* Formatowanie wyniku (uproszczone) */
static int format_fp(u64 val, char *buf, size_t len)
{
    if (val == 0)
        return snprintf(buf, len, "0.0e0\n");

    int sign = (val & 1) ? -1 : 1;
    int exp_bias = (val >> 1) & 0x7FFFFFF;
    int exp = exp_bias - BIAS;
    u64 mant = ((val >> 28) & 0xFFFFFFFFFULL) | (1ULL << 36);

    /* wyciągamy część całkowitą i ułamkową (przybliżoną) */
    u64 int_part = mant >> 36;
    u64 frac_part = ((mant & ((1ULL << 36)-1)) * 1000000ULL) >> 36;

    return snprintf(buf, len, "%s%llu.%06llue%d\n",
                    sign < 0 ? "-" : "", int_part, frac_part, exp);
}

/* ----- PROC FS ops ----- */
static ssize_t arg_write(struct file *f, const char __user *ubuf,
                         size_t count, loff_t *ppos,
                         void __iomem *high, void __iomem *low)
{
    char kbuf[64];
    u64 val;
    if (count >= sizeof(kbuf)) return -ENOSPC;
    if (copy_from_user(kbuf, ubuf, count)) return -EFAULT;
    kbuf[count] = '\0';
    if (parse_fp(kbuf, &val)) return -EINVAL;
    iowrite32(val >> 32, high);
    iowrite32(val & 0xFFFFFFFF, low);
    *ppos += count;
    return count;
}

static ssize_t a1stma_write(struct file *f, const char __user *b, size_t c, loff_t *p)
{ return arg_write(f, b, c, p, arg1_h, arg1_l); }

static ssize_t a2stma_write(struct file *f, const char __user *b, size_t c, loff_t *p)
{ return arg_write(f, b, c, p, arg2_h, arg2_l); }

static ssize_t ctstma_write(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
    char kbuf[16];
    long cmd;
    if (count >= sizeof(kbuf)) return -ENOSPC;
    if (copy_from_user(kbuf, ubuf, count)) return -EFAULT;
    kbuf[count] = '\0';
    if (kstrtol(kbuf, 10, &cmd)) return -EINVAL;
    printk(KERN_INFO "WRITE CTRL addr=%p val=%ld\n", ctrl, cmd);
    if (cmd == 0 || cmd == 1)
        iowrite32(cmd, ctrl);
    else
        return -EINVAL;
    *ppos += count;
    return count;
}

static ssize_t ststma_read(struct file *f, char __user *ubuf, size_t count, loff_t *ppos)
{
    const char *msg;
    int len;
    if (*ppos > 0) return 0;
    switch (ioread32(status)) {
        case 0: msg = "idle\n"; break;
        case 1: msg = "busy\n"; break;
        case 2: msg = "done\n"; break;
        default: msg = "unknown\n";
    }
    len = strlen(msg);
    if (copy_to_user(ubuf, msg, len)) return -EFAULT;
    *ppos = len;
    return len;
}

static ssize_t restma_read(struct file *f, char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[64];
    int len;
    if (*ppos > 0) return 0;
    if (ioread32(status) != 2) return -EAGAIN;
    len = format_fp(((u64)ioread32(res_h) << 32) | ioread32(res_l), buf, sizeof(buf));
    if (copy_to_user(ubuf, buf, len)) return -EFAULT;
    *ppos = len;
    return len;
}

static const struct file_operations a1stma_ops = { .owner = THIS_MODULE, .write = a1stma_write };
static const struct file_operations a2stma_ops = { .owner = THIS_MODULE, .write = a2stma_write };
static const struct file_operations ctstma_ops = { .owner = THIS_MODULE, .write = ctstma_write };
static const struct file_operations ststma_ops = { .owner = THIS_MODULE, .read  = ststma_read };
static const struct file_operations restma_ops = { .owner = THIS_MODULE, .read  = restma_read };

static struct proc_dir_entry *proc_dir;

static int __init my_init_module(void)
{
    printk(KERN_INFO "FP multiplier: init\n");
    baseptr = ioremap(SYKT_GPIO_BASE_ADDR, SYKT_GPIO_SIZE);
    if (!baseptr) return -ENOMEM;

    arg1_h = baseptr + 0x0100;
    arg1_l = baseptr + 0x0108;
    arg2_h = baseptr + 0x00F0;
    arg2_l = baseptr + 0x00F8;
    ctrl   = baseptr + 0x00D0;
    status = baseptr + 0x00E8;
    res_h  = baseptr + 0x00D8;
    res_l  = baseptr + 0x00E0;

    proc_dir = proc_mkdir("sykom", NULL);
    if (!proc_dir) goto err_iounmap;

    if (!proc_create("a1stma", 0220, proc_dir, &a1stma_ops) ||
        !proc_create("a2stma", 0220, proc_dir, &a2stma_ops) ||
        !proc_create("ctstma", 0220, proc_dir, &ctstma_ops) ||
        !proc_create("ststma", 0444, proc_dir, &ststma_ops) ||
        !proc_create("restma", 0444, proc_dir, &restma_ops))
        goto err_cleanup;

    printk(KERN_INFO "FP multiplier: ready\n");
    return 0;

err_cleanup:
    proc_remove(proc_dir);
err_iounmap:
    iounmap(baseptr);
    return -ENOMEM;
}

static void __exit my_cleanup_module(void)
{
    printk(KERN_INFO "FP multiplier: cleanup\n");
    iowrite32(SYKT_EXIT | (SYKT_EXIT_CODE << 16), baseptr);
    proc_remove(proc_dir);
    iounmap(baseptr);
}

module_init(my_init_module);
module_exit(my_cleanup_module);
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
#include <linux/math64.h>

MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student SYKOM");
MODULE_DESCRIPTION("FP64 multiplier (PROCFS, full scientific format)");
MODULE_VERSION("0.2");

#define SYKT_GPIO_BASE_ADDR  (0x00100000)
#define SYKT_GPIO_SIZE       (0x8000)
#define SYKT_EXIT            (0x3333)
#define SYKT_EXIT_CODE       (0x7F)
#define CTRL_START           1
#define BIAS                 67108864UL

static void __iomem *baseptr;
static void __iomem *arg1_h, *arg1_l, *arg2_h, *arg2_l;
static void __iomem *ctrl, *status, *res_h, *res_l;

/* ---------- Funkcje konwersji z tekstu ---------- */

static u8 Convert_from_text_s(const char **p)
{
    if (**p == '-') { (*p)++; return 1; }
    if (**p == '+') (*p)++;
    return 0;
}

static int Convert_from_text_e(const char **p, int *exp)
{
    int sign = 1;
    long val;
    if (**p != 'e' && **p != 'E') return -EINVAL;
    (*p)++;
    if (**p == '-') { sign = -1; (*p)++; }
    else if (**p == '+') { (*p)++; }
    if (!isdigit(**p)) return -EINVAL;
    val = simple_strtol(*p, (char **)p, 10);
    *exp = sign * val;
    return 0;
}

static int Convert_from_text_m(const char **p, u64 *mant_out, int *exp_corr)
{
    u64 v = 0;
    int digits = 0;
    int frac_digits = 0;
    int seen_dot = 0;

    /* parsuj maks. 9 cyfr znaczących */
    while (**p == '.' || isdigit(**p)) {
        if (**p == '.') {
            if (seen_dot)
                return -EINVAL;
            seen_dot = 1;
        } else {
            if (digits < 9) {
                v = v * 10 + (**p - '0');
                digits++;
                if (seen_dot)
                    frac_digits++;
            } else {
                /* ignoruj kolejne cyfry, ale licz pozycję */
                if (seen_dot)
                    frac_digits++;
            }
        }
        (*p)++;
    }

    if (digits == 0)   /* brak cyfr */
        return -EINVAL;

    if (v == 0) {
        *mant_out = 0;
        *exp_corr = 0;
        return 0;
    }

    /* zamień dziesiętne na przybliżone binarne */
    *exp_corr = 0;

    while (frac_digits-- > 0) {
        v = div64_u64(v, 10);
        (*exp_corr)--;
    }

    /* normalizacja binarna do [2^36 .. 2^37) */
    while (v >= (1ULL << 37)) {
        v >>= 1;
        (*exp_corr)++;
    }
    while (v < (1ULL << 36)) {
        v <<= 1;
        (*exp_corr)--;
    }

    /* usuń ukrytą jedynkę */
    *mant_out = v - (1ULL << 36);

    return 0;
}

/* ---------- Makra pakujące ---------- */
#define SET_S(s)         ((u64)((s) & 1) << 0)
#define SET_E_H(e)       ((u64)((e) & 0x7FFFFFF) << 1)
#define SET_E_L(e)       0
#define SET_M_H(m)       ((u64)((m) & 0xFFFFFFFFFULL) << 28)
#define SET_M_L(m)       0

/* ---------- Parsowanie ---------- */
static int parse_fp(const char *buf, u64 *val)
{
    const char *p = buf;
    u8 s;
    int e_dec, e_corr;
    u64 m;
    long e_final;
    u32 a1_h, a1_l;
    s = Convert_from_text_s(&p);
    if (Convert_from_text_m(&p, &m, &e_corr)) return -EINVAL;
    e_dec = 0;
    if (*p == 'e' || *p == 'E') {
        if (Convert_from_text_e(&p, &e_dec)) return -EINVAL;
    }
    while (isspace(*p)) p++;
    if (*p != '\0') return -EINVAL;
    if (m == 0) { *val = 0; return 0; }
    e_final = (long)e_dec + e_corr + BIAS;
    if (e_final < 0 || e_final > 0x7FFFFFF) return -EINVAL;
    a1_h = SET_M_H(m);
    a1_l = SET_M_L(m);
    a1_h |= SET_E_H((u32)e_final);
    a1_l |= SET_E_L((u32)e_final);
    a1_l |= SET_S(s);
    *val = ((u64)a1_h << 32) | a1_l;
    return 0;
}

/* ---------- Formatowanie wyniku (prawdziwy format naukowy) ---------- */
static int format_fp(u64 val, char *buf, size_t len)
{
    int sign;
    long exp2, exp10 = 0;
    u64 mant;

    if (val == 0)
        return snprintf(buf, len, "0.0e0\n");

    sign = val & 1;
    exp2 = ((val >> 1) & 0x7FFFFFF) - BIAS;
    mant = (1ULL << 36) | ((val >> 28) & 0xFFFFFFFFFULL);

    /* Przeskaluj z binarnego na dziesiętny */
    while (exp2 > 0) {
        mant <<= 1;
        exp2--;
    }
    while (exp2 < 0) {
        mant >>= 1;
        exp2++;
    }

    /* Normalizacja dziesiętna */
    while (mant >= (10ULL << 36)) {
        mant = div64_u64(mant, 10);
        exp10++;
    }
    while (mant < (1ULL << 36)) {
        mant *= 10;
        exp10--;
    }

    /* Wyciągnij np. 6 cyfr po kropce */
    u64 int_part = mant >> 36;
    u64 frac_part = ((mant & ((1ULL << 36) - 1)) * 1000000ULL) >> 36;

    return snprintf(buf, len,
        "%s%llu.%06llue%ld\n",
        sign ? "-" : "",
        int_part, frac_part, exp10);
}

/* ---------- Obsługa plików PROCFS ---------- */
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

static ssize_t ctstma_write(struct file *f,
                            const char __user *ubuf,
                            size_t count, loff_t *ppos)
{
    char kbuf[16];
    long cmd;

    if (count >= sizeof(kbuf))
        return -ENOSPC;
    if (copy_from_user(kbuf, ubuf, count))
        return -EFAULT;

    kbuf[count] = '\0';

    if (kstrtol(kbuf, 10, &cmd))
        return -EINVAL;

    printk(KERN_INFO "WRITE CTRL addr=%p val=%ld\n", ctrl, cmd);

    /* AKCEPTUJEMY 0 i 1 */
    if (cmd == 0) {
        iowrite32(0, ctrl);          /* reset FSM */
    } else if (cmd == 1) {
        iowrite32(1, ctrl);          /* start */
        msleep(1);
    } else {
        return -EINVAL;
    }

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

static const struct file_operations a1stma_ops = {
    .owner = THIS_MODULE,
    .write = a1stma_write,
};

static const struct file_operations a2stma_ops = {
    .owner = THIS_MODULE,
    .write = a2stma_write,
};

static const struct file_operations ctstma_ops = {
    .owner = THIS_MODULE,
    .write = ctstma_write,
};

static const struct file_operations ststma_ops = {
    .owner = THIS_MODULE,
    .read  = ststma_read,
};

static const struct file_operations restma_ops = {
    .owner = THIS_MODULE,
    .read  = restma_read,
};

/* ---------- Inicjalizacja / wyjście ---------- */
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

    /* zakończenie symulacji QEMU */
    iowrite32(SYKT_EXIT | (SYKT_EXIT_CODE << 16), baseptr);

    proc_remove(proc_dir);
    iounmap(baseptr);
}

module_init(my_init_module)
module_exit(my_cleanup_module)
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <asm/io.h>
#include <linux/math64.h>

MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Malgorzata Dominika Stypa");
MODULE_DESCRIPTION("Simple kernel module for SYKOM project - FP64 multiplier");
MODULE_VERSION("beta");

// Format: bit[0]=znak, bits[27:1]=wykładnik(27b, BIAS=2^26), bits[63:28]=mantysa(36b)
#define SYKT_GPIO_BASE_ADDR (0x00100000)
#define SYKT_GPIO_SIZE      (0x8000)
#define SYKT_EXIT           (0x3333)
#define SYKT_EXIT_CODE      (0x7F)
#define BIAS                67108864ULL   // 2^26

// dla funkcji format_fp
#define TMP_SIZE 32
#define FRAC_DIGITS 9 // liczba cyfr ułamkowych do wyświetlenia (1 cyfra przed przecinkiem + 9 cyfr = 10 cyfr znaczących)
#define OUT_SIZE (FRAC_DIGITS + 3)  // wynikowy rozmiar w bajtach: 1 (cyfra) + 1 (kropka) + 10 (ułamkowe) + 1 (terminator) = 12

static void __iomem *base;
static void __iomem *arg1_h, *arg1_l, *arg2_h, *arg2_l;
static void __iomem *io_ctrl, *io_status, *res_h, *res_l;

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *pe_a1, *pe_a2, *pe_ctrl, *pe_stat, *pe_res;

static int parse_fp(const char *buf, u64 *out)
{
    int sign = 0, frac_digits = 0, exp_e = 0, e_sign = 1, i, fin_exp;
    u64 int_part = 0, frac_part = 0, value, mantissa;
    int binary_exp = 0, exp10;
    const char *p = buf;

    if (*p == '-') { sign = 1; p++; } else if (*p == '+') p++;

    while (isdigit(*p)) {
        if (int_part > (U64_MAX / 10)) return -ERANGE;
        int_part = int_part * 10 + (*p++ - '0');
    }
    if (*p == '.') {
        p++;
        while (isdigit(*p) && frac_digits < 20) { // ograniczenie do 20 cyfr ułamkowych dla bezpieczeństwa
            if (frac_part > (U64_MAX / 10)) break; // zabezpieczenie przed przepełnieniem
            frac_part = frac_part * 10 + (*p++ - '0');
            frac_digits++;
        }
        while (isdigit(*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '-') { e_sign = -1; p++; } else if (*p == '+') p++;
        while (isdigit(*p)) {
            exp_e = exp_e * 10 + (*p++ - '0');
        }
        exp_e *= e_sign;
    }
    while (isspace(*p)) p++;
    if (*p != '\0') return -EINVAL;

    if (!int_part && !frac_part) { *out = 0; return 0; }

    value = int_part;
    for (i = 0; i < frac_digits; i++) {
        if (value > (U64_MAX / 10)) return -ERANGE;
        value *= 10;
    }
    value += frac_part;
    if (value < int_part) return -ERANGE;   // przepełnienie przy dodawaniu

    exp10 = exp_e - frac_digits;

    while (value < (1ULL << 60)) { value <<= 1; binary_exp--; }
    while (value >= (1ULL << 61)) { value >>= 1; binary_exp++; }

    while (exp10 > 0) {
        // Jeśli nie mieści się po pomnożeniu, przesuń w prawo
        while (value > (U64_MAX / 10)) {
            value >>= 1;
            binary_exp++;
        }
        value *= 10;
        // Po mnożeniu znormalizuj do zakresu [2^60, 2^61)
        while (value >= (1ULL << 61)) {
            value >>= 1;
            binary_exp++;
        }
        exp10--;
    }

    while (exp10 < 0) {
        if (value == 0) { // underflow -> zero
            *out = 0;
            return 0;
        }
        while (value < (1ULL << 60)) {
            value <<= 1;
            binary_exp--;
        }
        do_div(value, 10);
        exp10++;
    }

    while (value >= (1ULL << 37)) { value >>= 1; binary_exp++; }
    while (value <  (1ULL << 36)) { value <<= 1; binary_exp--; }

    // fin_exp = binary_exp + (int)BIAS;
    fin_exp = binary_exp + 36 + (int)BIAS;

    if (fin_exp < 0 || fin_exp > 0x7FFFFFF) return -ERANGE;

    mantissa = value - (1ULL << 36);
    *out = (mantissa << 28) | ((u64)(u32)fin_exp << 1) | (u64)sign;
    return 0;
}

// X.XXXXXXXXXe±D\n
static int format_fp(u64 val, char *buf, size_t size)
{
    int sign, bin_exp, dec_exp = 0, sci_exp, len, i;
    u64 mant, dec;
    char tmp[TMP_SIZE], out[OUT_SIZE];

    // obsługa zera (w tym ujemne zero M=0, E=0, S=1)
    if (val == 0 || val == 1) { // val == 1 to tylko bit znaku
        return snprintf(buf, size, "0.0e0\n");
    }

    sign = (int)(val & 1);
    mant = (1ULL << 36) | ((val >> 28) & ((1ULL << 36) - 1)); // Mantysa ma 36 bitów + 1 bit ukryty
    // Wykładnik binarny skorygowany o wagę mantysy i BIAS
    bin_exp = (int)((val >> 1) & 0x7FFFFFFU) - (int)BIAS - 36;
    dec = mant;

    // Obsługa wykładników dodatnich (mnożenie przez 2)
    while (bin_exp > 0) {
        if (dec > (U64_MAX >> 1)) {
            do_div(dec, 10);
            dec_exp++;
        }
        dec <<= 1; bin_exp--;
    }

    // Obsługa wykładników ujemnych (mnożenie przez 10, potem dzielenie przez 2)
    while (bin_exp < 0) {
        if (dec <= (U64_MAX / 10)) {
            // Mnożymy przez 10, aby przesunąć przecinek w prawo
            dec *= 10;
            dec_exp--;
        } else {
            // Jeśli liczba jest za duża na mnożenie, dopiero wtedy redukujemy wykładnik binarny
            dec >>= 1;
            bin_exp++;
        }
    }

    len = snprintf(tmp, TMP_SIZE, "%llu", dec);
    if (len <= 0) return -EINVAL;
    sci_exp = (len - 1) + dec_exp;

    out[0] = tmp[0]; out[1] = '.';
    for (i = 2; i < OUT_SIZE - 1; i++)
        out[i] = (i - 1 < len) ? tmp[i - 1] : '0';
    out[OUT_SIZE - 1] = '\0';

    return snprintf(buf, size, "%s%se%d\n", sign ? "-" : "", out, sci_exp);
}

static ssize_t arg_write(const char __user *ubuf, size_t cnt, loff_t *off, void __iomem *hi, void __iomem *lo)
{
    char buf[64]; u64 val;
    u64 val;
    int ret;

    if (cnt >= sizeof(buf)) return -ENOSPC;
    if (copy_from_user(buf, ubuf, cnt)) return -EFAULT;
    buf[cnt] = '\0';

    ret = parse_fp(buf, &val);
    if (ret < 0) return ret; // Zwrócenie błędu z parse_fp (np. -EINVAL, -ERANGE)

    iowrite32((u32)(val >> 32), hi);
    iowrite32((u32)(val & 0xFFFFFFFFU), lo);

    *off += cnt;
    return cnt;
}

static ssize_t a1_write(struct file *f, const char __user *b, size_t c, loff_t *o)
    { return arg_write(b, c, o, arg1_h, arg1_l); }
static ssize_t a2_write(struct file *f, const char __user *b, size_t c, loff_t *o)
    { return arg_write(b, c, o, arg2_h, arg2_l); }

static ssize_t ctrl_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *off)
{
    char buf[8];
    long cmd;
    int ret;
    
    if (cnt >= sizeof(buf)) return -ENOSPC;
    if (copy_from_user(buf, ubuf, cnt)) return -EFAULT;
    buf[cnt] = '\0';

    ret = kstrtol(buf, 10, &cmd);
    if (ret < 0) return ret; // nieprawidłowa liczba
    if (cmd != 0 && cmd != 1) return -EINVAL; // niedozwolona wartość
    
    iowrite32((u32)cmd, io_ctrl);
    *off += cnt;
    return cnt;
}

static ssize_t status_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *off)
{
    u32 st; const char *msg; size_t len;
    if (*off) return 0;
    st  = ioread32(io_status);
    switch (st) {
        case 0:  msg = "idle\n"; break;
        case 1:  msg = "busy\n"; break;
        case 2:  msg = "done\n"; break;
        default: msg = "unknown/error\n"; break; // Obsługa wartości nieoczekiwanych
    }

    len = strlen(msg);
    if (cnt < len) return -EINVAL; // nieprawidłowy argument
    if (copy_to_user(ubuf, msg, len)) return -EFAULT; // błąd dostępu do pamięci
    *off = len; return len;
}

static ssize_t result_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *off)
{
    u64 v; char buf[64]; int len;
    if (*off) return 0;
    if (ioread32(io_status) != 2) return -EAGAIN;
    v   = ((u64)ioread32(res_h) << 32) | (u64)ioread32(res_l);
    printk(KERN_INFO "FP64 mul: res_h=0x%08X res_l=0x%08X raw=0x%016llX\n", ioread32(res_h), ioread32(res_l), v); // debug
    len = format_fp(v, buf, sizeof(buf));
    if (len < 0 || (size_t)len > cnt) return -EINVAL;
    if (copy_to_user(ubuf, buf, len)) return -EFAULT;
    *off = len; return len;
}

static const struct file_operations a1_fops = { .write = a1_write };
static const struct file_operations a2_fops = { .write = a2_write };
static const struct file_operations ctrl_fops = { .write = ctrl_write };
static const struct file_operations stat_fops = { .read  = status_read };
static const struct file_operations res_fops = { .read  = result_read };

int my_init_module(void)
{
    printk(KERN_INFO "Init my module.\n");

    base = ioremap(SYKT_GPIO_BASE_ADDR, SYKT_GPIO_SIZE);
    if (!base) return -ENOMEM;

    arg1_h = base + 0x100; arg1_l = base + 0x108;
    arg2_h = base + 0x0F0; arg2_l = base + 0x0F8;
    io_ctrl = base + 0x0D0; io_status = base + 0x0E8;
    res_h = base + 0x0D8; res_l = base + 0x0E0;

    proc_dir = proc_mkdir("sykom", NULL);
    if (!proc_dir) goto fail;

    pe_a1   = proc_create("a1stma", 0220, proc_dir, &a1_fops);
    pe_a2   = proc_create("a2stma", 0220, proc_dir, &a2_fops);
    pe_ctrl = proc_create("ctstma", 0220, proc_dir, &ctrl_fops);
    pe_stat = proc_create("ststma", 0444, proc_dir, &stat_fops);
    pe_res  = proc_create("restma", 0444, proc_dir, &res_fops);

    if (!pe_a1 || !pe_a2 || !pe_ctrl || !pe_stat || !pe_res) {
        if (pe_a1)   proc_remove(pe_a1);
        if (pe_a2)   proc_remove(pe_a2);
        if (pe_ctrl) proc_remove(pe_ctrl);
        if (pe_stat) proc_remove(pe_stat);
        if (pe_res)  proc_remove(pe_res);
        proc_remove(proc_dir);
        goto fail;
    }
    printk(KERN_INFO "FP64 multiplier: ready\n");
    return 0;

    fail:
        iounmap(base);
        return -ENOMEM;
}

void my_cleanup_module(void)
{
    printk(KERN_INFO "Cleanup my module.\n");

    // Wpisanie kodu wyjscia do rejestru sterującego emulatora
    iowrite32(SYKT_EXIT | (SYKT_EXIT_CODE << 16), base);
    proc_remove(pe_a1);   proc_remove(pe_a2);
    proc_remove(pe_ctrl); proc_remove(pe_stat); proc_remove(pe_res);
    proc_remove(proc_dir);
    iounmap(base);
}

module_init(my_init_module);
module_exit(my_cleanup_module);
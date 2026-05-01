#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/errno.h>
#include <asm/io.h>

MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ignacy Kopka");
MODULE_DESCRIPTION("FPU Kernel Module for SYKOM project");
MODULE_VERSION("PRO-ORIGINAL");

#define SYKT_GPIO_BASE_ADDR (0x00100000)
#define SYKT_GPIO_SIZE      (0x8000)
#define SYKT_EXIT           (0x3333)
#define SYKT_EXIT_CODE      (0x7F)

// TWOJE ORYGINALNE ADRESY Z ZADANIA
#define REG_ARG1_LO 0x0180
#define REG_ARG1_HI 0x0188
#define REG_ARG2_LO 0x0190
#define REG_ARG2_HI 0x0198
#define REG_STATUS  0x01A0
#define REG_RES_LO  0x01A8
#define REG_RES_HI  0x01B0
#define REG_CTRL    0x01B8

#define PROCFS_MAX_SIZE 128

void __iomem *baseptr;
static struct proc_dir_entry *proc_a1koig, *proc_a2koig, *proc_rekoig, *proc_stkoig, *proc_ctkoig;

static void parse_scientific_to_fpu(const char *str, u32 *hi, u32 *lo) {
    u32 integer_part = 0, fraction_part = 0, fraction_digits = 0;
    int exp_part = 0, sign = 0, exp_sign = 1, i = 0;
    *hi = 0; *lo = 0;

    if (str[i] == '-') { sign = 1; i++; } else if (str[i] == '+') { i++; }
    while (str[i] >= '0' && str[i] <= '9') { integer_part = integer_part * 10 + (str[i] - '0'); i++; }
    if (str[i] == '.') {
        i++;
        while (str[i] >= '0' && str[i] <= '9') {
            if (fraction_digits < 9) { fraction_part = fraction_part * 10 + (str[i] - '0'); fraction_digits++; }
            i++;
        }
    }
    if (str[i] == 'e' || str[i] == 'E') {
        i++;
        if (str[i] == '-') { exp_sign = -1; i++; } else if (str[i] == '+') { i++; }
        while (str[i] >= '0' && str[i] <= '9') { exp_part = exp_part * 10 + (str[i] - '0'); i++; }
    }
    if (integer_part == 0 && fraction_part == 0) { *hi = (sign << 21); *lo = 0; return; }

    int total_exp10 = (exp_part * exp_sign) - fraction_digits;
    u64 value = integer_part;
    int j;
    for (j = 0; j < fraction_digits; j++) value = (value * 10);
    value += fraction_part;
    
    int binary_exp = 52;
    int shift_up = 0;
    while (value > 0 && value < (1ULL << 60)) { value <<= 1; shift_up++; }
    binary_exp -= shift_up;

    while(total_exp10 > 0) {
        if (value >= (1ULL << 60)) { value >>= 4; binary_exp += 4; }
        u64 val_8 = value << 3; u64 val_2 = value << 1;
        value = val_8 + val_2;
        total_exp10--;
    }
    while(total_exp10 < 0) {
        u64 temp = 0, rem = value; int bits = 63;
        while (bits >= 0) {
            temp <<= 1; u64 masked = (rem >> bits);
            if (masked >= 10) { temp |= 1; rem -= (10ULL << bits); }
            bits--;
        }
        value = temp;
        while (value > 0 && value < (1ULL << 60)) { value <<= 1; binary_exp--; }
        total_exp10++;
    }

    if (value > 0) {
        // Wyrównanie do 52 bitów
        while (value < (1ULL << 52)) { value <<= 1; binary_exp--; }
        while (value >= (1ULL << 53)) { value >>= 1; binary_exp++; }
        
        // Przesuwamy ułamek o 1 bit w lewo, aby wypełnić niestandardową 53-bitową mantysę
        value <<= 1;
    } else { *hi = (sign << 21); *lo = 0; return; }

    u32 actual_exp = (binary_exp + 511) & 0x3FF; 
    u32 mantissa_hi = (value >> 32) & 0x1FFFFF; // Maska na 21 bitów HI
    u32 mantissa_lo = (value & 0xFFFFFFFF);     // Maska na 32 bity LO
    
    *hi = (actual_exp << 22) | (sign << 21) | mantissa_hi;
    *lo = mantissa_lo;
}

// Bezpieczna funkcja dzieląca 64-bitową liczbę przez 10 dla 32-bitowych jąder
static u32 div_u64_by_10(u64 *val) {
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

// Formatowanie wyliczonych bitów na system dziesiętny naukowy z zachowaniem surowego RAW
static void format_fpu_to_scientific(u32 hi, u32 lo, char *buf) {
    u32 exp = (hi >> 22) & 0x3FF;
    u32 sign = (hi >> 21) & 0x1;
    u32 mantissa_hi = hi & 0x1FFFFF;
    u64 mantissa = (((u64)mantissa_hi) << 32) | lo;

    // Obsługa zera
    if (exp == 0 && mantissa == 0) {
        snprintf(buf, PROCFS_MAX_SIZE, "RAW: HI=0x%08X LO=0x%08X | DEC: %s0.000000e0\n", hi, lo, sign ? "-" : "");
        return;
    }
    // Obsługa nieskończoności
    if (exp == 0x3FF) {
        snprintf(buf, PROCFS_MAX_SIZE, "RAW: HI=0x%08X LO=0x%08X | DEC: %sInf\n", hi, lo, sign ? "-" : "+");
        return;
    }

    // Dodanie ukrytego bitu jedności
    u64 val = (1ULL << 53) | mantissa;
    int bin_exp = (int)exp - 511 - 53;
    int dec_exp = 0;

    // Przeliczenie potęg dwójki na potęgi dziesiątki (z utrzymaniem precyzji w 64 bitach)
    while (bin_exp > 0) {
        if (val >= (0xFFFFFFFFFFFFFFFFULL / 2)) {
            div_u64_by_10(&val);
            dec_exp++;
        } else {
            val <<= 1;
            bin_exp--;
        }
    }

    while (bin_exp < 0) {
        if (val >= (0xFFFFFFFFFFFFFFFFULL / 5)) {
            div_u64_by_10(&val);
            dec_exp++;
        } else {
            val *= 5; // To działa jak pomnożenie przez 10 i podzielenie przez 2
            bin_exp++;
            dec_exp--;
        }
    }

    // Normalizacja ułamka do 7 precyzyjnych cyfr dziesiętnych
    while (val >= 10000000ULL) {
        div_u64_by_10(&val);
        dec_exp++;
    }
    while (val > 0 && val < 1000000ULL) {
        val *= 10;
        dec_exp--;
    }

    // Wyciągnięcie końcowych składników
    int e = dec_exp + 6;
    u32 val32 = (u32)val; 
    u32 integer_part = val32 / 1000000;
    u32 fraction_part = val32 % 1000000;

    // Połączenie RAW i DEC w jeden czytelny łańcuch znaków
    snprintf(buf, PROCFS_MAX_SIZE, "RAW: HI=0x%08X LO=0x%08X | DEC: %s%u.%06ue%d\n", 
             hi, lo, sign ? "-" : "", integer_part, fraction_part, e);
}

static ssize_t a1koig_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[PROCFS_MAX_SIZE]; u32 hi, lo;
    if (count >= PROCFS_MAX_SIZE) count = PROCFS_MAX_SIZE - 1;
    if (copy_from_user(buf, ubuf, count)) return -EFAULT;
    buf[count] = '\0';
    parse_scientific_to_fpu(buf, &hi, &lo);
    writel(lo, baseptr + REG_ARG1_LO);
    writel(hi, baseptr + REG_ARG1_HI);
    return count;
}

static ssize_t a2koig_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[PROCFS_MAX_SIZE]; u32 hi, lo;
    if (count >= PROCFS_MAX_SIZE) count = PROCFS_MAX_SIZE - 1;
    if (copy_from_user(buf, ubuf, count)) return -EFAULT;
    buf[count] = '\0';
    parse_scientific_to_fpu(buf, &hi, &lo);
    writel(lo, baseptr + REG_ARG2_LO);
    writel(hi, baseptr + REG_ARG2_HI);
    return count;
}

static ssize_t ctkoig_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[16];
    if (count >= sizeof(buf)) count = sizeof(buf) - 1;
    if (copy_from_user(buf, ubuf, count)) return -EFAULT;
    buf[count] = '\0';
    if (buf[0] == '1') writel(1, baseptr + REG_CTRL);
    else if (buf[0] == '0') writel(0, baseptr + REG_CTRL);
    return count;
}

static ssize_t rekoig_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[PROCFS_MAX_SIZE];
    if (*ppos > 0) return 0; // Usunięto wadliwy warunek 'count < ...'
    
    u32 lo = readl(baseptr + REG_RES_LO);
    u32 hi = readl(baseptr + REG_RES_HI);
    format_fpu_to_scientific(hi, lo, buf);
    int len = strlen(buf);
    
    if (count < len) return -EINVAL; // Opcjonalne zabezpieczenie przed zbyt małym buforem
    if (copy_to_user(ubuf, buf, len)) return -EFAULT;
    *ppos = len;
    return len;
}

static ssize_t stkoig_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos) {
    char buf[32];
    if (*ppos > 0) return 0; // Usunięto wadliwy warunek
    
    u32 status = readl(baseptr + REG_STATUS);
    int len = snprintf(buf, sizeof(buf), "Status: 0x%08X\n", status);
    if (copy_to_user(ubuf, buf, len)) return -EFAULT;
    *ppos = len;
    return len;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops a1koig_ops = { .proc_write = a1koig_write };
static const struct proc_ops a2koig_ops = { .proc_write = a2koig_write };
static const struct proc_ops ctkoig_ops = { .proc_write = ctkoig_write };
static const struct proc_ops rekoig_ops = { .proc_read  = rekoig_read };
static const struct proc_ops stkoig_ops = { .proc_read  = stkoig_read };
#else
static const struct file_operations a1koig_ops = { .write = a1koig_write };
static const struct file_operations a2koig_ops = { .write = a2koig_write };
static const struct file_operations ctkoig_ops = { .write = ctkoig_write };
static const struct file_operations rekoig_ops = { .read  = rekoig_read };
static const struct file_operations stkoig_ops = { .read  = stkoig_read };
#endif

int my_init_module(void){
    printk(KERN_INFO "SYKOM: Zaladowano sterownik FPU (ORYGINALNE ADRESY).\n");
    baseptr = ioremap(SYKT_GPIO_BASE_ADDR, SYKT_GPIO_SIZE);
    if (!baseptr) return -ENOMEM;
    proc_a1koig = proc_create("a1koig", 0666, NULL, &a1koig_ops);
    proc_a2koig = proc_create("a2koig", 0666, NULL, &a2koig_ops);
    proc_rekoig = proc_create("rekoig", 0444, NULL, &rekoig_ops);
    proc_stkoig = proc_create("stkoig", 0444, NULL, &stkoig_ops);
    proc_ctkoig = proc_create("ctkoig", 0666, NULL, &ctkoig_ops);
    return 0;
}

void my_cleanup_module(void){
    proc_remove(proc_a1koig); proc_remove(proc_a2koig); proc_remove(proc_rekoig);
    proc_remove(proc_stkoig); proc_remove(proc_ctkoig);
    writel(SYKT_EXIT | ((SYKT_EXIT_CODE)<<16), baseptr);
    iounmap(baseptr);
}
module_init(my_init_module); module_exit(my_cleanup_module);
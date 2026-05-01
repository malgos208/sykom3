#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <asm/io.h>
#include <linux/math64.h>

MODULE_LICENSE("GPL");

#define BASE_ADDR 0x00100000
#define BIAS      67108864ULL

static void __iomem *base;
static struct proc_dir_entry *proc_dir;

// Pomocnicza funkcja parsująca (uproszczona)
static u64 text_to_fp64(const char *buf) {
    u64 m = 0; int e = 0, s = 0;
    // Przykład dla 2.5e0: mantysa 2.5 -> normalizowana do formatu [63:28]
    // Tutaj należy zaimplementować pełne Convert_from_text[cite: 5]
    if (buf[0] == '-') s = 1;
    // ... logika konwersji ...
    return (m << 28) | ((BIAS + e) << 1) | s;
}

static ssize_t a_write(struct file *f, const char __user *ub, size_t c, loff_t *o, int addr_h, int addr_l) {
    char kbuf[64];
    if (c >= 64) return -EINVAL;
    if (copy_from_user(kbuf, ub, c)) return -EFAULT;
    kbuf[c] = '\0';
    u64 val = text_to_fp64(kbuf);
    iowrite32(val >> 32, base + addr_h);
    iowrite32(val & 0xFFFFFFFF, base + addr_l);
    return c;
}

static ssize_t st_read(struct file *f, char __user *ub, size_t c, loff_t *o) {
    if (*o) return 0;
    u32 s = ioread32(base + 0xE8);
    const char *msg = (s == 2) ? "done\n" : (s == 1 ? "busy\n" : "idle\n");
    size_t len = strlen(msg);
    if (copy_to_user(ub, msg, len)) return -EFAULT;
    *o = len; return len;
}

static ssize_t res_read(struct file *f, char __user *ub, size_t c, loff_t *o) {
    if (*o) return 0;
    u64 v = ((u64)ioread32(base + 0xD8) << 32) | ioread32(base + 0xE0);
    char buf[64];
    // Rekonstrukcja: Mantysa=v[63:28], Exp=v[27:1]-BIAS, Sign=v[0]
    snprintf(buf, 64, "%llu.0e0\n", (v >> 28)); // Format dziesiętny naukowy
    size_t len = strlen(buf);
    if (copy_to_user(ub, buf, len)) return -EFAULT;
    *o = len; return len;
}

static ssize_t a1_w(struct file *f, const char __user *b, size_t c, loff_t *o) { return a_write(f, b, c, o, 0x100, 0x108); }
static ssize_t a2_w(struct file *f, const char __user *b, size_t c, loff_t *o) { return a_write(f, b, c, o, 0xF0, 0xF8); }
static ssize_t ct_w(struct file *f, const char __user *b, size_t c, loff_t *o) {
    char k[8]; if (copy_from_user(k, b, (c < 8 ? c : 7))) return -EFAULT;
    iowrite32(k[0] == '1' ? 1 : 0, base + 0xD0);
    return c;
}

static const struct file_operations f_a1={.write=a1_w}, f_a2={.write=a2_w}, f_ct={.write=ct_w}, f_st={.read=st_read}, f_re={.read=res_read};

static int __init start(void) {
    base = ioremap(BASE_ADDR, 0x1000);
    proc_dir = proc_mkdir("sykom", NULL);
    proc_create("a1stma", 0666, proc_dir, &f_a1);
    proc_create("a2stma", 0666, proc_dir, &f_a2);
    proc_create("ctstma", 0666, proc_dir, &f_ct);
    proc_create("ststma", 0444, proc_dir, &f_st);
    proc_create("restma", 0444, proc_dir, &f_re);
    return 0;
}

static void __exit stop(void) {
    iowrite32(0x3333 | (0x7F << 16), base); // Finisher[cite: 1]
    remove_proc_entry("sykom", NULL);
    iounmap(base);
}

module_init(start); module_exit(stop);
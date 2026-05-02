#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Ścieżki PROCFS */
#define A1   "/proc/sykom/a1stma"
#define A2   "/proc/sykom/a2stma"
#define CTL  "/proc/sykom/ctstma"
#define STA  "/proc/sykom/ststma"
#define RES  "/proc/sykom/restma"

/* --------------------------------------------------------- */
/* Pomocnicze funkcje I/O                                    */
/* --------------------------------------------------------- */

static int write_str(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fputs(s, f);
    fclose(f);
    return 0;
}

static int read_str(const char *path, char *buf, size_t maxlen)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    if (!fgets(buf, maxlen, f)) {
        fclose(f);
        return -EINVAL;
    }
    fclose(f);
    return 0;
}

/* --------------------------------------------------------- */
/* Protokół START / ACK                                      */
/* --------------------------------------------------------- */

static int start_operation(void)
{
    return write_str(CTL, "1\n");
}

static int ack_operation(void)
{
    return write_str(CTL, "0\n");
}

static int wait_done(void)
{
    char buf[32];
    for (;;) {
        if (read_str(STA, buf, sizeof(buf))) return -1;
        if (!strncmp(buf, "done", 4))
            return 0;
        usleep(10000); /* 10 ms */
    }
}

/* --------------------------------------------------------- */
/* Test z błędem procentowym                                 */
/* --------------------------------------------------------- */

static int run_test(const char *label,
                    const char *a1,
                    const char *a2,
                    double reference)
{
    char res_buf[64];
    double measured;
    double err_percent;

    printf("\n[%s]\n", label);
    printf("  a1 = %s", a1);
    printf("  a2 = %s", a2);
    printf("  wartosc referencyjna = %.12g\n", reference);

    if (write_str(A1, a1)) return -1;
    if (write_str(A2, a2)) return -1;

    if (start_operation()) return -1;
    if (wait_done()) return -1;

    if (read_str(RES, res_buf, sizeof(res_buf))) return -1;

    measured = strtod(res_buf, NULL);

    printf("  wynik z modulu = %s", res_buf);

    if (reference == 0.0) {
        if (measured == 0.0)
            printf("  blad: 0.0%% (oba wyniki zerowe)\n");
        else
            printf("  blad: nieokreslony (dzielenie przez zero)\n");
    } else {
        err_percent = (measured - reference) / reference * 100.0;
        printf("  blad wzgledny: %+0.3f%%\n", err_percent);
    }

    if (ack_operation()) return -1;
    return 0;
}

/* --------------------------------------------------------- */
/* main                                                      */
/* --------------------------------------------------------- */

int main(void)
{
    printf("FP64 multiplier – testy z bledem procentowym\n");
    printf("===============================================\n");

    run_test("TEST 1: 1.0 * 2.0",
             "1.0e0\n",
             "2.0e0\n",
             2.0);

    run_test("TEST 2: -1.5 * 2.0",
             "-1.5e0\n",
             "2.0e0\n",
             -3.0);

    run_test("TEST 3: 1.75 * 1.75",
             "1.75e0\n",
             "1.75e0\n",
             3.0625);

    run_test("TEST 4: 1.25 * 1.25",
             "1.25e0\n",
             "1.25e0\n",
             1.5625);

    run_test("TEST 5: 1e-6 * 1e6",
             "1.0e-6\n",
             "1.0e6\n",
             1.0);

    run_test("TEST 6: -2.0 * -3.0",
             "-2.0e0\n",
             "-3.0e0\n",
             6.0);

    run_test("TEST 7: 0.0 * 123.45",
             "0.0e0\n",
             "1.2345e2\n",
             0.0);

    printf("\n===============================================\n");
    printf("Koniec testow\n");
    return 0;
}

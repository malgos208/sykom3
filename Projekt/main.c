#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

/* Ściezki PROCFS */
#define A1   "/proc/sykom/a1stma"
#define A2   "/proc/sykom/a2stma"
#define CTL  "/proc/sykom/ctstma"
#define STA  "/proc/sykom/ststma"
#define RES  "/proc/sykom/restma"

/* Pomocnicze funkcje I/O                                    */

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

/* Protokół START / ACK                                      */
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
    int timeout = 100; // Zapobiega nieskonczonej pętli
    while (timeout--) {
        if (read_str(STA, buf, sizeof(buf))) return -1;
        if (strstr(buf, "done")) // Uzywamy strstr dla pewności
            return 0;
        usleep(1000); // 1 ms - sprzęt jest szybki
    }
    fprintf(stderr, "Błąd: Timeout podczas oczekiwania na status 'done'\n");
    return -ETIMEDOUT;
}


/* Test z błędem procentowym                                 */
static int run_test(const char *label, const char *a1, const char *a2, double reference) {
    char res_buf[64];
    double measured;
    double err_percent;

    printf("\n[%s]\n", label);
    printf("  a1 = %-15s a2 = %-15s\n", a1, a2);

    // Reset protokołu przed operacją
    if (ack_operation()) return -1;
    usleep(1000); // 1ms na ustabilizowanie stanu IDLE

    // Wpisanie danych
    if (write_str(A1, a1)) return -1;
    if (write_str(A2, a2)) return -1;

    // Bit startu (ctrl_reg[0] = 1).
    if (start_operation()) return -1;
    
    if (wait_done()) {
            fprintf(stderr, "  BŁĄD: Hardware nie zgłosił 'done' dla testu: %s\n", label);
            return -1;
    }
    if (read_str(RES, res_buf, sizeof(res_buf))) return -1;

    // Parsowanie wyniku z modułu
    measured = strtod(res_buf, NULL);

    printf("  Ref: %-15.12g Modul: %-15.12g\n", reference, measured);

    if (fabs(reference) < 1e-300) { // Obsługa okolic zera
        if (fabs(measured) < 1e-300)
            printf("  Status: OK (Oba wyniki zerowe)\n");
        else
            printf("  Status: BLAD (Oczekiwano zera, otrzymano %g)\n", measured);
    } else {
        err_percent = ((measured - reference) / reference) * 100.0;
        printf("  Bład wzgledny: %+0.6f%%\n", err_percent);
        
        // Kryterium zaliczenia testu (np. błąd < 0.001%)
        if (fabs(err_percent) < 0.001)
            printf("  Status: PASS\n");
        else
            printf("  Status: WARNING (Niska precyzja)\n");
    }

    ack_operation();
    return 0;
}

int main(void)
{
    printf("FP64 Multiplier Tester\n");
    printf("===============================================\n");

    run_test("T1: Mnozenie całkowite", "2.0e0", "5.0e0", 10.0);
    run_test("T2: Ułamki (potęgi 2)", "1.75e0", "1.75e0", 3.0625);
    run_test("T3: Znaki mieszane", "-2.5e0", "4.0e0", -10.0);
    run_test("T4: Małe wartości", "1.0e-20", "2.0e-20", 2.0e-40);
    run_test("T5: Duze wartości", "1.2345e10", "1.0e10", 1.2345e20);
    run_test("T6: Mnozenie przez zero", "0.0e0", "123.456e10", 0.0);
    run_test("T7: Precyzja mantysy", "1.111111e0", "1.111111e0", 1.234567654321);
    run_test("T8: Duza rozpiętość", "1.0e400", "2.0e-400", 2.0);

    printf("\n===============================================\n");
    printf("Testy zakoczone.\n");
    return 0;
}
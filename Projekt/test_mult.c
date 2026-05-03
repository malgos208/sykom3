#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

// Ścieżki PROCFS
#define A1   "/proc/sykom/a1stma"
#define A2   "/proc/sykom/a2stma"
#define CTL  "/proc/sykom/ctstma"
#define STA  "/proc/sykom/ststma"
#define RES  "/proc/sykom/restma"

// Próg błędu względnego uznawany za PASS.
// Format ma 36-bitową mantysę → precyzja 2^-36 ≈ 1.5e-11.
// Jądro dodatkowo traci 1-2 bity na operacjach dzielenia w parse_fp/format_fp,
// dlatego dopuszczamy margines 1e-7% (czyli ~7 razy powyżej precyzji formatu).
#define ERR_PASS_PCT  1e-7

static int pass_cnt = 0, fail_cnt = 0;

// ─── I/O POMOCNICZE ─────────────────────────────────────────────────────────

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
    if (!fgets(buf, maxlen, f)) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

// Odczyt pliku z oczekiwaniem na EAGAIN – przydatne do "restma" przed gotowością.
// Zwraca 0 jeśli udało się odczytać, 1 jeśli EAGAIN, -1 jeśli inny błąd.
static int read_str_noblock(const char *path, char *buf, size_t maxlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == EAGAIN) return 1;
        perror(path); return -1;
    }
    if (!fgets(buf, maxlen, f)) {
        int e = errno;
        fclose(f);
        if (e == EAGAIN) return 1;
        return -1;
    }
    fclose(f);
    return 0;
}

// Próba zapisu – zwraca 0 przy sukcesie, -1 przy błędzie (errno ustawione).
static int try_write(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int r = fputs(s, f) < 0 ? -1 : 0;
    fclose(f);
    return r;
}

static int wait_done(void)
{
    char buf[32];
    int timeout = 10000;  // 10000 × 100 µs = 1 s
    while (timeout--) {
        if (read_str(STA, buf, sizeof(buf))) return -EIO;
        if (strstr(buf, "done")) return 0;
        usleep(100);
    }
    return -ETIMEDOUT;
}

// Zwolnienie modułu (ctrl=0) i sprawdzenie powrotu do idle.
static int release_and_check_idle(void)
{
    char buf[32];
    if (write_str(CTL, "0\n")) return -1;
    usleep(5000);  // 5 ms – dwa cykle zegara 1 kHz
    if (read_str(STA, buf, sizeof(buf))) return -1;
    return strstr(buf, "idle") ? 0 : -1;
}

// ─── TESTY POPRAWNYCH DANYCH ─────────────────────────────────────────────────

// Wykonuje mnożenie a1 × a2, porównuje z wartością referencyjną `ref`.
// `ref` jest podawany jawnie (nie wyliczany przez strtod) dla pełnej kontroli.
static void run_test(const char *label, const char *a1, const char *a2, double ref)
{
    char res_buf[64];
    printf("\n[%s]\n", label);
    printf("  a1=%-16s  a2=%-16s  ref=%.10g\n", a1, a2, ref);

    // Sprawdź że moduł jest w IDLE przed startem
    char sta[32];
    if (read_str(STA, sta, sizeof(sta))) {
        printf("  FAIL: nie mozna odczytac statusu\n");
        fail_cnt++; return;
    }
    if (!strstr(sta, "idle")) {
        printf("  FAIL: modul nie jest w stanie idle przed testem (status='%s')\n", sta);
        fail_cnt++; return;
    }

    if (write_str(A1, a1)) { fail_cnt++; return; }
    if (write_str(A2, a2)) { fail_cnt++; return; }
    if (write_str(CTL, "1\n")) { fail_cnt++; return; }

    int ret = wait_done();
    if (ret == -ETIMEDOUT) {
        printf("  FAIL: timeout oczekiwania na done\n");
        fail_cnt++; return;
    } else if (ret) {
        printf("  FAIL: blad odczytu statusu\n");
        fail_cnt++; return;
    }

    if (read_str(RES, res_buf, sizeof(res_buf))) {
        printf("  FAIL: blad odczytu wyniku\n");
        fail_cnt++; return;
    }

    if (release_and_check_idle()) {
        printf("  WARNING: modul nie wrocil do idle po zwolnieniu ctrl\n");
    }

    double calc = strtod(res_buf, NULL);
    printf("  wynik: %s", res_buf);

    if (fabs(ref) < 1e-300) {
        // Oczekujemy zera
        if (fabs(calc) < 1e-300) {
            printf("  PASS (zero)\n");
            pass_cnt++;
        } else {
            printf("  FAIL (oczekiwano 0, otrzymano %g)\n", calc);
            fail_cnt++;
        }
    } else {
        double rel_err_pct = fabs((calc - ref) / ref) * 100.0;
        printf("  blad wzgledny: %+.4e%%  ", rel_err_pct);
        if (rel_err_pct <= ERR_PASS_PCT) {
            printf("PASS\n");
            pass_cnt++;
        } else {
            printf("FAIL\n");
            fail_cnt++;
        }
    }
}

// ─── TESTY BŁĘDNYCH DANYCH ───────────────────────────────────────────────────

// Sprawdza że zapis niepoprawnej wartości do pliku wejściowego jest odrzucany.
// Oczekujemy: fopen zwraca NULL z errno=EINVAL lub fputs < 0.
static void run_invalid_test(const char *label, const char *path, const char *val)
{
    printf("\n[%s] zapis '%s' do %s\n", label, val, path);
    errno = 0;
    int ret = try_write(path, val);

    // Moduł jądra powinien zwrócić -EINVAL → fopen/fputs sygnalizuje błąd
    if (ret == -1) {
        printf("  PASS (zapis odrzucony, errno=%d)\n", errno);
        pass_cnt++;
    } else {
        // Sprawdź czy nie zepsuto stanu (status musi być idle)
        char sta[32];
        read_str(STA, sta, sizeof(sta));
        printf("  FAIL: zapis zaakceptowany (status=%s)\n", sta);
        // Posprzątaj na wypadek gdyby ctrl dostał śmieć
        write_str(CTL, "0\n");
        usleep(5000);
        fail_cnt++;
    }
}

// Sprawdza że odczyt RES przed uruchomieniem zwraca błąd (moduł nie jest done).
static void run_early_read_test(void)
{
    printf("\n[T_ERR_EARLY] odczyt restma przed uruchomieniem obliczen\n");
    // Upewnij się że jesteśmy w IDLE
    write_str(CTL, "0\n"); usleep(5000);

    char buf[64];
    int r = read_str_noblock(RES, buf, sizeof(buf));
    // Oczekujemy EAGAIN (status != done) lub błąd odczytu
    if (r == 1) {
        printf("  PASS (EAGAIN – wynik niedostepny)\n");
        pass_cnt++;
    } else if (r == 0) {
        // Jeśli dostaliśmy wartość – sprawdź czy status był done
        char sta[32]; read_str(STA, sta, sizeof(sta));
        printf("  FAIL: odczyt zwrocil '%s' przy statusie '%s'\n", buf, sta);
        fail_cnt++;
    } else {
        printf("  FAIL: nieoczekiwany blad odczytu restma\n");
        fail_cnt++;
    }
}

// ─── MAIN ────────────────────────────────────────────────────────────────────

int main(void)
{
    printf("=== FP64 Multiplier – testy systemowe ===\n");

    // ── Testy poprawnych danych ──────────────────────────────────────────
    // Wartości referencyjne obliczone niezależnie (nie przez strtod * strtod)
    // aby uniknąć zakrycia błędów dokładnością IEEE 754 double.

    // Podstawowe przypadki
    run_test("T01: 2.0 * 5.0 = 10.0",
             "2.0e0", "5.0e0", 10.0);

    run_test("T02: 1.75 * 1.75 = 3.0625  (potegi 2 – bezbladne)",
             "1.75e0", "1.75e0", 3.0625);

    run_test("T03: 2.5 * 4.0 = 10.0  (test z systemu)",
             "2.5e0", "4.0e0", 10.0);

    // Znaki
    run_test("T04: -2.5 * 4.0 = -10.0  (plus * minus)",
             "-2.5e0", "4.0e0", -10.0);

    run_test("T05: -2.5 * -4.0 = 10.0  (minus * minus)",
             "-2.5e0", "-4.0e0", 10.0);

    // Zero
    run_test("T06: 0 * 123.456e10 = 0  (arg1=zero)",
             "0.0e0", "123.456e10", 0.0);

    run_test("T07: 123.456e10 * 0 = 0  (arg2=zero)",
             "123.456e10", "0.0e0", 0.0);

    // Round-trip przez format
    run_test("T08: 3.14159265 * 1.0 = 3.14159265  (mnozenie przez 1)",
             "3.14159265e0", "1.0e0", 3.14159265);

    run_test("T09: 2.71828182 * -1.0 = -2.71828182  (mnozenie przez -1)",
             "2.71828182e0", "-1.0e0", -2.71828182);

    // Małe i duże wykładniki
    run_test("T10: 1.0e-20 * 2.0e-20 = 2.0e-40  (male wykladniki)",
             "1.0e-20", "2.0e-20", 2.0e-40);

    run_test("T11: 1.2345e10 * 1.0e10 = 1.2345e20  (duze wykladniki)",
             "1.2345e10", "1.0e10", 1.2345e20);

    run_test("T12: 1.0e100 * 1.0e100 = 1.0e200  (ekstremalne duze)",
             "1.0e100", "1.0e100", 1.0e200);

    run_test("T13: 1.0e-100 * 1.0e-100 = 1.0e-200  (ekstremalne male)",
             "1.0e-100", "1.0e-100", 1.0e-200);

    // Precyzja mantysy – wartości bezbłędne w formacie 36-bitowym
    // 1.5 = 1.1b, 1.25 = 1.01b – mnożenie jest dokładne
    run_test("T14: 1.5 * 1.5 = 2.25  (dokladne w formacie binarnym)",
             "1.5e0", "1.5e0", 2.25);

    run_test("T15: 1.25 * 1.25 = 1.5625  (dokladne w formacie binarnym)",
             "1.25e0", "1.25e0", 1.5625);

    // Przemienność (wynik musi być bit-identyczny)
    {
        printf("\n[T16: przemiennosc  3.0 * 1.5  vs  1.5 * 3.0]\n");
        char r1[64], r2[64];
        int ok = 1;

        write_str(A1, "3.0e0"); write_str(A2, "1.5e0");
        write_str(CTL, "1\n"); wait_done();
        read_str(RES, r1, sizeof(r1));
        release_and_check_idle();

        write_str(A1, "1.5e0"); write_str(A2, "3.0e0");
        write_str(CTL, "1\n"); wait_done();
        read_str(RES, r2, sizeof(r2));
        release_and_check_idle();

        // Porównuj łańcuchy (bit-identical output)
        if (strcmp(r1, r2) == 0) {
            printf("  PASS: '%s' == '%s'\n", r1, r2);
            pass_cnt++;
        } else {
            printf("  FAIL: 3.0*1.5='%s'  1.5*3.0='%s'\n", r1, r2);
            fail_cnt++;
        }
    }

    // Dwa kolejne obliczenia – sprawdzenie że FSM wraca do IDLE
    {
        printf("\n[T17: dwa kolejne obliczenia bez twardego resetu]\n");
        char r1[64];
        run_test("  T17a: 2.0 * 3.0 = 6.0", "2.0e0", "3.0e0", 6.0);
        run_test("  T17b: 4.0 * 5.0 = 20.0", "4.0e0", "5.0e0", 20.0);
    }

    // ── Testy błędnych danych wejściowych ────────────────────────────────
    printf("\n--- Testy blednych danych ---\n");

    // Wyczyść stan
    write_str(CTL, "0\n"); usleep(5000);

    run_early_read_test();

    run_invalid_test("T_ERR1: tekst zamiast liczby",
                     A1, "abc\n");

    run_invalid_test("T_ERR2: pusty string",
                     A1, "\n");

    run_invalid_test("T_ERR3: przepelniony wykladnik",
                     A1, "1.0e999\n");

    run_invalid_test("T_ERR4: bledny znak",
                     A1, "++1.0e0\n");

    run_invalid_test("T_ERR5: dwa znaki dziesiętne",
                     A1, "1.2.3\n");

    run_invalid_test("T_ERR6: bledna wartosc ctrl (2)",
                     CTL, "2\n");

    run_invalid_test("T_ERR7: bledna wartosc ctrl (tekst)",
                     CTL, "start\n");

    // ── Podsumowanie ─────────────────────────────────────────────────────
    printf("\n===========================================\n");
    printf("WYNIKI: %d PASS / %d FAIL  (razem %d)\n",
           pass_cnt, fail_cnt, pass_cnt + fail_cnt);
    if (fail_cnt == 0)
        printf("WSZYSTKIE TESTY ZALICZONE\n");
    printf("===========================================\n");
    return fail_cnt ? 1 : 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

// Ściezki PROCFS
#define A1   "/proc/sykom/a1stma"
#define A2   "/proc/sykom/a2stma"
#define CTL  "/proc/sykom/ctstma"
#define STA  "/proc/sykom/ststma"
#define RES  "/proc/sykom/restma"

// Pomocnicze funkcje I/O
static int write_str(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fputs(s, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

static int read_str(const char *path, char *buf, size_t maxlen)
{
    FILE *f = fopen(path, "r"); //  otwarcie pliku w trybie do odczytu
    if (!f) {
        perror(path); // wypisanie ostatniego błędu systemowego + wypisanie nazwy pliku
        return -1;
    }
    if (!fgets(buf, maxlen, f)) { // wczytanie jednej linii z pliku do bufora buf
        fclose(f); // zamknięcie pliku
        return -1;
    }
    fclose(f);
    return 0;
}

static int wait_done(void)
{
    char buf[32];
    int timeout = 10000;  // maksymalna liczba prób (maksymalny czas oczekiwania: 10000 * 100us = 1s)
    while (timeout--) {
        if (read_str(STA, buf, sizeof(buf))) return -EIO; // błąd odczytu statusu
        if (strstr(buf, "done")) return 0; // sukces - status 'done' osiągnięty
        
        usleep(100);  // 100 us między próbami
    }
    return -ETIMEDOUT; // przekroczono limit czasu
}

// Test z błędem procentowym
static int run_test(const char *label, const char *a1, const char *a2)
{
    char res_buf[64];
    double reference = strtod(a1, NULL) * strtod(a2, NULL);

    // Tytuł testu i wypisanie argumentów
    printf("\n[%s]\n", label);
    printf("a1 = %-s, a2 = %-s\n", a1, a2);

    // 1. Zapis argumentów do modułu
    if (write_str(A1, a1)) return -1;
    if (write_str(A2, a2)) return -1;

    // 2. Sygnał startu obliczeń (ctrl_reg[0] = 1)
    if (write_str(CTL, "1")) return -1;

    // 3. Oczekiwanie na status 'done'
    int ret = wait_done();

    if (ret == -ETIMEDOUT) {
        fprintf(stderr, "  BLAD: Timeout podczas oczekiwania na 'done' w test %s\n", label);
        return -1;
    } else if (ret == -EIO) {
        fprintf(stderr, "  BLAD: Nie można odczytać statusu w test %s\n", label);
        return -1;
    }

    // 4. Odczyt wyniku i zapisanie do bufora res_buf
    if (read_str(RES, res_buf, sizeof(res_buf))) return -1;

    // 5. Sygnał powrotu do IDLE (ctrl_reg[0] = 0)
    if (write_str(CTL, "0")) return -1;

    // 6. Weryfikacja wyniku (porówanie wynikowego stringu z oczekiwanym)
    double calculated = strtod(res_buf, NULL);

    printf("\tSurowy wynik: %s", res_buf); // wzięty bezpośrednio z pliku wynikowego
    printf("\tReferencyjna wartość: %-15.10g\n", reference);
    printf("\tObliczona wartość: %-15.10g\n", calculated);
    // % specyfikacja formatu:
    // -: wyrównanie do lewej
    // 15: minimalna szerokość wypisywanego string (15 znaków), gdy wynik krótszy wypełniane spacjami z prawej strony
    // .10: dokładność (10 cyfr ZNACZĄCYCH - przed i po przecinku)
    // g: format ogólny (automatycznie wybiera między notacją zwykłą a naukową)

    // fabs - moduł z liczby double

    // gdy wartość referencyjna jest bliska zeru (lub równa zero), błąd względny może być bardzo duży lub nieskończony, dlatego w takich przypadkach lepiej oceniać bezwzględną różnicę między wartościami zamiast błędu względnego
    if (fabs(reference) < 1e-300) {
        if (fabs(calculated) < 1e-300)
            printf("\tStatus: PASS (liczba bardzo bliska zeru)\n");
        else
            printf("\tStatus: FAIL (oczekiwano 0, otrzymano %g)\n", calculated);
    } else {
        double relative_err = (calculated - reference) / reference * 100.0;
        printf("\tBłąd względny : %+0.6f%%\n", relative_err);
        if (fabs(relative_err) < 0.001)
            printf("\tStatus: PASS\n");
        else if (fabs(relative_err) < 0.01)
            printf("\tStatus: WARNING (niska precyzja <0.01%%)\n");
        else
            printf("\tStatus: FAIL\n");
    }
    return 0;
}

// Test sprawdzający odrzucanie niepoprawnych danych
static int run_invalid_test(const char *label, const char *path, const char *val)
{
    printf("\n[%s] zapis '%s' do %s\n", label, val, path);

    write_str(path, val); // próba zapisu błędnych danych

    // Sprawdź, czy moduł nadal działa (1.0 * 1.0 = 1.0)
    write_str(A1, "1.0e0");
    write_str(A2, "1.0e0");
    write_str(CTL, "1");
    wait_done();

    char res[64];
    read_str(RES, res, sizeof(res));
    write_str(CTL, "0");

    printf("\tStatus: %s", strstr(res, "1.000000000e0") ? "PASS" : "FAIL"s);
    return 0;
}

// Test sprawdzający próbę odczytu wyniku przed wykonaniem obliczeń
static int run_early_read_test(void)
{
    printf("\n[T_ERR_EARLY: Odczyt wyniku przed obliczeniami]\n");

    // Upewnij się, że moduł jest w IDLE
    write_str(CTL, "0");
    usleep(5000);

    errno = 0;
    FILE *f = fopen(RES, "r");
    if (!f) {
        if (errno == EAGAIN) {
            printf("\tStatus: PASS (EAGAIN – wynik niedostępny)\n");
        } else {
            printf("\tStatus: PASS (odczyt zablokowany, errno=%d)\n", errno);
        }
        return 0;
    }

    char buf[64];
    if (fgets(buf, sizeof(buf), f)) {
        // Jeśli coś odczytaliśmy, sprawdźmy status
        char sta[32];
        read_str(STA, sta, sizeof(sta));
        printf("\tStatus: FAIL (odczytano '%s' przy statusie '%s')\n", buf, sta);
    } else {
        printf("\tStatus: PASS (odczyt zablokowany przez fgets)\n");
    }
    fclose(f);
    return 0;
}

int main(void)
{
    printf("Tester mnożenia FP64\n");
    write_str(CTL, "0"); // reset modułu przed testami
    usleep(5000);

    // === Podstawowe mnożenie ===
    run_test("T01: 2.0 * 5.0 = 10.0", "2.0e0", "5.0e0");
    run_test("T02: 1.75 * 1.75 = 3.0625", "1.75e0", "1.75e0");
    run_test("T03: 1.23456789 * 9.87654321", "1.23456789e0", "9.87654321e0");

    // === Znaki ===
    run_test("T04: (+) * (-) = (-)", "-2.5e0", "4.0e0");
    run_test("T05: (-) * (-) = (+)", "-2.5e0", "-4.0e0");
    run_test("T06: (-) * (+) = (-)", "2.5e0", "-4.0e0");

    // === Zero ===
    run_test("T07: 0 * liczba = 0", "0.0e0", "123.456e10");
    run_test("T08: liczba * 0 = 0", "123.456e10", "0.0e0");
    run_test("T09: 0 * 0 = 0", "0.0e0", "0.0e0");
    run_test("T10: -0 * 5.0 = 0", "-0.0e0", "5.0e0");
    run_test("T11: 5.0 * -0 = 0", "5.0e0", "-0.0e0");

    // === Mnożenie przez 1 i -1 (round-trip) ===
    run_test("T12: liczba * 1 = liczba", "3.14159265e0", "1.0e0");
    run_test("T13: liczba * -1 = -liczba", "2.71828182e0", "-1.0e0");

    // === Małe liczby ===
    run_test("T14: 1e-20 * 2e-20 = 2e-40", "1.0e-20", "2.0e-20");
    run_test("T15: 1e-100 * 1e-100 = 1e-200", "1.0e-100", "1.0e-100");
    run_test("T16: 1e-150 * 1e-150 = 1e-300", "1.0e-150", "1.0e-150");
    run_test("T17: 1.5e-50 * 2e-50 = 3e-100", "1.5e-50", "2.0e-50");

    // === Duże liczby ===
    run_test("T18: 1.2345e10 * 1e10 = 1.2345e20", "1.2345e10", "1.0e10");
    run_test("T19: 1e100 * 1e100 = 1e200", "1.0e100", "1.0e100");
    run_test("T20: 1e150 * 1e150 = 1e300", "1.0e150", "1.0e150");
    run_test("T21: 1.5e50 * 2e50 = 3e100", "1.5e50", "2.0e50");

    // === Mieszane zakresy ===
    run_test("T22: 1e100 * 1e-100 = 1", "1.0e100", "1.0e-100");
    run_test("T23: 1e150 * 1e-50 = 1e100", "1.0e150", "1.0e-50");

    // === Precyzja mantysy ===
    run_test("T24: 1.111111 * 1.111111", "1.111111e0", "1.111111e0");
    run_test("T25: zaokrąglanie w górę", "1.111111118e0", "1.0e0");
    run_test("T26: zaokrąglanie w dół", "1.111111112e0", "1.0e0");
    run_test("T27: duża mantysa * 1", "1.999999999e0", "1.0e0");

    // === Duże/małe z mantysą ===
    run_test("T28: 1.999999e100 * 1", "1.999999e100", "1.0e0");
    run_test("T29: 1.999999e-100 * 1", "1.999999e-100", "1.0e0");
    run_test("T30: 1.23456789e50 * 1", "1.23456789e50", "1.0e0");

    // === Potęgi dwójki (dokładne w binarnym) ===
    run_test("T31: 1024 * 1024 = 1048576", "1.024e3", "1.024e3");

    // === Symetria znaków ===
    run_test("T32: (-1) * (-1) = 1", "-1.0e0", "-1.0e0");
    run_test("T33: (-1) * 1 = -1", "-1.0e0", "1.0e0");

    // === Bardzo mała * zero ===
    run_test("T34: -1e-300 * 0 = 0", "-1.0e-300", "0.0e0");

    // === Testy błędnych danych ===
    printf("\nTesty błędnych danych\n");

    write_str(CTL, "0");
    usleep(5000);

    run_early_read_test();

    run_invalid_test("T_ERR1: tekst zamiast liczby", A1, "abc");
    run_invalid_test("T_ERR2: pusty string", A1, "");
    run_invalid_test("T_ERR3: przepełniony wykładnik", A1, "1.0e999");
    run_invalid_test("T_ERR4: podwójny znak", A1, "++1.0e0");
    run_invalid_test("T_ERR5: dwie kropki", A1, "1.2.3");
    run_invalid_test("T_ERR6: ctrl = 2", CTL, "2");
    run_invalid_test("T_ERR7: ctrl = tekst", CTL, "start");

    write_str(CTL, "0");

    printf("\nTesty zakończone.\n");
    return 0;
}
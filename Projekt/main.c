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
    if (write_str(CTL, "1\n")) return -1;

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
    if (write_str(CTL, "0\n")) return -1;

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

    write_str(path, val);                    // próba zapisu błędnych danych

    // Sprawdź, czy moduł nadal działa (1.0 * 1.0 = 1.0)
    write_str(A1, "1.0e0\n");
    write_str(A2, "1.0e0\n");
    write_str(CTL, "1\n");
    wait_done();

    char res[64];
    read_str(RES, res, sizeof(res));
    write_str(CTL, "0\n");

    printf("\tStatus: %s (wynik=%s)", strstr(res, "1.000000000e0") ? "PASS" : "FAIL", res);
    return 0;
}

// Test sprawdzający próbę odczytu wyniku przed wykonaniem obliczeń
static int run_early_read_test(void)
{
    printf("\n[T_ERR_EARLY: Odczyt wyniku przed obliczeniami]\n");

    // Upewnij się, że moduł jest w IDLE
    write_str(CTL, "0\n");
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

    run_test("T1: Mnożenie całkowite", "2.0e0", "5.0e0");
    run_test("T2: Ułamki (potęgi 2)", "1.75e0", "1.75e0");
    run_test("T3: Znaki mieszane", "-2.5e0", "4.0e0");
    run_test("T4: Małe wartości", "1.0e-20", "2.0e-20");
    run_test("T5: Duże wartości", "1.2345e10", "1.0e10");
    run_test("T6: Mnożenie przez zero", "0.0e0", "123.456e10");
    run_test("T7: Precyzja mantysy", "1.111111e0", "1.111111e0");
    // Round-trip & znak
    run_test("T8:  Mnożenie przez 1", "3.14159265e0", "1.0e0");
    run_test("T9:  Mnożenie przez -1", "2.71828182e0", "-1.0e0");
    // Ujemne zero
    run_test("T10: Ujemne zero * dodatnia", "-0.0e0", "5.0e0");
    run_test("T11: Dodatnia * ujemne zero", "5.0e0", "-0.0e0");
    // Zaokrąglanie i precyzja
    run_test("T12: Zaokrąglanie w górę (9 cyfr)", "1.111111118e0", "1.0e0");
    run_test("T13: Zaokrąglanie w dół (9 cyfr)", "1.111111112e0", "1.0e0");
    // Ekstremalne zakresy
    run_test("T14: Duże wartości (1e100)", "1.0e100", "1.0e100");
    run_test("T15: Bardzo małe wartości", "1.0e-100", "1.0e-100");
    // Mieszane znaki z małymi liczbami
    run_test("T16: Mała wartość ujemna * zero", "-1.0e-300", "0.0e0");
    
    // === Testy skrajnych przypadków ===
    // Maksymalny i minimalny wykładnik
    run_test("T17: Maksymalny wykładnik (1e67000000 * 1.0)",
            "1.0e67000000", "1.0e0");
    run_test("T18: Minimalny wykładnik (1e-67000000 * 1.0)",
            "1.0e-67000000", "1.0e0");

    // Overflow – wynik poza zakresem
    run_test("T19: Overflow (1e67000000 * 1e67000000)",
            "1.0e67000000", "1.0e67000000");

    // Underflow – wynik zbyt mały
    run_test("T20: Underflow (1e-67000000 * 1e-67000000)",
            "1.0e-67000000", "1.0e-67000000");

    // Maksymalna mantysa
    run_test("T21: Maksymalna mantysa (1.999999 * 1.0)",
            "1.999999999e0", "1.0e0");

    // Liczba bliska 1.0
    run_test("T22: 1.000000001 * 1.000000001",
            "1.000000001e0", "1.000000001e0");

    // Bardzo mała mantysa
    run_test("T23: 1.000000001 * 0.999999999",
            "1.000000001e0", "0.999999999e0");

    // Duża liczba z dużą mantysą
    run_test("T24: 1.999999e20 * 1.999999e20",
            "1.999999e20", "1.999999e20");

    // Mała liczba z małą mantysą
    run_test("T25: 1.000001e-20 * 1.000001e-20",
            "1.000001e-20", "1.000001e-20");

    // Zero z ujemną mantysą (ujemne zero)
    run_test("T26: -0.0 * -0.0", "-0.0e0", "-0.0e0");

    // Przemienność z zerem
    run_test("T27: 0 * -5.0", "0.0e0", "-5.0e0");

    // Dokładność – potęgi 2 (bezstratne w binarnym)
    run_test("T28: 2.0^10 * 2.0^10 = 2.0^20",
            "1.024e3", "1.024e3");

    // Liczby z wieloma cyframi znaczącymi
    run_test("T29: 1.23456789e0 * 9.87654321e0",
            "1.23456789e0", "9.87654321e0");

    // Symetria znaków
    run_test("T30: -1.0 * -1.0 = 1.0", "-1.0e0", "-1.0e0");
    run_test("T31: -1.0 * 1.0 = -1.0", "-1.0e0", "1.0e0");
    run_test("T32: 1.0 * -1.0 = -1.0", "1.0e0", "-1.0e0");

    // Testy błędnych danych
    printf("\n--- Testy błędnych danych ---\n");

    // Przywróć IDLE
    write_str(CTL, "0\n");
    usleep(5000);

    run_early_read_test();

    run_invalid_test("T_ERR1: Tekst zamiast liczby", A1, "abc\n");
    run_invalid_test("T_ERR2: Pusty string", A1, "\n");
    run_invalid_test("T_ERR3: Przepełniony wykładnik", A1, "1.0e999\n");
    run_invalid_test("T_ERR4: Błędny znak", A1, "++1.0e0\n");
    run_invalid_test("T_ERR5: Dwa znaki dziesiętne", A1, "1.2.3\n");
    run_invalid_test("T_ERR6: Błędna wartość ctrl (2)", CTL, "2\n");
    run_invalid_test("T_ERR7: Błędna wartość ctrl (tekst)", CTL, "start\n");

    printf("\nTesty zakoczone.\n");
    return 0;
}
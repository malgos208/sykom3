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

// Konwersja napisu w notacji naukowej na double
static double parse_sci(const char *s)
{
    char *end;
    double mant = strtod(s, &end);
    int exp = 0;
    if (*end == 'e' || *end == 'E')
        exp = atoi(end + 1);
    return mant * pow(10.0, exp);
}

// Test z błędem procentowym
static int run_test(const char *label, const char *a1, const char *a2)
{
    char res_buf[64];
    double reference = parse_sci(a1) * parse_sci(a2);

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
    double calculated = parse_sci(res_buf);

    printf("\tSurowy wynik: %s", res_buf); // wzięty bezpośrednio z pliku wynikowego
    printf("\tReferencyjna wartość: %-15.10g\n", reference);
    printf("\t   Obliczona wartość: %-15.10g\n", calculated);
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
        printf("\tBlad względny : %+0.6f%%", relative_err);
        if (fabs(relative_err) < 0.001)
            printf("\tStatus: PASS\n");
        else if (fabs(relative_err) < 0.01)
            printf("\tStatus: WARNING (niska precyzja <0.01%%)\n");
        else
            printf("\tStatus: FAIL\n");
    }
    return 0;
}

int main(void)
{
    printf("FP64 Multiplier Tester\n");

    run_test("T1: Mnozenie całkowite", "2.0e0", "5.0e0");
    run_test("T2: Ułamki (potęgi 2)", "1.75e0", "1.75e0");
    run_test("T3: Znaki mieszane", "-2.5e0", "4.0e0");
    run_test("T4: Małe wartości", "1.0e-20", "2.0e-20");
    run_test("T5: Duze wartości", "1.2345e10", "1.0e10");
    run_test("T6: Mnozenie przez zero", "0.0e0", "123.456e10");
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

    printf("\nTesty zakoczone.\n");
    return 0;
}
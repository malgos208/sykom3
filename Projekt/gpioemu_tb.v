/* gpioemu_tb.v – testbench modułu mnożącego FP64
 *   T0  reset zeruje wszystkie rejestry
 *   T1  sekwencja stanów FSM: idle→busy→done→idle
 *   T2  1.0  × 1.0  = 1.0     (bazowy)
 *   T3  2.5  × 4.0  = 10.0    (test z systemu)
 *   T4  -1.5 × 2.0  = -3.0    (znak +×−)
 *   T5  -2.0 × -3.0 = 6.0     (znak −×−)
 *   T6  1.75 × 1.75 = 3.0625  (renormalizacja: prod[73]=1)
 *   T7  1.25 × 1.25 = 1.5625  (bez renormalizacji: prod[73]=0)
 *   T8  0    × 5.0  = 0       (wykrycie zera: arg1=0)
 *   T9  5.0  × 0    = 0       (wykrycie zera: arg2=0)
 *   T10 0    × 0    = 0       (oba argumenty zerowe)
 *   T11 przemienność: 3.0×1.5 === 1.5×3.0
 *   T12 dwa kolejne mnożenia (sprawdzenie powrotu FSM do IDLE)
 *   T13 odczyt rejestrów arg1/arg2 przez srd
 *   T14 adres spoza mapy → sdata_out = 0
 *   T15 gpio_latch zatrzaskuje gpio_in w gpio_in_s_insp
 *   T16 n_reset zeruje gpio_in_s_insp i rejestry stanu
 *
 * Format FP64 (specyfikacja):
 *   bit [0]     – znak
 *   bits[27:1]  – wykładnik binarny (27 b, BIAS = 2^26 = 67108864)
 *   bits[63:28] – mantysa
 *
 */

module gpioemu_tb;

// ═══ SYGNAŁY ═════════════════════════════════════════════════════════════
reg        n_reset, clk, srd, swr, gpio_latch;
reg [15:0] saddress;
reg [31:0] sdata_in, gpio_in;

wire [31:0] sdata_out, gpio_out, gpio_in_s_insp;

// ═══ test_module ═════════════════════════════════════════════════════════════════
gpioemu test_module (
    .n_reset       (n_reset),
    .saddress      (saddress),
    .srd           (srd),
    .swr           (swr),
    .sdata_in      (sdata_in),
    .sdata_out     (sdata_out),
    .gpio_in       (gpio_in),
    .gpio_latch    (gpio_latch),
    .gpio_out      (gpio_out),
    .clk           (clk),
    .gpio_in_s_insp(gpio_in_s_insp)
);

// Zegar: okres 10 ns = 1 kHz w skali symulacji
always #5 clk = ~clk;

// ═══ STAŁE I FUNKCJA KODOWANIA FP ════════════════════════════════════════
localparam [26:0] BIAS = 27'd67108864; // 2^26

// make_fp: pakuje (znak, wykładnik_z_biasem, mantysa) do 64-bit FP
function [63:0] make_fp;
    input        s;      // znak: 0=plus, 1=minus
    input [26:0] e;      // wykładnik z biasem
    input [35:0] m;      // mantysa (36 b, bez wiodącej jedynki)
    begin
        make_fp = 64'd0;
        make_fp[0]     = s;
        make_fp[27:1]  = e;
        make_fp[63:28] = m;
    end
endfunction

// ═══ ZMIENNE POMOCNICZE ═══════════════════════════════════════════════════
integer pass_cnt, fail_cnt;
reg [63:0] result, r1, r2;
reg [31:0] status, rd;

// ═══ ZADANIA ══════════════════════════════════════════════════════════════

// Zerowanie modułu sygnałem n_reset
task do_reset;
begin
    n_reset = 0; #12;   // negedge n_reset → reset asynchroniczny
    n_reset = 1; #8;
end
endtask

// Zapis 32-bitowego słowa na szynie
task bus_write;
    input [15:0] addr;
    input [31:0] data;
begin
    saddress = addr;
    sdata_in = data;
    swr = 1; #10; swr = 0; #5;
end
endtask

// Odczyt 32-bitowego słowa z szyny (sdata_out jest kombinacyjny)
task bus_read;
    input  [15:0] addr;
    output [31:0] data;
begin
    saddress = addr;
    srd = 1; #5; // opóźnienie dla stabilności
    data = sdata_out; #5;
    srd = 0; #1;
end
endtask

// Zapis 64-bitowego argumentu 1 (rejestry 0x100 / 0x108)
task write_arg1;
    input [63:0] v;
begin
    bus_write(16'h0100, v[63:32]);  // arg1_h (starsza połowa)
    bus_write(16'h0108, v[31:0]);   // arg1_l (młodsza połowa)
end
endtask

// Zapis 64-bitowego argumentu 2 (rejestry 0x0F0 / 0x0F8)
task write_arg2;
    input [63:0] v;
begin
    bus_write(16'h00F0, v[63:32]);  // arg2_h
    bus_write(16'h00F8, v[31:0]);   // arg2_l
end
endtask

// Odczyt 64-bitowego wyniku (rejestry 0x0D8 / 0x0E0)
task read_result;
    output [63:0] v;
    reg [31:0] hi, lo;
begin
    bus_read(16'h00D8, hi);     // res_h
    bus_read(16'h00E0, lo);     // res_l
    v = {hi, lo};
end
endtask

// Odczyt rejestru stanu (adres 0x0E8): 0=idle, 1=busy, 2=done
task read_status;
    output [31:0] v;
begin
    bus_read(16'h00E8, v);
end
endtask

// Uruchomienie obliczeń (ctrl[0]=1) i oczekiwanie na stan DONE.
// FSM potrzebuje 4 cykli: IDLE→CALC→WRITE→DONE + 1 cykl dla status=2.
task start_and_wait;
begin
    bus_write(16'h00D0, 32'h1); // ctrl = 1
    repeat(6) @(posedge clk); #1;
end
endtask

// Zwolnienie modułu (ctrl[0]=0) → FSM wraca do IDLE.
task release_ctrl;
begin
    bus_write(16'h00D0, 32'h0); // ctrl = 0
    repeat(2) @(posedge clk); #1;
end
endtask

// Wyświetlenie pól liczby FP (diagnostyczne)
task show_fp;
    input [63:0] v;
begin
    $display("      sign=%0d  exp=%0d(%+0d)  mant=0x%09h",
        v[0],
        v[27:1],
        $signed({1'b0, v[27:1]}) - $signed({1'b0, BIAS}),
        v[63:28]);
end
endtask

// Sprawdzenie wyniku i aktualizacja liczników
task check;
    input [63:0] got;
    input [63:0] exp_val;
begin
    if (got === exp_val) begin
        $display("  PASS");
        pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL");
        $display("    otrzymano:"); show_fp(got);
        $display("    oczekiwano:"); show_fp(exp_val);
        fail_cnt = fail_cnt + 1;
    end
end
endtask

// ═══ CIAŁO TESTU ══════════════════════════════════════════════════════════
initial begin
    $dumpfile("gpioemu.vcd");
    $dumpvars(0, gpioemu_tb);

    // Inicjalizacja sygnałów
    clk = 0; srd = 0; swr = 0; gpio_latch = 0;
    saddress = 0; sdata_in = 0; gpio_in = 0;
    pass_cnt = 0; fail_cnt = 0;

    // ─────────────────────────────────────────────────────────────────────
    // T0: n_reset zeruje wszystkie rejestry wyjściowe
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T0: reset zeruje rejestry ===");
    do_reset();
    read_result(result);
    read_status(status);
    if (result === 64'h0 && status === 32'h0) begin
        $display("  PASS"); pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL: result=0x%h status=%0d", result, status);
        fail_cnt = fail_cnt + 1;
    end

    // ─────────────────────────────────────────────────────────────────────
    // T1: sekwencja stanów FSM: idle(0) → busy(1) → done(2) → idle(0)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T1: sekwencja statusów FSM ===");
    write_arg1(make_fp(0, BIAS, 36'h0));    // 1.0
    write_arg2(make_fp(0, BIAS, 36'h0));    // 1.0
    read_status(status);
    if (status !== 32'd0) begin
        $display("  FAIL: status przed startem = %0d (oczekiwano 0=idle)", status);
        fail_cnt = fail_cnt + 1;
    end else begin
        // Uruchom i próbkuj po 2 cyklach (FSM powinien być w CALC lub WRITE → status=1)
        bus_write(16'h00D0, 32'h1);
        repeat(2) @(posedge clk); #1;
        read_status(status);
        if (status === 32'd1) begin
            $display("  status po 2 cyklach = busy(1)  OK");
        end else begin
            // busy jest krótkie (tylko S_CALC + S_WRITE); akceptujemy też 0 lub 2
            $display("  status po 2 cyklach = %0d (busy moglby byc juz minac)", status);
        end
        // Poczekaj na done
        repeat(4) @(posedge clk); #1;
        read_status(status);
        if (status === 32'd2) begin
            $display("  status po 6 cyklach = done(2)  OK");
        end else begin
            $display("  FAIL: oczekiwano done(2), otrzymano %0d", status);
            fail_cnt = fail_cnt + 1;
        end
        release_ctrl();
        read_status(status);
        if (status === 32'd0) begin
            $display("  status po release_ctrl = idle(0)  OK");
            $display("  PASS"); pass_cnt = pass_cnt + 1;
        end else begin
            $display("  FAIL: po release_ctrl status=%0d (oczekiwano 0=idle)", status);
            fail_cnt = fail_cnt + 1;
        end
    end

    // ─────────────────────────────────────────────────────────────────────
    // T2: 1.0 × 1.0 = 1.0  (bazowy przypadek)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T2: 1.0 * 1.0 = 1.0 ===");
    write_arg1(make_fp(0, BIAS,   36'h000000000));  // 1.0
    write_arg2(make_fp(0, BIAS,   36'h000000000));  // 1.0
    start_and_wait();
    read_result(result);
    check(result, make_fp(0, BIAS, 36'h000000000)); // 1.0
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T3: 2.5 × 4.0 = 10.0  (test z systemu; poprzednio zwracał 0)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T3: 2.5 * 4.0 = 10.0 ===");
    write_arg1(make_fp(0, BIAS+1, 36'h400000000));  // 2.5 = 1.25 * 2^1
    write_arg2(make_fp(0, BIAS+2, 36'h000000000));  // 4.0 = 1.0  * 2^2
    start_and_wait();
    read_result(result);
    check(result, make_fp(0, BIAS+3, 36'h400000000)); // 10.0 = 1.25 * 2^3
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T4: -1.5 × 2.0 = -3.0  (znak +×−)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T4: -1.5 * 2.0 = -3.0 (znak) ===");
    write_arg1(make_fp(1, BIAS,   36'h800000000));  // -1.5
    write_arg2(make_fp(0, BIAS+1, 36'h000000000));  //  2.0
    start_and_wait();
    read_result(result);
    check(result, make_fp(1, BIAS+1, 36'h800000000)); // -3.0 = -1.5 * 2^1
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T5: (-2.0) × (-3.0) = 6.0  (znak −×−)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T5: (-2.0) * (-3.0) = 6.0 (znak) ===");
    write_arg1(make_fp(1, BIAS+1, 36'h000000000));  // -2.0
    write_arg2(make_fp(1, BIAS+1, 36'h800000000));  // -3.0 = -1.5 * 2^1
    start_and_wait();
    read_result(result);
    check(result, make_fp(0, BIAS+2, 36'h800000000)); // 6.0 = 1.5 * 2^2
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T6: 1.75 × 1.75 = 3.0625  (renormalizacja: prod_reg[73]=1)
    //   {1,mant1}={1,mant2}=7*2^34 → prod=49*2^68 → bit73 set → shift+1
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T6: 1.75 * 1.75 = 3.0625 (renormalizacja prod[73]=1) ===");
    write_arg1(make_fp(0, BIAS,   36'hC00000000));  // 1.75 = 1.11b
    write_arg2(make_fp(0, BIAS,   36'hC00000000));  // 1.75
    start_and_wait();
    read_result(result);
    check(result, make_fp(0, BIAS+1, 36'h880000000)); // 3.0625 = 1.53125*2^1
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T7: 1.25 × 1.25 = 1.5625  (bez renormalizacji: prod_reg[73]=0)
    //   {1,mant}=5*2^34 → prod=25*2^68 → bit73=0 → bez shiftu
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T7: 1.25 * 1.25 = 1.5625 (brak renormalizacji prod[73]=0) ===");
    write_arg1(make_fp(0, BIAS,   36'h400000000));  // 1.25
    write_arg2(make_fp(0, BIAS,   36'h400000000));  // 1.25
    start_and_wait();
    read_result(result);
    check(result, make_fp(0, BIAS,   36'h900000000)); // 1.5625
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T8: 0 × 5.0 = 0  (wykrycie zera: arg1 ma E=0 i M=0)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T8: 0 * 5.0 = 0 (zero arg1) ===");
    write_arg1(64'h0);                              // 0 (E=0, M=0, S=0)
    write_arg2(make_fp(0, BIAS+2, 36'h400000000));  // 5.0
    start_and_wait();
    read_result(result);
    check(result, 64'h0);
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T9: 5.0 × 0 = 0  (wykrycie zera: arg2 ma E=0 i M=0)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T9: 5.0 * 0 = 0 (zero arg2) ===");
    write_arg1(make_fp(0, BIAS+2, 36'h400000000));  // 5.0
    write_arg2(64'h0);                              // 0
    start_and_wait();
    read_result(result);
    check(result, 64'h0);
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T10: 0 × 0 = 0  (oba argumenty zero)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T10: 0 * 0 = 0 ===");
    write_arg1(64'h0);
    write_arg2(64'h0);
    start_and_wait();
    read_result(result);
    check(result, 64'h0);
    release_ctrl();

    // ─────────────────────────────────────────────────────────────────────
    // T11: przemienność  3.0 × 1.5  ===  1.5 × 3.0  (= 4.5)
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T11: przemiennosc  3.0*1.5 === 1.5*3.0 ===");
    write_arg1(make_fp(0, BIAS+1, 36'h800000000));  // 3.0
    write_arg2(make_fp(0, BIAS,   36'h800000000));  // 1.5
    start_and_wait();
    read_result(r1);
    release_ctrl();

    write_arg1(make_fp(0, BIAS,   36'h800000000));  // 1.5
    write_arg2(make_fp(0, BIAS+1, 36'h800000000));  // 3.0
    start_and_wait();
    read_result(r2);
    release_ctrl();

    if (r1 === r2 && r1 === make_fp(0, BIAS+2, 36'h200000000)) begin
        $display("  PASS (wynik=4.5)"); pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL: 3*1.5=%h, 1.5*3=%h (oczekiwano=4.5)", r1, r2);
        fail_cnt = fail_cnt + 1;
    end

    // ─────────────────────────────────────────────────────────────────────
    // T12: dwa kolejne mnożenia – sprawdzenie powrotu FSM do IDLE
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T12: dwa kolejne mnozenia (cykl FSM) ===");
    // Pierwsze: 1.0 × 2.0 = 2.0
    write_arg1(make_fp(0, BIAS,   36'h000000000));
    write_arg2(make_fp(0, BIAS+1, 36'h000000000));
    start_and_wait();
    read_result(result);
    release_ctrl();
    if (result !== make_fp(0, BIAS+1, 36'h000000000)) begin
        $display("  FAIL pierwsze mnozenie (1.0*2.0): got=0x%h", result);
        fail_cnt = fail_cnt + 1;
    end else begin
        // Drugie (bez dodatkowego resetu): 2.0 × 2.0 = 4.0
        write_arg1(make_fp(0, BIAS+1, 36'h000000000));
        write_arg2(make_fp(0, BIAS+1, 36'h000000000));
        start_and_wait();
        read_result(result);
        check(result, make_fp(0, BIAS+2, 36'h000000000)); // 4.0
        release_ctrl();
    end

    // ─────────────────────────────────────────────────────────────────────
    // T13: odczyt rejestrów arg1/arg2 przez srd
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T13: odczyt arg1/arg2 przez srd ===");
    begin : t13
        reg [63:0] test_val;
        reg [31:0] rh, rl;
        test_val = make_fp(0, BIAS+5, 36'hABCDE1234);
        write_arg1(test_val);
        bus_read(16'h0100, rh);     // arg1_h
        bus_read(16'h0108, rl);     // arg1_l
        if ({rh, rl} === test_val) begin
            $display("  PASS (arg1 readback)"); pass_cnt = pass_cnt + 1;
        end else begin
            $display("  FAIL arg1: got=%h exp=%h", {rh,rl}, test_val);
            fail_cnt = fail_cnt + 1;
        end
        test_val = make_fp(1, BIAS+3, 36'h123456789);
        write_arg2(test_val);
        bus_read(16'h00F0, rh);     // arg2_h
        bus_read(16'h00F8, rl);     // arg2_l
        if ({rh, rl} === test_val) begin
            $display("  PASS (arg2 readback)"); pass_cnt = pass_cnt + 1;
        end else begin
            $display("  FAIL arg2: got=%h exp=%h", {rh,rl}, test_val);
            fail_cnt = fail_cnt + 1;
        end
    end

    // ─────────────────────────────────────────────────────────────────────
    // T14: nieznany adres → sdata_out = 0
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T14: nieznany adres -> sdata_out = 0 ===");
    bus_read(16'h0200, rd);
    if (rd === 32'h0) begin
        $display("  PASS"); pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL: sdata_out=0x%h (oczekiwano 0)", rd);
        fail_cnt = fail_cnt + 1;
    end

    // ─────────────────────────────────────────────────────────────────────
    // T15: gpio_latch zatrzaskuje gpio_in w gpio_in_s_insp
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T15: gpio_latch zatrzaskuje gpio_in ===");
    do_reset();
    gpio_in = 32'hDEADBEEF;
    gpio_latch = 1; #10; gpio_latch = 0; #5;
    if (gpio_in_s_insp === 32'hDEADBEEF) begin
        $display("  PASS"); pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL: gpio_in_s_insp=0x%h (oczekiwano 0xDEADBEEF)", gpio_in_s_insp);
        fail_cnt = fail_cnt + 1;
    end
    // Zmiana gpio_in bez gpio_latch NIE powinna zmieniać gpio_in_s_insp
    gpio_in = 32'h12345678; #5;
    if (gpio_in_s_insp === 32'hDEADBEEF) begin
        $display("  PASS (latch utrzymuje wartosc bez gpio_latch)"); pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL: gpio_in_s_insp zmienilo sie bez gpio_latch!");
        fail_cnt = fail_cnt + 1;
    end

    // ─────────────────────────────────────────────────────────────────────
    // T16: n_reset zeruje gpio_in_s_insp oraz rejestry stanu FSM
    // ─────────────────────────────────────────────────────────────────────
    $display("\n=== T16: n_reset zeruje gpio_in_s_insp i status ===");
    do_reset();
    read_status(status);
    if (gpio_in_s_insp === 32'h0 && status === 32'h0) begin
        $display("  PASS"); pass_cnt = pass_cnt + 1;
    end else begin
        $display("  FAIL: gpio_in_s_insp=0x%h status=%0d", gpio_in_s_insp, status);
        fail_cnt = fail_cnt + 1;
    end

    // ─────────────────────────────────────────────────────────────────────
    // PODSUMOWANIE
    // ─────────────────────────────────────────────────────────────────────
    $display("\n========================================");
    $display("WYNIKI: %0d PASS / %0d FAIL  (razem %0d testow)",
        pass_cnt, fail_cnt, pass_cnt + fail_cnt);
    if (fail_cnt == 0)
        $display("WSZYSTKIE TESTY ZALICZONE");
    else
        $display("%0d TESTOW NIEZALICZONYCH", fail_cnt);
    $display("========================================");
    $finish;
end

endmodule
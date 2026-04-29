module gpioemu_tb;
    // Sygnały sterujące
    reg n_reset, clk, srd, swr, gpio_latch;
    reg [15:0] saddress;
    reg [31:0] sdata_in, gpio_in;
    
    // Sygnały wyjściowe z modułu
    wire [31:0] sdata_out, gpio_out, gpio_in_s_insp;
    
    // Instancja testowanego modułu
    gpioemu test_module (
        .n_reset(n_reset),
        .saddress(saddress),
        .srd(srd),
        .swr(swr),
        .sdata_in(sdata_in),
        .sdata_out(sdata_out),
        .gpio_in(gpio_in),
        .gpio_latch(gpio_latch),
        .gpio_out(gpio_out),
        .clk(clk),
        .gpio_in_s_insp(gpio_in_s_insp)
    );

    // Generowanie zegara (okres 10 jednostek)
    always #5 clk = ~clk;

    // BIAS dla 27-bitowego wykładnika: 2^(27-1) = 2^26 = 67108864
    localparam [26:0] BIAS = 27'd67108864;

    // Zmienne pomocnicze
    reg [63:0] result_val, result1, result2;
    reg [31:0] status_val;

    //----------------------------------------------------------------------
    // Funkcja pomocnicza do tworzenia liczby 64-bitowej w formacie:
    // [0]=S (znak), [27:1]=E (eksponenta z BIAS), [63:28]=M (mantysa)
    //----------------------------------------------------------------------
    function [63:0] make_fp;
        input        s;          // znak (0=dodatni, 1=ujemny)
        input [26:0] e;          // wykładnik (już z BIAS-em)
        input [35:0] m;          // mantysa (bez ukrytej jedynki)
        begin
            make_fp[0]      = s;
            make_fp[27:1]   = e;
            make_fp[63:28]  = m;
        end
    endfunction

    //----------------------------------------------------------------------
    // Funkcja pomocnicza do czytelnego wyświetlania liczby FP
    //----------------------------------------------------------------------
    task display_fp;
        input [63:0] fp;
        input [255:0] label;     // opcjonalna etykieta
        reg [35:0] mant;
        reg [26:0] exp;
        reg        sign;
        begin
            sign = fp[0];
            exp  = fp[27:1];
            mant = fp[63:28];
            
            if (label != "")
                $write("%s: ", label);
            else
                $write("FP: ");
                
            if (exp == 27'd0 && mant == 36'd0) begin
                $display("0.0 (S=%b, E=0, M=0)", sign);
            end else begin
                $display("S=%b, E=%0d (BIAS+%0d), M=0x%h",
                sign, exp, exp - BIAS, mant);
            end
        end
    endtask

    //----------------------------------------------------------------------
    // Pomocnicze taski do często powtarzanych operacji
    //----------------------------------------------------------------------
    task reset_module;
        begin
            #10 n_reset = 0;
            #10 n_reset = 1;
            #10;
        end
    endtask

    task write_arg1;
        input [63:0] value;
        begin
            #10 saddress = 16'h0100; sdata_in = value[63:32]; swr = 1; #10 swr = 0;
            #10 saddress = 16'h0108; sdata_in = value[31:0];  swr = 1; #10 swr = 0;
        end
    endtask

    task write_arg2;
        input [63:0] value;
        begin
            #10 saddress = 16'h00F0; sdata_in = value[63:32]; swr = 1; #10 swr = 0;
            #10 saddress = 16'h00F8; sdata_in = value[31:0];  swr = 1; #10 swr = 0;
        end
    endtask

    task start_operation;
        begin
            #10 saddress = 16'h00D0; sdata_in = 32'h00000001; swr = 1; #10 swr = 0;
            #50; // Oczekiwanie na zakonczenie
        end
    endtask

    task read_result;
        output [63:0] result;
        begin
            #10 saddress = 16'h00D8; srd = 1; #10 srd = 0;
            result[63:32] = sdata_out;
            #10 saddress = 16'h00E0; srd = 1; #10 srd = 0;
            result[31:0] = sdata_out;
        end
    endtask

    task read_status;
        output [31:0] stat;
        begin
            #10 saddress = 16'h00E8; srd = 1; #10 srd = 0;
            stat = sdata_out;
        end
    endtask

    //======================================================================
    // GŁÓWNY BLOK TESTOWY
    //======================================================================
    initial begin
        $dumpfile("gpioemu.vcd");
        $dumpvars(0, gpioemu_tb);

        // Inicjalizacja
        clk = 0; n_reset = 1;
        srd = 0; swr = 0;
        gpio_latch = 0;
        saddress = 16'h0;
        sdata_in = 32'h0;
        gpio_in = 32'h0;
        result_val = 64'h0;
        status_val = 32'h0;
        result1 = 64'h0;
        result2 = 64'h0;

        // Reset początkowy
        reset_module();

        // ================================================================
        // TEST 1: 1.0 * 2.0 = 2.0
        // ================================================================
        $display("\n=== TEST 1: 1.0 * 2.0 = 2.0 ===");
        $display("  arg1 (1.0): S=0, E=BIAS, M=0");
        $display("  arg2 (2.0): S=0, E=BIAS+1, M=0");
        write_arg1(make_fp(1'b0, BIAS, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        start_operation();
        read_status(status_val);
        $display("  Status: %h (oczekiwane: 00000002)", status_val);
        read_result(result_val);
        display_fp(result_val, "  Wynik ");
        $display("  Oczekiwany: S=0, E=BIAS+1, M=0");
        $display("  TEST 1: %s",
        (result_val[0] == 0 && result_val[27:1] == BIAS+1 && result_val[63:28] == 0) 
        ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 2: -1.5 * 2.0 = -3.0
        // ================================================================
        $display("\n=== TEST 2: -1.5 * 2.0 = -3.0 ===");
        $display("  arg1 (-1.5): S=1, E=BIAS, M=0.5");
        $display("  arg2 (2.0):  S=0, E=BIAS+1, M=0");
        reset_module();
        write_arg1(make_fp(1'b1, BIAS, 36'h80000000));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        start_operation();
        read_result(result_val);
        display_fp(result_val, "  Wynik ");
        $display("  Oczekiwany: S=1, E=BIAS+1, M=0.5");
        $display("  TEST 2: %s",
        (result_val[0] == 1 && result_val[63:28] == 36'h80000000)
        ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 3: Neutralność – odczyt spod nieprawidłowego adresu
        // ================================================================
        $display("\n=== TEST 3: Neutralnosc magistrali ===");
        #10 saddress = 16'hFFFF; srd = 1; #10 srd = 0;
        $display("  Odczyt spod FFFF: %h (oczekiwane: 00000000)", sdata_out);
        $display("  TEST 3: %s", (sdata_out == 32'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 4: Brak uruchomienia przy control = 0
        // ================================================================
        $display("\n=== TEST 4: Control = 0 nie uruchamia ===");
        reset_module();
        write_arg1(make_fp(1'b0, BIAS, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        #10 saddress = 16'h00D0; sdata_in = 32'h00000000; swr = 1; #10 swr = 0;
        #20;
        read_status(status_val);
        $display("  Status: %h (oczekiwane: 00000000 - IDLE)", status_val);
        $display("  TEST 4: %s", (status_val == 32'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 5: GPIO – zatrzaśnięcie i podgląd
        // ================================================================
        $display("\n=== TEST 5: Zatrzasniecie GPIO ===");
        #10 gpio_in = 32'hDEADBEEF;
        #10 gpio_latch = 1; #10 gpio_latch = 0;
        #10;
        $display("  gpio_in_s_insp: %h (oczekiwane: DEADBEEF)", gpio_in_s_insp);
        $display("  TEST 5: %s", (gpio_in_s_insp == 32'hDEADBEEF) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 6: Reset globalny czyści rejestry
        // ================================================================
        $display("\n=== TEST 6: Reset globalny ===");
        reset_module();
        #10 saddress = 16'h0100; srd = 1; #10 srd = 0;
        $display("  arg1_h po resecie: %h (oczekiwane: 0)", sdata_out);
        $display("  TEST 6: %s", (sdata_out == 32'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 7: Mnożenie przez zero (0.0 * 5.0 = 0.0)
        // ================================================================
        $display("\n=== TEST 7: Mnożenie przez zero (0.0 * 5.0 = 0.0) ===");
        $display("  arg1 (0.0): E=0, M=0 (specjalny wzorzec zera)");
        $display("  arg2 (5.0): S=0, E=BIAS+2, M=0.25");
        reset_module();
        write_arg1(make_fp(1'b0, 27'd0, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+2, 36'h4000000));
        start_operation();
        read_result(result_val);
        display_fp(result_val, "  Wynik ");
        $display("  Oczekiwany: 0.0");
        $display("  TEST 7: %s",
        (result_val == 64'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 8: -2.0 * -3.0 = +6.0
        // ================================================================
        $display("\n=== TEST 8: -2.0 * -3.0 = +6.0 ===");
        $display("  arg1 (-2.0): S=1, E=BIAS+1, M=0");
        $display("  arg2 (-3.0): S=1, E=BIAS+1, M=0.5");
        reset_module();
        write_arg1(make_fp(1'b1, BIAS+1, 36'h0));
        write_arg2(make_fp(1'b1, BIAS+1, 36'h80000000));
        start_operation();
        read_result(result_val);
        display_fp(result_val, "  Wynik ");
        $display("  Oczekiwany: S=0, E=BIAS+2, M=0.5");
        $display("  TEST 8: %s",
        (result_val[0] == 0 && result_val[63:28] == 36'h80000000)
        ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 9: Odczyt przed zakonczeniem operacji
        // ================================================================
        $display("\n=== TEST 9: Odczyt przed zakonczeniem ===");
        reset_module();
        write_arg1(make_fp(1'b0, BIAS, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        
        // Uruchom, ale NIE czekaj długo
        #10 saddress = 16'h00D0; sdata_in = 32'h1; swr = 1; #10 swr = 0;
        #10; // Tylko jeden cykl
        
        read_status(status_val);
        $display("  Status po 1 cyklu: %h (0=IDLE, 1=BUSY, 2=DONE)", status_val);
        
        read_result(result_val);
        display_fp(result_val, "  Przedwczesny odczyt");
        
        // Poczekaj na zakonczenie
        #50;
        read_status(status_val);
        $display("  Status po odczekaniu: %h (oczekiwane: 2=DONE)", status_val);
        read_result(result_val);
        display_fp(result_val, "  Wynik po odczekaniu");
        $display("  TEST 9: %s",
        (result_val[0] == 0 && result_val[27:1] == BIAS+1 && result_val[63:28] == 0)
        ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 10: Podwójny start
        // ================================================================
        $display("\n=== TEST 10: Podwójny start ===");
        reset_module();
        write_arg1(make_fp(1'b0, BIAS, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        
        // Pierwszy start
        #10 saddress = 16'h00D0; sdata_in = 32'h1; swr = 1; #10 swr = 0;
        #10;
        
        // Drugi start (przed zakonczeniem)
        #10 saddress = 16'h00D0; sdata_in = 32'h1; swr = 1; #10 swr = 0;
        #50;
        read_result(result_val);
        display_fp(result_val, "  Wynik po podwojnym starcie");
        $display("  TEST 10: %s",
        (result_val[0] == 0 && result_val[27:1] == BIAS+1 && result_val[63:28] == 0)
        ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 11: Nieprawidłowe wartości control
        // ================================================================
        $display("\n=== TEST 11: Nieprawidlowe wartosci control ===");
        reset_module();
        write_arg1(make_fp(1'b0, BIAS, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        
        // control = 0xFF
        #10 saddress = 16'h00D0; sdata_in = 32'h000000FF; swr = 1; #10 swr = 0;
        #30;
        read_status(status_val);
        $display("  Status po control=0xFF: %h (oczekiwane: 0=IDLE)", status_val);
        $display("  TEST 11a: %s", (status_val == 32'h0) ? "ZALICZONY" : "BLAD");
        
        // control = 2
        #10 saddress = 16'h00D0; sdata_in = 32'h00000002; swr = 1; #10 swr = 0;
        #30;
        read_status(status_val);
        $display("  Status po control=2: %h (oczekiwane: 0=IDLE)", status_val);
        $display("  TEST 11b: %s", (status_val == 32'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 12: Częściowy zapis argumentów
        // ================================================================
        $display("\n=== TEST 12: Czesciowy zapis argumentow ===");
        reset_module();
        
        // Zapisz tylko górną część arg1
        #10 saddress = 16'h0100; sdata_in = 32'h80000000; swr = 1; #10 swr = 0;
        
        // Dolna część powinna być 0 (z resetu)
        #10 saddress = 16'h0108; srd = 1; #10 srd = 0;
        $display("  arg1_l po czesciowym zapisie: %h (oczekiwane: 00000000)", sdata_out);
        $display("  TEST 12: %s", (sdata_out == 32'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 13: Przemienność mnożenia (a*b == b*a)
        // ================================================================
        $display("\n=== TEST 13: Przemiennosc mnozenia ===");
        
        // 2.0 * 1.0
        reset_module();
        write_arg1(make_fp(1'b0, BIAS+1, 36'h0));  // 2.0
        write_arg2(make_fp(1'b0, BIAS, 36'h0));    // 1.0
        start_operation();
        read_result(result1);
        display_fp(result1, "  2.0*1.0");
        
        // 1.0 * 2.0
        reset_module();
        write_arg1(make_fp(1'b0, BIAS, 36'h0));    // 1.0
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));  // 2.0
        start_operation();
        read_result(result2);
        display_fp(result2, "  1.0*2.0");
        
        if (result1 == result2) begin
            $display("  Przemiennosc: OK");
            $display("  TEST 13: ZALICZONY");
        end else begin
            $display("  Przemiennosc: BLAD! (%h != %h)", result1, result2);
            $display("  TEST 13: BLAD");
        end

        // ================================================================
        // TEST 14: Próba zapisu do rejestru tylko do odczytu (status)
        // ================================================================
        $display("\n=== TEST 14: Zapis do rejestru status (read-only) ===");
        reset_module();
        #10 saddress = 16'h00E8; sdata_in = 32'hFFFFFFFF; swr = 1; #10 swr = 0;
        #10 saddress = 16'h00E8; srd = 1; #10 srd = 0;
        $display("  Status po próbie zapisu: %h (oczekiwane: 0)", sdata_out);
        $display("  TEST 14: %s", (sdata_out == 32'h0) ? "ZALICZONY" : "BLAD");

        // ================================================================
        // TEST 15: Wielokrotne mnożenie bez resetu
        // ================================================================
        $display("\n=== TEST 15: Wielokrotne mnozenie ===");
        reset_module();
        
        // Pierwsza operacja: 1.0 * 2.0 = 2.0
        write_arg1(make_fp(1'b0, BIAS, 36'h0));
        write_arg2(make_fp(1'b0, BIAS+1, 36'h0));
        start_operation();
        read_result(result1);
        display_fp(result1, "  Operacja 1 (1.0*2.0)");
        
        // Druga operacja: 2.0 * 4.0 = 8.0
        write_arg1(make_fp(1'b0, BIAS+1, 36'h0));  // 2.0
        write_arg2(make_fp(1'b0, BIAS+2, 36'h0));  // 4.0
        start_operation();
        read_result(result2);
        display_fp(result2, "  Operacja 2 (2.0*4.0)");
        $display("  Oczekiwany: S=0, E=BIAS+3, M=0");
        $display("  TEST 15: %s",
        (result2[0] == 0 && result2[27:1] == BIAS+3 && result2[63:28] == 0) 
        ? "ZALICZONY" : "BLAD");

        // Koniec symulacji
        #100;
        $display("\n===========================================");
        $display("=== WSZYSTKIE TESTY ZAKONCZONE ===");
        $display("===========================================\n");
        $finish;
    end

    // Monitor komunikatów (uproszczony)
    initial begin
        $monitor("Czas=%0t | Adr=%h | swr=%b srd=%b | data_in=%h | data_out=%h",
        $time, saddress, swr, srd, sdata_in, sdata_out);
    end
endmodule
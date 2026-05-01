module gpioemu_tb;

    
    // SYGNAŁY
    
    reg n_reset, clk, srd, swr, gpio_latch;
    reg [15:0] saddress;
    reg [31:0] sdata_in, gpio_in;

    wire [31:0] sdata_out, gpio_out, gpio_in_s_insp;

    
    // test_module
    
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

    // ZEGAR
    always #5 clk = ~clk;

    // POMOCNICZE
    reg [63:0] result, r1, r2;
    reg [31:0] status;

    localparam [26:0] BIAS = 27'd67108864; // BIAS = 2^(27-1)

    // FORMAT FP:
    // [0]     znak
    // [27:1]  wykładnik z BIAS
    // [63:28] mantysa (bez ukrytej jedynki)
    
    function [63:0] make_fp;
        input        s;
        input [26:0] e;
        input [35:0] m;
        begin
            make_fp = 64'd0;
            make_fp[0]      = s;
            make_fp[27:1]   = e;
            make_fp[63:28]  = m;
        end
    endfunction

    task reset;
        begin
            n_reset = 0; #10;
            n_reset = 1; #10;
        end
    endtask

    task write_arg1(input [63:0] v);
        begin
            saddress = 16'h0100; sdata_in = v[63:32]; swr = 1; #10 swr = 0;
            saddress = 16'h0108; sdata_in = v[31:0];  swr = 1; #10 swr = 0;
        end
    endtask

    task write_arg2(input [63:0] v);
        begin
            saddress = 16'h00F0; sdata_in = v[63:32]; swr = 1; #10 swr = 0;
            saddress = 16'h00F8; sdata_in = v[31:0];  swr = 1; #10 swr = 0;
        end
    endtask

    task start;
        begin
            saddress = 16'h00D0; sdata_in = 32'h1; swr = 1; #10 swr = 0;
            #40; // czas na FSM: CALC -> WRITE -> DONE
        end
    endtask

    task ctrl_reset;
        begin
            saddress = 16'h00D0;
            sdata_in = 32'h0;   // echo 0 > ctrl
            swr = 1; #10 swr = 0;
            #20;               // daj FSM wrócić do IDLE
        end
    endtask


    task read_result(output [63:0] v);
        begin
            saddress = 16'h00D8; srd = 1; #10 srd = 0;
            v[63:32] = sdata_out;
            saddress = 16'h00E0; srd = 1; #10 srd = 0;
            v[31:0]  = sdata_out;
        end
    endtask

    task read_status(output [31:0] v);
        begin
            saddress = 16'h00E8; srd = 1; #10 srd = 0;
            v = sdata_out;
        end
    endtask

    // Wypisywanie pól liczby
    task print_fp(input [63:0] fp);
        begin
            $display("  sign = %0d", fp[0]);
            $display("  exp  = %0d (BIAS %+0d)", fp[27:1], fp[27:1] - BIAS);
            $display("  mant = 0x%h", fp[63:28]);
        end
    endtask

    
    // TESTY
    
    initial begin
        $dumpfile("gpioemu.vcd");
        $dumpvars(0, gpioemu_tb);

        clk = 0;
        srd = 0; swr = 0;
        gpio_latch = 0;
        saddress = 0;
        sdata_in = 0;
        gpio_in = 0;

        // TEST 1: 1.0 * 2.0 = 2.0
        $display("TEST 1 - mnozenie 1.0 * 2.0");
        reset();
        write_arg1(make_fp(0, BIAS,   36'h0)); // 1.0
        write_arg2(make_fp(0, BIAS+1, 36'h0)); // 2.0
        start();
        read_result(result);
        print_fp(result);
        $display("wynik = %h", result);

        // TEST 2: -1.5 * 2.0 = -3.0
        $display("\nTEST 2 - mnozenie -1.5 * 2.0");
        ctrl_reset();
        write_arg1(make_fp(1, BIAS,   36'h80000000)); // -1.5
        write_arg2(make_fp(0, BIAS+1, 36'h0));
        start();
        read_result(result);
        print_fp(result);
        $display("wynik = %h", result);

        // TEST 3: 1.75 * 1.75 = 3.0625 (renormalizacja w górę)
        $display("\nTEST 3 - mnozenie 1.75 * 1.75");
        ctrl_reset();
        write_arg1(make_fp(0, BIAS, 36'hC000000)); // 1.75
        write_arg2(make_fp(0, BIAS, 36'hC000000));
        start();
        read_result(result);
        print_fp(result);
        $display("wynik = %h", result);

        // TEST 4: 1.25 * 1.25 = 1.5625 (bez renormalizacji)
        $display("\nTEST 4 - mnozenie 1.25 * 1.25");
        ctrl_reset();
        write_arg1(make_fp(0, BIAS, 36'h4000000)); // 1.25
        write_arg2(make_fp(0, BIAS, 36'h4000000));
        start();
        read_result(result);
        print_fp(result);
        $display("wynik = %h", result);

        // TEST 5: 2.0 * 4.0 = 8.0
        $display("\nTEST 5 - mnozenie 2.0 * 4.0");
        ctrl_reset();
        write_arg1(make_fp(0, BIAS+1, 36'h0)); // 2.0
        write_arg2(make_fp(0, BIAS+2, 36'h0)); // 4.0
        start();
        read_result(result);
        print_fp(result);
        $display("wynik = %h", result);


        // TEST 6: 0 * 5 = 0
        $display("\nTEST 6 - mnozenie 0 * 5");
        ctrl_reset();
        write_arg1(make_fp(0, 0, 36'h0)); // 0.0
        write_arg2(make_fp(0, BIAS+2, 36'h4000000)); // 5.0
        start();
        read_result(result);
        print_fp(result);
        $display("wynik = %h", result);


        // TEST 7: przemienność
        $display("\nTEST 7 - przemiennosc mnozenia");
        ctrl_reset();
        write_arg1(make_fp(0, BIAS+1, 36'h0)); // 2.0
        write_arg2(make_fp(0, BIAS,   36'h0)); // 1.0
        start();
        read_result(r1);
        print_fp(r1);

        ctrl_reset();
        write_arg1(make_fp(0, BIAS,   36'h0)); // 1.0
        write_arg2(make_fp(0, BIAS+1, 36'h0)); // 2.0
        start();
        read_result(r2);
        print_fp(r2);

        $display("przemiennosc = %s", (r1 == r2) ? "OK" : "BLAD");

        $display("\nWSZYSTKIE TESTY ZAKONCZONE");
        $finish;
    end

endmodule
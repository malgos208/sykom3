/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */
/* verilator lint_off CASEINCOMPLETE */
/* verilator lint_off MULTIDRIVEN */

/*
 * gpioemu.v – 64-bitowy mnożnik zmiennoprzecinkowy dla SYKOM
 *
 * Format 64-bitowej liczby zmiennoprzecinkowej:
 *   bit 63      – znak (sign)
 *   bity 62:36  – eksponenta (27 bitów), bias = 2^26 − 1 = 67108863
 *   bity 35:0   – mantysa (36 bitów), bez ukrytego bitu jedności
 *
 * Podział na rejestry 32-bitowe (część starsza HI / młodsza LO):
 *   HI[31]    = znak
 *   HI[30:4]  = eksponenta (27 bitów)
 *   HI[3:0]   = mantysa[35:32]  (4 bity)
 *   LO[31:0]  = mantysa[31:0]   (32 bity)
 *
 * Adresy rejestrów (relatywne):
 *   0x0100 – argument 1, część starsza (HI)
 *   0x0108 – argument 1, część młodsza (LO)
 *   0x00F0 – argument 2, część starsza (HI)
 *   0x00F8 – argument 2, część młodsza (LO)
 *   0x00D0 – rejestr sterujący (CTRL)
 *   0x00E8 – rejestr stanu     (STATUS)
 *   0x00D8 – wynik, część starsza (HI)
 *   0x00E0 – wynik, część młodsza (LO)
 *
 * Kody stanu:
 *   0x00000000 – idle
 *   0x00000001 – computing
 *   0x00000002 – done (OK lub wynik zerowy)
 *   0x00000004 – overflow  (wynik → ±Inf)
 *   0x00000006 – underflow (wynik → ±0)
 */

module gpioemu(n_reset, saddress, srd, swr, sdata_in, sdata_out,
               gpio_in, gpio_latch, gpio_out, clk, gpio_in_s_insp);

    input         n_reset;
    input  [15:0] saddress;
    input         srd;
    input         swr;
    input  [31:0] sdata_in;
    output [31:0] sdata_out;

    input  [31:0] gpio_in;
    input         gpio_latch;
    output [31:0] gpio_out;
    input         clk;
    output [31:0] gpio_in_s_insp;

    reg [31:0] gpio_out_s;
    reg [31:0] gpio_in_s;
    reg [31:0] sdata_out_s;

    // -------------------------------------------------------
    // Rejestry projektowe
    // -------------------------------------------------------
    reg [31:0] arg1_hi, arg1_lo;
    reg [31:0] arg2_hi, arg2_lo;
    reg [31:0] ctrl;
    reg [31:0] status;
    reg [31:0] res_hi, res_lo;

    // Dekodowanie pól argumentu 1
    wire        sign1 = arg1_hi[31];
    wire [26:0] exp1  = arg1_hi[30:4];
    wire [35:0] mant1 = {arg1_hi[3:0], arg1_lo};

    // Dekodowanie pól argumentu 2
    wire        sign2 = arg2_hi[31];
    wire [26:0] exp2  = arg2_hi[30:4];
    wire [35:0] mant2 = {arg2_hi[3:0], arg2_lo};

    // -------------------------------------------------------
    // Automat stanów (FSM)
    // -------------------------------------------------------
    localparam S_IDLE = 3'd0;
    localparam S_CALC = 3'd1;
    localparam S_NORM = 3'd2;
    localparam S_DONE = 3'd3;
    reg [2:0] state;

    // Iloczyn: (1 + mantysa_36bit) * (1 + mantysa_36bit)
    // (37 bitów) * (37 bitów) = 74 bity
    reg [73:0]       prod_reg;
    reg signed [29:0] exp_reg;   // exp1 + exp2 − bias (ze znakiem)
    reg               sign_reg;

    // Normalizacja iloczynu:
    //   jeśli bit 73 = 1 → wynik ≥ 2, przesuwamy mantysę w prawo o 1 i dodajemy 1 do wykładnika
    //   jeśli bit 73 = 0 → wynik ∈ [1, 2), mantysa gotowa
    wire signed [29:0] final_exp_w =
        prod_reg[73] ? (exp_reg + 30'sd1) : exp_reg;
    wire [35:0] final_mant =
        prod_reg[73] ? prod_reg[72:37] : prod_reg[71:36];

    // Zakres prawidłowej eksponenty: 1 … 134217726 (0x7FFFFFE)
    // Nieskończoność: 134217727 (0x7FFFFFF = same bits all-1)
    // Zero:           eksponenta = 0, mantysa = 0

    // -------------------------------------------------------
    // 1. Zapis z procesora (reaguje na zbocze narastające swr)
    // -------------------------------------------------------
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1_hi <= 32'd0;  arg1_lo <= 32'd0;
            arg2_hi <= 32'd0;  arg2_lo <= 32'd0;
            ctrl    <= 32'd0;
        end else begin
            case (saddress)
                16'h0100: arg1_hi <= sdata_in;
                16'h0108: arg1_lo <= sdata_in;
                16'h00F0: arg2_hi <= sdata_in;
                16'h00F8: arg2_lo <= sdata_in;
                16'h00D0: ctrl    <= sdata_in;
            endcase
        end
    end

    // -------------------------------------------------------
    // 2. Automat stanów i obliczenia (zegar 1 kHz)
    // -------------------------------------------------------
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            status   <= 32'd0;
            res_hi   <= 32'd0;
            res_lo   <= 32'd0;
            state    <= S_IDLE;
            gpio_out_s <= 32'd0;
            gpio_in_s  <= 32'd0;
            prod_reg <= 74'd0;
            exp_reg  <= 30'sd0;
            sign_reg <= 1'b0;
        end else begin
            if (gpio_latch) gpio_in_s <= gpio_in;

            case (state)

                // ── Stan bezczynności ──────────────────────────
                S_IDLE: begin
                    if (ctrl[0] == 1'b1) begin
                        // Sprawdzenie: czy któryś z argumentów jest zerem?
                        if ((exp1 == 27'd0 && mant1 == 36'd0) ||
                            (exp2 == 27'd0 && mant2 == 36'd0)) begin
                            res_hi <= 32'd0;
                            res_lo <= 32'd0;
                            status <= 32'h00000002; // done – wynik zerowy
                            state  <= S_DONE;
                        end else begin
                            // Obliczenie iloczynu mantys (z ukrytym bitem 1)
                            prod_reg <= {1'b1, mant1} * {1'b1, mant2};
                            // Suma wykładników pomniejszona o bias
                            exp_reg  <= $signed({3'b0, exp1})
                                      + $signed({3'b0, exp2})
                                      - 30'sd67108863;
                            sign_reg <= sign1 ^ sign2;
                            status   <= 32'h00000001; // computing
                            state    <= S_CALC;
                        end
                    end
                end

                // ── Oczekiwanie na ustabilizowanie się iloczynu ─
                S_CALC: begin
                    state <= S_NORM;
                end

                // ── Normalizacja i zapis wyniku ──────────────────
                S_NORM: begin
                    if (final_exp_w >= 30'sd134217727) begin
                        // Przepełnienie – wynik: ±Inf
                        res_hi <= {sign_reg, 27'h7FFFFFF, 4'h0};
                        res_lo <= 32'd0;
                        status <= 32'h00000004; // overflow
                    end else if (final_exp_w <= 30'sd0) begin
                        // Niedomiar – wynik: ±0
                        res_hi <= 32'd0;
                        res_lo <= 32'd0;
                        status <= 32'h00000006; // underflow
                    end else begin
                        // Wynik prawidłowy
                        res_hi <= {sign_reg,
                                   final_exp_w[26:0],
                                   final_mant[35:32]};
                        res_lo <= final_mant[31:0];
                        status <= 32'h00000002; // done
                    end
                    state <= S_DONE;
                end

                // ── Wynik gotowy – czekamy na reset CTRL ─────────
                S_DONE: begin
                    if (ctrl[0] == 1'b0) begin
                        state <= S_IDLE;
                    end
                end

            endcase
        end
    end

    // -------------------------------------------------------
    // 3. Odczyt przez procesor (układ kombinacyjny)
    // -------------------------------------------------------
    always @(*) begin
        sdata_out_s = 32'd0;
        case (saddress)
            16'h0100: sdata_out_s = arg1_hi;
            16'h0108: sdata_out_s = arg1_lo;
            16'h00F0: sdata_out_s = arg2_hi;
            16'h00F8: sdata_out_s = arg2_lo;
            16'h00D0: sdata_out_s = ctrl;
            16'h00D8: sdata_out_s = res_hi;
            16'h00E0: sdata_out_s = res_lo;
            16'h00E8: sdata_out_s = status;
        endcase
    end

    assign sdata_out     = sdata_out_s;
    assign gpio_out      = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;

endmodule
/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */
/* verilator lint_off CASEINCOMPLETE */
/* verilator lint_off MULTIDRIVEN */

module gpioemu(
    input            n_reset,
    input  [15:0]    saddress,
    input            srd,
    input            swr,
    input  [31:0]    sdata_in,
    output reg [31:0] sdata_out,
    input  [31:0]    gpio_in,
    input            gpio_latch,
    output [31:0]    gpio_out,
    input            clk,
    output [31:0]    gpio_in_s_insp
);

    // WYMAGANE REJESTRY WEWNĘTRZNE
    reg [31:0] gpio_in_s;
    reg [31:0] gpio_out_s;
    reg [31:0] sdata_in_s;

    // Rejestry projektowe
    reg [31:0] arg1_h, arg1_l;
    reg [31:0] arg2_h, arg2_l;
    reg [31:0] ctrl_reg;
    reg [31:0] status_reg;
    reg [31:0] res_h, res_l;

    // Automat stanów
    reg [1:0] state;
    localparam S_IDLE = 2'd0, S_CALC = 2'd1, S_DONE = 2'd2;

    // Rejestry pomocnicze dla mnożenia
    reg [73:0] prod_reg;
    reg signed [27:0] exp_reg;
    reg sign_reg;

    // BIAS = 2^26 = 67108864
    localparam [26:0] BIAS = 27'd67108864;

    // Wyciągnięcie pól z argumentów (zgodnie z formatem)
    wire [35:0] mant1 = {arg1_h, arg1_l[31:28]};
    wire [35:0] mant2 = {arg2_h, arg2_l[31:28]};
    wire [26:0] exp1  = arg1_l[27:1];
    wire [26:0] exp2  = arg2_l[27:1];
    wire sign1 = arg1_l[0];
    wire sign2 = arg2_l[0];

    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;
    always @(*) sdata_in_s = sdata_in;

    // Zatrzaśnięcie gpio_in
    always @(posedge gpio_latch or negedge n_reset) begin
        if (!n_reset) gpio_in_s <= 0;
        else gpio_in_s <= gpio_in;
    end

    // Zapis z procesora (reakcja na zbocze swr)
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1_h <= 0; arg1_l <= 0;
            arg2_h <= 0; arg2_l <= 0;
            ctrl_reg <= 0;
        end else begin
            case (saddress)
                16'h0100: arg1_h <= sdata_in_s;
                16'h0108: arg1_l <= sdata_in_s;
                16'h00F0: arg2_h <= sdata_in_s;
                16'h00F8: arg2_l <= sdata_in_s;
                16'h00D0: ctrl_reg <= sdata_in_s;
                default: ;
            endcase
        end
    end

    // Pomocnicze sygnały kombinacyjne do normalizacji (nie wewnątrz always!)
    wire signed [27:0] final_exp_signed = exp_reg + (prod_reg[73] ? 28'd1 : 28'd0);
    wire [26:0] new_exp = final_exp_signed[26:0];
    wire [35:0] new_mant = prod_reg[73] ? prod_reg[72:37] : prod_reg[71:36];

    // Automat stanów i obliczenia (reakcja na clk)
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state <= S_IDLE;
            status_reg <= 0;
            res_h <= 0; res_l <= 0;
            gpio_out_s <= 0;
            prod_reg <= 0;
            exp_reg <= 0;
            sign_reg <= 0;
        end else begin
            case (state)
                S_IDLE: begin
                    if (ctrl_reg[0]) begin
                        // Wykryto sygnał startu
                        if ((exp1 == 0 && mant1 == 0) || (exp2 == 0 && mant2 == 0)) begin
                            // Przynajmniej jeden argument zerowy -> wynik zero
                            res_h <= 0;
                            res_l <= 0;
                            status_reg <= 2; // done
                            state <= S_DONE;
                        end else begin
                            // Mnożenie 1.mant1 * 1.mant2
                            prod_reg <= {1'b1, mant1} * {1'b1, mant2};
                            exp_reg  <= $signed({1'b0, exp1}) + $signed({1'b0, exp2}) - $signed({1'b0, BIAS});
                            sign_reg <= sign1 ^ sign2;
                            status_reg <= 1; // busy
                            state <= S_CALC;
                        end
                    end else begin
                        status_reg <= 0; // idle
                    end
                end

                S_CALC: begin
                    // Jeden cykl na ustabilizowanie się wyniku mnożenia
                    state <= S_DONE;
                end

                S_DONE: begin
                    // Normalizacja i zapis wyniku
                    if (final_exp_signed < 0 || final_exp_signed > 28'h7FFFFFF) begin
                        // Przekroczenie zakresu -> zero
                        res_h <= 0;
                        res_l <= 0;
                    end else begin
                        res_h <= new_mant[35:4];
                        res_l <= {new_mant[3:0], new_exp, sign_reg};
                    end
                    status_reg <= 2; // done
                    // Powrót do IDLE dopiero po wyzerowaniu bitu sterującego
                    if (!ctrl_reg[0]) begin
                        state <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    // Odczyt dla procesora (układ kombinacyjny)
    always @(*) begin
        case (saddress)
            16'h0100: sdata_out = arg1_h;
            16'h0108: sdata_out = arg1_l;
            16'h00F0: sdata_out = arg2_h;
            16'h00F8: sdata_out = arg2_l;
            16'h00E8: sdata_out = status_reg;
            16'h00D8: sdata_out = res_h;
            16'h00E0: sdata_out = res_l;
            16'h00D0: sdata_out = ctrl_reg;
            default:  sdata_out = 0;
        endcase
    end

endmodule
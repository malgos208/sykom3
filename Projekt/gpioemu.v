/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */
/* verilator lint_off CASEINCOMPLETE */
/* verilator lint_off MULTIDRIVEN */

module gpioemu(
    input             n_reset,
    input  [15:0]     saddress,
    input             srd,
    input             swr,
    input  [31:0]     sdata_in,
    output reg [31:0] sdata_out,
    input  [31:0]     gpio_in,
    input             gpio_latch,
    output [31:0]     gpio_out,
    input             clk,
    output [31:0]     gpio_in_s_insp
);

    /* =========================
       REJESTRY GPIO / MAGISTRALI
       ========================= */
    reg [31:0] gpio_in_s;
    reg [31:0] gpio_out_s;
    reg [31:0] sdata_in_s;

    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;
    always @(*) sdata_in_s = sdata_in;

    always @(posedge gpio_latch or negedge n_reset) begin
        if (!n_reset)
            gpio_in_s <= 0;
        else
            gpio_in_s <= gpio_in;
    end

    /* =========================
       REJESTRY MAPOWANE W SoC
       ========================= */
    reg [31:0] arg1_h, arg1_l;
    reg [31:0] arg2_h, arg2_l;
    reg [31:0] ctrl_reg;
    reg [31:0] status_reg;
    reg [31:0] res_h, res_l;

    /* =========================
       ZAPIS Z CPU
       ========================= */
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1_h   <= 0; arg1_l <= 0;
            arg2_h   <= 0; arg2_l <= 0;
            ctrl_reg <= 0;
        end else begin
            case (saddress)
                16'h0100: arg1_h   <= sdata_in_s;
                16'h0108: arg1_l   <= sdata_in_s;
                16'h00F0: arg2_h   <= sdata_in_s;
                16'h00F8: arg2_l   <= sdata_in_s;
                16'h00D0: ctrl_reg <= sdata_in_s;
                default: ;
            endcase
        end
    end

    /* =========================
       FSM
       ========================= */
    reg [1:0] state;
    localparam S_IDLE = 2'd0,
               S_CALC = 2'd1,
               S_DONE = 2'd2;

    /* =========================
       REJESTRY ROBOCZE (STABILNE)
       ========================= */
    reg [63:0] arg1_r;
    reg [63:0] arg2_r;

    reg [73:0]        prod_reg;
    reg signed [27:0] exp_reg;
    reg               sign_reg;

    localparam [26:0] BIAS = 27'd67108864; // 2^26

    /* =========================
       WYCIĄGNIĘCIE PÓL (1:1)
       ========================= */
    wire        sign1 = arg1_r[0];
    wire        sign2 = arg2_r[0];
    wire [26:0] exp1  = arg1_r[27:1];
    wire [26:0] exp2  = arg2_r[27:1];
    wire [35:0] mant1 = arg1_r[63:28];
    wire [35:0] mant2 = arg2_r[63:28];

    /* =========================
       NORMALIZACJA
       ========================= */
    wire signed [27:0] final_exp_signed =
        exp_reg + (prod_reg[73] ? 28'd1 : 28'd0);

    wire [26:0] new_exp  = final_exp_signed[26:0];
    wire [35:0] new_mant =
        prod_reg[73] ? prod_reg[72:37]
                      : prod_reg[71:36];

    /* =========================
       FSM + OBLICZENIA
       ========================= */
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state      <= S_IDLE;
            status_reg <= 0;
            res_h      <= 0;
            res_l      <= 0;
            arg1_r     <= 0;
            arg2_r     <= 0;
            prod_reg   <= 0;
            exp_reg    <= 0;
            sign_reg   <= 0;
            gpio_out_s <= 0;
        end else begin
            case (state)

                /* -------------------------
                   IDLE — tylko zatrzaśnięcie
                   ------------------------- */
                S_IDLE: begin
                    status_reg <= 0;
                    if (ctrl_reg[0]) begin
                        arg1_r <= {arg1_h, arg1_l};
                        arg2_r <= {arg2_h, arg2_l};
                        status_reg <= 1; // busy
                        state <= S_CALC;
                    end
                end

                /* -------------------------
                   CALC — faktyczne liczenie
                   ------------------------- */
                S_CALC: begin
                    if (arg1_r == 64'd0 || arg2_r == 64'd0) begin
                        prod_reg <= 0;
                        exp_reg  <= 0;
                        sign_reg <= 0;
                    end else begin
                        prod_reg <= {1'b1, mant1} * {1'b1, mant2};
                        exp_reg  <=
                            $signed({1'b0, exp1}) +
                            $signed({1'b0, exp2}) -
                            $signed({1'b0, BIAS});
                        sign_reg <= sign1 ^ sign2;
                    end
                    state <= S_DONE;
                end

                /* -------------------------
                   DONE — zapis wyniku
                   ------------------------- */
                S_DONE: begin
                    if (final_exp_signed < 0 ||
                        final_exp_signed > 28'h7FFFFFF) begin
                        res_h <= 0;
                        res_l <= 0;
                    end else begin
                        res_h <= new_mant[35:4];
                        res_l <= { new_mant[3:0], new_exp, sign_reg };
                    end
                    status_reg <= 2; // done
                    if (!ctrl_reg[0])
                        state <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    /* =========================
       ODCZYT DLA CPU
       ========================= */
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
            default:  sdata_out = 32'd0;
        endcase
    end

endmodule
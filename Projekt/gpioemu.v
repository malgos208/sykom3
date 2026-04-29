/* verilator lint_off UNUSED */
/* verilator lint_off UNDRIVEN */
/* verilator lint_off MULTIDRIVEN */
/* verilator lint_off COMBDLY */

module gpioemu(
    input           n_reset,
    input  [15:0]   saddress,        // magistrala - adres
    input           srd,             // odczyt przez CPU
    input           swr,             // zapis przez CPU
    input  [31:0]   sdata_in,        // magistrala wejściowa CPU
    output [31:0]   sdata_out,       // magistrala wyjściowa do CPU
    input  [31:0]   gpio_in,
    input           gpio_latch,
    output [31:0]   gpio_out,
    input           clk,
    output [31:0]   gpio_in_s_insp
);

    /* ---------------- Rejestry ogólne ---------------- */

    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;

    /* ---------------- Sterowanie ---------------- */

    reg        ena;          // LATCHED enable
    reg [31:0] control;      // rejestr kontrolny (debug)

    /* ---------------- Dane FP ---------------- */

    reg [63:0] arg1, arg2, result;
    reg [31:0] status;

    reg        sign_r;
    reg [27:0] exp_sum;
    reg [73:0] mant_prod;

    /* ---------------- FSM ---------------- */

    reg [1:0] state, next_state;
    localparam [1:0] IDLE = 2'b00,
                     CALC = 2'b01,
                     DONE = 2'b10;

    /* ---------------- Stałe ---------------- */

    localparam [26:0] BIAS = 27'd67108864;

    /* ---------------- Detekcja zera ---------------- */

    wire arg1_is_zero = (arg1[27:1] == 27'd0) && (arg1[63:28] == 36'd0);
    wire arg2_is_zero = (arg2[27:1] == 27'd0) && (arg2[63:28] == 36'd0);

    /* ---------------- Mantysy rozszerzone ---------------- */

    wire [73:0] mant1_ext = arg1_is_zero ? 74'd0 : {37'd0, 1'b1, arg1[63:28]};
    wire [73:0] mant2_ext = arg2_is_zero ? 74'd0 : {37'd0, 1'b1, arg2[63:28]};

    /* ============================================================
       BLOK 1: ZAPISY Z CPU – ZAWSZE AKTYWNE (niezależne od ena)
       ============================================================ */
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            arg1    <= 64'h0;
            arg2    <= 64'h0;
            control <= 32'h0;
            ena     <= 1'b0;
        end
        else if (swr) begin
            case (saddress)
                16'h0100: arg1[63:32] <= sdata_in;
                16'h0108: arg1[31:0]  <= sdata_in;
                16'h00F0: arg2[63:32] <= sdata_in;
                16'h00F8: arg2[31:0]  <= sdata_in;

                16'h00D0: begin
                    control <= sdata_in;
                    ena     <= sdata_in[0];   // latch enable
                end
                default: ;
            endcase
        end
    end

    /* ============================================================
       BLOK 2: FSM I OPERACJE – AKTYWNE TYLKO GDY ena = 1
       ============================================================ */
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state     <= IDLE;
            status    <= 32'h0;
            result    <= 64'h0;
            sign_r    <= 1'b0;
            exp_sum   <= 28'h0;
            mant_prod <= 74'h0;
            gpio_in_s <= 32'h0;
        end
        else if (ena) begin
            state <= next_state;

            if (gpio_latch)
                gpio_in_s <= gpio_in;

            case (state)
                IDLE: begin
                    status <= 32'h0;
                end

                CALC: begin
                    status    <= 32'h1;
                    mant_prod <= mant1_ext * mant2_ext;
                    exp_sum   <= {1'b0, arg1[27:1]} +
                                 {1'b0, arg2[27:1]} -
                                 {1'b0, BIAS};
                    sign_r    <= arg1[0] ^ arg2[0];
                end

                DONE: begin
                    status <= 32'h2;
                    if (arg1_is_zero || arg2_is_zero) begin
                        result <= 64'h0;
                    end else begin
                        result[0] <= sign_r;
                        if (mant_prod[73]) begin
                            result[27:1]  <= exp_sum[26:0] + 27'd1;
                            result[63:28] <= mant_prod[72:37];
                        end else begin
                            result[27:1]  <= exp_sum[26:0];
                            result[63:28] <= mant_prod[71:36];
                        end
                    end
                end
            endcase
        end
        else begin
            /* wymuszenie IDLE przy ena = 0 */
            state  <= IDLE;
            status <= 32'h0;
        end
    end

    /* ---------------- Logika next_state ---------------- */

    always @(*) begin
        next_state = state;
        case (state)
            IDLE: if (ena) next_state = CALC;
            CALC:          next_state = DONE;
            DONE: if (!ena) next_state = IDLE;
            default:       next_state = IDLE;
        endcase
    end

    /* ---------------- Odczyt rejestrów ---------------- */

    reg [31:0] read_mux;
    always @(*) begin
        case (saddress)
            16'h0100: read_mux = arg1[63:32];
            16'h0108: read_mux = arg1[31:0];
            16'h00F0: read_mux = arg2[63:32];
            16'h00F8: read_mux = arg2[31:0];
            16'h00D0: read_mux = {31'b0, ena};
            16'h00E8: read_mux = status;
            16'h00D8: read_mux = result[63:32];
            16'h00E0: read_mux = result[31:0];
            default:  read_mux = 32'h0;
        endcase
    end

    assign sdata_out = srd ? read_mux : 32'h0;
    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;

endmodule
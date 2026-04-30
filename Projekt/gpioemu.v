/* verilator lint_off UNUSED */
/* verilator lint_off UNDRIVEN */
/* verilator lint_off MULTIDRIVEN */
/* verilator lint_off COMBDLY */

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

    // bufory (wymagane przez narzędzia)
    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;
    assign gpio_out       = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;

    always @(*) sdata_in_s = sdata_in;

    always @(posedge gpio_latch or negedge n_reset) begin
        if (!n_reset) gpio_in_s <= 32'h0;
        else          gpio_in_s <= gpio_in;
    end

    // ========== REJESTRY ARGUMENTÓW I WYNIKU ==========
    reg [63:0] arg1;
    reg [63:0] arg2;
    reg [63:0] result;

    // ========== STEROWANIE ==========
    reg        start;      // żądanie startu z CPU
    reg [31:0] status;     // 0=idle, 1=busy, 2=done

    // ========== FSM ==========
    reg [1:0] state;
    localparam IDLE   = 2'd0;
    localparam CALC   = 2'd1;
    localparam FINISH = 2'd2;

    // ========== PARAMETRY FP ==========
    localparam [26:0] BIAS = 27'd67108864;   // 2^26

    wire arg1_is_zero = (arg1[27:1] == 0) && (arg1[63:28] == 0);
    wire arg2_is_zero = (arg2[27:1] == 0) && (arg2[63:28] == 0);

    wire [73:0] mant1_ext = arg1_is_zero ? 74'd0 : {37'd0, 1'b1, arg1[63:28]};
    wire [73:0] mant2_ext = arg2_is_zero ? 74'd0 : {37'd0, 1'b1, arg2[63:28]};

    reg        sign_r;
    reg [27:0] exp_sum;
    reg [73:0] mant_prod;

    // ========== ZAPIS Z CPU ==========
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1  <= 64'h0;
            arg2  <= 64'h0;
            start <= 1'b0;
        end else begin
            case (saddress)
                16'h0100: arg1[63:32] <= sdata_in_s;
                16'h0108: arg1[31:0]  <= sdata_in_s;
                16'h00F0: arg2[63:32] <= sdata_in_s;
                16'h00F8: arg2[31:0]  <= sdata_in_s;
                16'h00D0: start       <= sdata_in_s[0];   // zapis 1 -> start
                default: ;
            endcase
        end
    end

    // ========== FSM (bez ena) ==========
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state      <= IDLE;
            status     <= 32'h0;
            result     <= 64'h0;
            gpio_out_s <= 32'h0;
        end else begin
            case (state)
                IDLE: begin
                    // jeśli pojawi się żądanie startu
                    if (start) begin
                        start <= 1'b0;        // skasuj żądanie (jednorazowe uruchomienie)
                        state <= CALC;
                        // status pozostaje 0 (idle) – zmieni się w CALC
                    end
                end

                CALC: begin
                    status    <= 32'h1;        // busy
                    mant_prod <= mant1_ext * mant2_ext;
                    exp_sum   <= {1'b0, arg1[27:1]} +
                                 {1'b0, arg2[27:1]} -
                                 {1'b0, BIAS};
                    sign_r    <= arg1[0] ^ arg2[0];
                    state     <= FINISH;
                end

                FINISH: begin
                    // obliczenie wyniku
                    if (arg1_is_zero || arg2_is_zero)
                        result <= 64'h0;
                    else if (mant_prod[73]) begin
                        result[0]      <= sign_r;
                        result[27:1]   <= exp_sum[26:0] + 1;
                        result[63:28]  <= mant_prod[72:37];
                    end else begin
                        result[0]      <= sign_r;
                        result[27:1]   <= exp_sum[26:0];
                        result[63:28]  <= mant_prod[71:36];
                    end
                    status <= 32'h2;         // done
                    state  <= IDLE;          // powrót do oczekiwania na kolejny start
                end

                default: state <= IDLE;
            endcase
        end
    end

    // ========== ODCZYT Z CPU ==========
    always @(posedge srd or negedge n_reset) begin
        if (!n_reset)
            sdata_out <= 32'h0;
        else begin
            case (saddress)
                16'h0100: sdata_out <= arg1[63:32];
                16'h0108: sdata_out <= arg1[31:0];
                16'h00F0: sdata_out <= arg2[63:32];
                16'h00F8: sdata_out <= arg2[31:0];
                16'h00E8: sdata_out <= status;
                16'h00D8: sdata_out <= result[63:32];
                16'h00E0: sdata_out <= result[31:0];
                default:  sdata_out <= 32'h0;
            endcase
        end
    end

endmodule
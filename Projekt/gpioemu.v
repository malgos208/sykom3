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

    // bufory wymagane
    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;

    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;
    always @(*) sdata_in_s = sdata_in;
    always @(posedge gpio_latch or negedge n_reset)
        if (!n_reset) gpio_in_s <= 0;
        else gpio_in_s <= gpio_in;

    // rejestry argumentów i wyniku
    reg [63:0] arg1, arg2;
    reg [63:0] result;
    reg [31:0] status;   // 0=idle, 1=busy, 2=done
    reg        ctrl;     // bit0 rejestru sterującego

    // parametry FP (Twojego formatu)
    localparam [26:0] BIAS = 27'd67108864;   // 2^26
    wire arg1_zero = (arg1[27:1] == 0) && (arg1[63:28] == 0);
    wire arg2_zero = (arg2[27:1] == 0) && (arg2[63:28] == 0);
    wire [73:0] mant1 = arg1_zero ? 74'd0 : {37'd0, 1'b1, arg1[63:28]};
    wire [73:0] mant2 = arg2_zero ? 74'd0 : {37'd0, 1'b1, arg2[63:28]};

    // Zapis argumentów i rejestru sterującego (synchronicznie z swr)
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1 <= 0;
            arg2 <= 0;
            ctrl <= 0;
        end else begin
            case (saddress)
                16'h0100: arg1[63:32] <= sdata_in_s;
                16'h0108: arg1[31:0]  <= sdata_in_s;
                16'h00F0: arg2[63:32] <= sdata_in_s;
                16'h00F8: arg2[31:0]  <= sdata_in_s;
                16'h00D0: ctrl <= sdata_in_s[0];
            endcase
        end
    end

    // FSM taktowany asynchronicznym clk (1 kHz) – styl kolegi
    reg [1:0] state;
    localparam S_IDLE = 2'd0, S_CALC = 2'd1, S_DONE = 2'd2;

    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state <= S_IDLE;
            status <= 0;
            result <= 0;
        end else begin
            case (state)
                S_IDLE: begin
                    if (ctrl) begin
                        status <= 1;   // busy
                        // wykonaj mnożenie (kombinacyjne)
                        if (arg1_zero || arg2_zero)
                            result <= 0;
                        else begin
                            wire [73:0] prod = mant1 * mant2;
                            wire [27:0] exp_sum = {1'b0, arg1[27:1]} + {1'b0, arg2[27:1]} - {1'b0, BIAS};
                            wire sign = arg1[0] ^ arg2[0];
                            if (prod[73])
                                result <= {prod[72:37], (exp_sum[26:0] + 27'b1), sign};
                            else
                                result <= {prod[71:36], exp_sum[26:0], sign};
                        end
                        state <= S_DONE;
                    end
                end
                S_DONE: begin
                    status <= 2;   // done
                    if (!ctrl) begin
                        state <= S_IDLE;
                        status <= 0;
                        result <= 0;
                    end
                end
                default: state <= S_IDLE;
            endcase
        end
    end

    // Odczyt z CPU
    always @(posedge srd or negedge n_reset) begin
        if (!n_reset)
            sdata_out <= 0;
        else begin
            case (saddress)
                16'h0100: sdata_out <= arg1[63:32];
                16'h0108: sdata_out <= arg1[31:0];
                16'h00F0: sdata_out <= arg2[63:32];
                16'h00F8: sdata_out <= arg2[31:0];
                16'h00E8: sdata_out <= status;
                16'h00D8: sdata_out <= result[63:32];
                16'h00E0: sdata_out <= result[31:0];
                default:  sdata_out <= 0;
            endcase
        end
    end

endmodule
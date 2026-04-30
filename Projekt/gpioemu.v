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

    // ---------- bufory ----------
    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;
    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;
    always @(*) sdata_in_s = sdata_in;
    always @(posedge gpio_latch or negedge n_reset)
        if (!n_reset) gpio_in_s <= 0;
        else gpio_in_s <= gpio_in;

    // ---------- rejestry argumentów ----------
    reg [63:0] arg1, arg2;

    // ---------- sygnały start i reset z CPU ----------
    reg start_meta, reset_meta;
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            start_meta <= 0;
            reset_meta <= 0;
        end else if (saddress == 16'h00D0) begin
            start_meta <= sdata_in_s[0];
            reset_meta <= ~sdata_in_s[0];   // zapis 0 -> reset
        end
    end

    // ---------- synchronizacja do domeny clk ----------
    reg start_sync, start_sync_d, reset_sync, reset_sync_d;
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            start_sync <= 0; start_sync_d <= 0;
            reset_sync <= 0; reset_sync_d <= 0;
        end else begin
            start_sync <= start_meta; start_sync_d <= start_sync;
            reset_sync <= reset_meta; reset_sync_d <= reset_sync;
        end
    end
    wire start_trigger = start_sync && !start_sync_d;
    wire reset_trigger = reset_sync && !reset_sync_d;

    // ---------- FSM ----------
    reg [1:0] state;
    reg [31:0] status;   // 0=idle, 1=busy, 2=done
    reg [63:0] result;

    localparam IDLE = 2'd0, CALC = 2'd1, DONE = 2'd2;

    // parametry FP
    localparam [26:0] BIAS = 27'd67108864;

    wire arg1_zero = (arg1[27:1] == 0) && (arg1[63:28] == 0);
    wire arg2_zero = (arg2[27:1] == 0) && (arg2[63:28] == 0);
    wire [73:0] mant1 = arg1_zero ? 74'd0 : {37'd0, 1'b1, arg1[63:28]};
    wire [73:0] mant2 = arg2_zero ? 74'd0 : {37'd0, 1'b1, arg2[63:28]};

    reg [73:0] mant_prod;
    reg [27:0] exp_sum;
    reg sign;

    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state <= IDLE;
            status <= 0;
            result <= 0;
            mant_prod <= 0;
            exp_sum <= 0;
            sign <= 0;
        end else begin
            case (state)
                IDLE: begin
                    if (reset_trigger) begin
                        status <= 0;
                        result <= 0;
                    end else if (start_trigger) begin
                        state <= CALC;
                        status <= 1;   // busy
                    end else begin
                        status <= 0;   // idle
                    end
                end
                CALC: begin
                    mant_prod <= mant1 * mant2;
                    exp_sum   <= {1'b0, arg1[27:1]} + {1'b0, arg2[27:1]} - {1'b0, BIAS};
                    sign      <= arg1[0] ^ arg2[0];
                    state     <= DONE;
                end
                DONE: begin
                    if (arg1_zero || arg2_zero)
                        result <= 0;
                    else if (mant_prod[73])
                        result <= {mant_prod[72:37], (exp_sum[26:0] + 27'b1), sign};
                    else
                        result <= {mant_prod[71:36], exp_sum[26:0], sign};
                    status <= 2;   // done
                    state  <= IDLE;
                end
                default: state <= IDLE;
            endcase
        end
    end

    // ---------- zapis argumentów ----------
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1 <= 0; arg2 <= 0;
        end else begin
            case (saddress)
                16'h0100: arg1[63:32] <= sdata_in_s;
                16'h0108: arg1[31:0]  <= sdata_in_s;
                16'h00F0: arg2[63:32] <= sdata_in_s;
                16'h00F8: arg2[31:0]  <= sdata_in_s;
                default: ;
            endcase
        end
    end

    // ---------- odczyt ----------
    always @(posedge srd or negedge n_reset) begin
        if (!n_reset) sdata_out <= 0;
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
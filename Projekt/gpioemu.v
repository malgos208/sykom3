/* verilator lint_off UNUSED */
/* verilator lint_off UNDRIVEN */
/* verilator lint_off MULTIDRIVEN */
/* verilator lint_off COMBDLY */

module gpioemu(
    input           n_reset,
    input           clk,

    input  [15:0]   saddress,
    input           srd,
    input           swr,
    input  [31:0]   sdata_in,
    output reg [31:0] sdata_out,

    input  [31:0]   gpio_in,
    input           gpio_latch,
    output [31:0]   gpio_out,

    output [31:0]   gpio_in_s_insp
);

    /* ---------------- Magistrala / debug ---------------- */

    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;

    always @(*) begin
        sdata_in_s = sdata_in;
    end

    /* ---------------- Sterowanie ---------------- */

    reg ena;   // dokładnie jak u kolegi

    /* ---------------- Dane FP ---------------- */

    reg [63:0] arg1, arg2, result;
    reg [31:0] status;

    reg        sign_r;
    reg [27:0] exp_sum;
    reg [73:0] mant_prod;

    /* ---------------- FSM ---------------- */

    reg [1:0] state, state_next;
    localparam [1:0]
        IDLE = 2'd0,
        CALC = 2'd1,
        DONE = 2'd2;

    localparam [26:0] BIAS = 27'd67108864;

    /* ---------------- Detekcja zera ---------------- */

    wire arg1_is_zero = (arg1[27:1] == 0) && (arg1[63:28] == 0);
    wire arg2_is_zero = (arg2[27:1] == 0) && (arg2[63:28] == 0);

    wire [73:0] mant1_ext = arg1_is_zero ? 74'd0 : {37'd0, 1'b1, arg1[63:28]};
    wire [73:0] mant2_ext = arg2_is_zero ? 74'd0 : {37'd0, 1'b1, arg2[63:28]};

    /* ======================================================
       ZAPISY Z CPU – IDENTYCZNIE JAK U KOLEGI
       ====================================================== */
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            arg1 <= 64'h0;
            arg2 <= 64'h0;
            ena  <= 1'b0;
        end else if (swr) begin
            case (saddress)
                16'h0100: arg1[63:32] <= sdata_in_s;
                16'h0108: arg1[31:0]  <= sdata_in_s;
                16'h00F0: arg2[63:32] <= sdata_in_s;
                16'h00F8: arg2[31:0]  <= sdata_in_s;
                16'h00D0: ena         <= sdata_in_s[0];
                default: ;
            endcase
        end
    end

    /* ======================================================
       FSM – BRAMKOWANY PRZEZ ena (1:1 jak u kolegi)
       ====================================================== */
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state     <= IDLE;
            status    <= 32'h0;
            result    <= 64'h0;
            mant_prod <= 74'h0;
            exp_sum   <= 28'h0;
            sign_r    <= 1'b0;
            gpio_in_s <= 32'h0;
        end else if (ena) begin
            state <= state_next;

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
                    if (arg1_is_zero || arg2_is_zero)
                        result <= 64'h0;
                    else begin
                        result[0] <= sign_r;
                        if (mant_prod[73]) begin
                            result[27:1]  <= exp_sum[26:0] + 1;
                            result[63:28] <= mant_prod[72:37];
                        end else begin
                            result[27:1]  <= exp_sum[26:0];
                            result[63:28] <= mant_prod[71:36];
                        end
                    end
                end

                default: begin
                    state  <= IDLE;
                    status <= 32'h0;
                end
            endcase
        end else begin
            state  <= IDLE;
            status <= 32'h0;
        end
    end

    /* ---------------- Next state – jak u kolegi ---------------- */

    always @(*) begin
        state_next = state;
        case (state)
            IDLE:    state_next = CALC;
            CALC:    state_next = DONE;
            DONE:    if (!ena) state_next = IDLE; // STICKY DONE
            default: state_next = IDLE;
        endcase
    end

    /* ---------------- Odczyt CPU ---------------- */

    always @(*) begin
        if (srd) begin
            case (saddress)
                16'h0100: sdata_out = arg1[63:32];
                16'h0108: sdata_out = arg1[31:0];
                16'h00F0: sdata_out = arg2[63:32];
                16'h00F8: sdata_out = arg2[31:0];
                16'h00D0: sdata_out = {31'b0, ena};
                16'h00E8: sdata_out = status;
                16'h00D8: sdata_out = result[63:32];
                16'h00E0: sdata_out = result[31:0];
                default:  sdata_out = 32'h0;
            endcase
        end else begin
            sdata_out = 32'h0;
        end
    end

    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;

endmodule
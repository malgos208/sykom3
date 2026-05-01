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

    reg [31:0] a1_h, a1_l, a2_h, a2_l;
    reg [31:0] res_h, res_l;
    reg [31:0] status;
    reg ctrl, ctrl_prev;

    assign gpio_out = 0;
    assign gpio_in_s_insp = 0;

    wire [63:0] arg1 = {a1_h, a1_l};
    wire [63:0] arg2 = {a2_h, a2_l};

    localparam BIAS = 27'd67108864;

    wire sign = arg1[0] ^ arg2[0];
    // 1 bit + 35 bitów = 36 bitów
    wire [35:0] mant1 = {1'b1, arg1[63:29]};
    wire [35:0] mant2 = {1'b1, arg2[63:29]};

    wire [71:0] mant_mul = mant1 * mant2;
    wire [26:0] exp1 = arg1[27:1];
    wire [26:0] exp2 = arg2[27:1];

    wire [27:0] exp_sum = {1'b0, exp1} + {1'b0, exp2} - BIAS;

    // FSM
    reg [1:0] state;
    localparam IDLE=0, CALC=1, DONE=2;

    // WRITE (SYNC!)
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            a1_h<=0; a1_l<=0; a2_h<=0; a2_l<=0;
            ctrl<=0;
        end else if (swr) begin
            case(saddress)
                16'h0100: a1_h <= sdata_in;
                16'h0108: a1_l <= sdata_in;
                16'h00F0: a2_h <= sdata_in;
                16'h00F8: a2_l <= sdata_in;
                16'h00D0: ctrl <= sdata_in[0];
                default: ;
            endcase
        end
    end

    // FSM LOGIC
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state<=IDLE;
            status<=0;
            res_h<=0;
            res_l<=0;
            ctrl_prev<=0;
        end else begin
            ctrl_prev <= ctrl;

            case(state)
                IDLE: begin
                    status <= 0;
                    if (ctrl && !ctrl_prev) begin // rising edge
                        status <= 1;
                        state <= CALC;
                    end
                end

                CALC: begin
                    if (exp1 == 27'd0 || exp2 == 27'd0) begin
                        {res_h, res_l} <= 64'd0;
                    end else if (mant_mul[71]) begin
                        // 35 bitów mantysy [70:36] + 28 bitów exp + 1 bit znaku = 64 bity
                        {res_h, res_l} <= {
                            mant_mul[70:36],
                            (exp_sum + 28'd1),
                            sign
                        };
                    end else begin
                        // 35 bitów mantysy [69:35] + 28 bitów exp + 1 bit znaku = 64 bity
                        {res_h, res_l} <= {
                            mant_mul[69:35],
                            exp_sum,
                            sign
                        };
                    end
                    state <= DONE;
                end

                DONE: begin
                    status <= 32'd2;
                    if (!ctrl)
                        state <= IDLE;
                end
                
                default: state <= IDLE;
            endcase
        end
    end

    // READ (ASYNC)
    always @(*) begin
        case(saddress)
            16'h0100: sdata_out = a1_h;
            16'h0108: sdata_out = a1_l;
            16'h00F0: sdata_out = a2_h;
            16'h00F8: sdata_out = a2_l;
            16'h00E8: sdata_out = status;
            16'h00D8: sdata_out = res_h;
            16'h00E0: sdata_out = res_l;
            16'h00D0: sdata_out = {31'd0, ctrl};
            default:  sdata_out = 32'd0;
        endcase
    end

endmodule
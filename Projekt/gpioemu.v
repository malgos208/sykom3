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

    // WYMAGANE REJESTRY WEWNETRZNE
    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;

    // Rejestry projektowe
    reg [31:0] a1_h, a1_l, a2_h, a2_l;
    reg [31:0] res_h, res_l;
    reg [31:0] status;
    reg        ctrl_bit;

    // Przypisania magistral i GPIO
    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;
    always @(*) sdata_in_s = sdata_in;

    // Obsługa latchowania GPIO[cite: 5]
    always @(posedge gpio_latch or negedge n_reset) begin
        if (!n_reset) gpio_in_s <= 0;
        else gpio_in_s <= gpio_in;
    end

    // Logika FP (Format: Mant[63:28], Exp[27:1], Sign[0])
    wire [63:0] arg1 = {a1_h, a1_l};
    wire [63:0] arg2 = {a2_h, a2_l};
    
    localparam [26:0] BIAS = 27'd67108864; // 2^26
    wire [73:0] mant_prod = {1'b1, arg1[63:28]} * {1'b1, arg2[63:28]};
    wire [27:0] exp_sum   = {1'b0, arg1[27:1]} + {1'b0, arg2[27:1]} - {1'b0, BIAS};

    // FSM
    reg [1:0] state;
    localparam IDLE = 2'd0, CALC = 2'd1, DONE = 2'd2;

    // Zapis z CPU (swr)
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            a1_h <= 0; a1_l <= 0; a2_h <= 0; a2_l <= 0; ctrl_bit <= 0;
        end else begin
            case (saddress)
                16'h0100: a1_h <= sdata_in_s;
                16'h0108: a1_l <= sdata_in_s;
                16'h00F0: a2_h <= sdata_in_s;
                16'h00F8: a2_l <= sdata_in_s;
                16'h00D0: ctrl_bit <= sdata_in_s[0];
            endcase
        end
    end

    // Automat stanów (clk)
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state <= IDLE; status <= 0; res_h <= 0; res_l <= 0;
            gpio_out_s <= 0;
        end else begin
            case (state)
                IDLE: begin
                    if (ctrl_bit) begin
                        status <= 1; // busy
                        state  <= CALC;
                    end else status <= 0;
                end
                CALC: begin
                    // Mnożenie z normalizacją
                    if (arg1[27:1] == 0 || arg2[27:1] == 0) begin
                        {res_h, res_l} <= 0;
                    end else if (mant_prod[73]) begin
                        {res_h, res_l} <= {mant_prod[72:37], (exp_sum[26:0] + 27'd1), (arg1[0] ^ arg2[0])};
                    end else begin
                        {res_h, res_l} <= {mant_prod[71:36], exp_sum[26:0], (arg1[0] ^ arg2[0])};
                    end
                    state <= DONE;
                end
                DONE: begin
                    status <= 2; // done
                    if (!ctrl_bit) state <= IDLE;
                end
            endcase
        end
    end

    // Odczyt do CPU (srd)
    always @(*) begin
        case (saddress)
            16'h0100: sdata_out = a1_h;
            16'h0108: sdata_out = a1_l;
            16'h00F0: sdata_out = a2_h;
            16'h00F8: sdata_out = a2_l;
            16'h00E8: sdata_out = status;
            16'h00D8: sdata_out = res_h;
            16'h00E0: sdata_out = res_l;
            16'h00D0: sdata_out = {31'd0, ctrl_bit};
            default:  sdata_out = 0;
        endcase
    end

endmodule
/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */
/* verilator lint_off CASEINCOMPLETE */
/* verilator lint_off MULTIDRIVEN */

module gpioemu(n_reset, saddress, srd, swr, sdata_in, sdata_out,
               gpio_in, gpio_latch, gpio_out, clk, gpio_in_s_insp);

    input n_reset;
    input [15:0] saddress;
    input srd;
    input swr;
    input [31:0] sdata_in;
    output [31:0] sdata_out;
    
    input [31:0] gpio_in;
    input gpio_latch;
    output [31:0] gpio_out;
    input clk;
    output [31:0] gpio_in_s_insp;

    reg [31:0] gpio_out_s;
    reg [31:0] gpio_in_s;
    reg [31:0] sdata_out_s; 

    // REJESTRY PROJEKTOWE
    reg [31:0] arg1_lo, arg1_hi;
    reg [31:0] arg2_lo, arg2_hi;
    reg [31:0] ctrl;
    reg [31:0] status;
    reg [31:0] res_lo, res_hi;

    wire [9:0]  exp1   = arg1_hi[31:22];
    wire        sign1  = arg1_hi[21];
    wire [52:0] mant1  = {arg1_hi[20:0], arg1_lo};

    wire [9:0]  exp2   = arg2_hi[31:22];
    wire        sign2  = arg2_hi[21];
    wire [52:0] mant2  = {arg2_hi[20:0], arg2_lo};

   localparam S_IDLE  = 3'd0;
   localparam S_CALC  = 3'd1;
   localparam S_NORM  = 3'd2;
   localparam S_DONE  = 3'd3;
   reg [2:0] state;

   reg [107:0] prod_reg;
   reg signed [12:0] exp_reg;      
   reg sign_reg;

   wire signed [12:0] final_exp = (prod_reg[107] == 1'b1) ? (exp_reg + 1) : exp_reg;
   wire [52:0] final_mant = (prod_reg[107] == 1'b1) ? prod_reg[106:54] : prod_reg[105:53];

    // ---------------------------------------------------------
    // 1. ZAPIS Z PROCESORA (Reaguje na impuls swr)
    // ---------------------------------------------------------
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1_lo <= 0; arg1_hi <= 0;
            arg2_lo <= 0; arg2_hi <= 0;
            ctrl <= 0;
        end else begin
            case (saddress)
                16'h0180: arg1_lo <= sdata_in;
                16'h0188: arg1_hi <= sdata_in;
                16'h0190: arg2_lo <= sdata_in;
                16'h0198: arg2_hi <= sdata_in;
                16'h01B8: ctrl    <= sdata_in;
            endcase
        end
    end

    // ---------------------------------------------------------
    // 2. AUTOMAT STANÓW I OBLICZENIA (Reaguje na zegar clk 1KHz)
    // ---------------------------------------------------------
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            status <= 0;
            res_lo <= 0; res_hi <= 0;
            state <= S_IDLE;
            gpio_out_s <= 0;
            gpio_in_s <= 0;
            prod_reg <= 0;
            exp_reg <= 0;
            sign_reg <= 0;
        end else begin
            if (gpio_latch) begin
                gpio_in_s <= gpio_in;
            end

            case (state)
                S_IDLE: begin
                    if (ctrl[0] == 1'b1) begin
                        status <= 32'h00000001;
                        if ((exp1 == 0 && mant1 == 0) || (exp2 == 0 && mant2 == 0)) begin
                            res_hi <= 0; res_lo <= 0;
                            status <= 32'h00000002;
                            state <= S_DONE;
                        end else begin
                            prod_reg <= {1'b1, mant1} * {1'b1, mant2};
                            exp_reg <= $signed({3'b0, exp1}) + $signed({3'b0, exp2}) - 13'sd511;
                            sign_reg <= sign1 ^ sign2;
                            state <= S_CALC;
                        end
                    end
                end

                S_CALC: begin
                    state <= S_NORM;
                end

                S_NORM: begin
                    if (final_exp > 1022) begin
                        res_hi <= {10'h3FF, sign_reg, 21'd0}; 
                        res_lo <= 32'd0;
                        status <= 32'h00000004; 
                    end
                    else if (final_exp <= 0) begin
                        res_hi <= 32'd0;
                        res_lo <= 32'd0;
                        status <= 32'h00000006; 
                    end
                    else begin
                        res_hi <= {final_exp[9:0], sign_reg, final_mant[52:32]};
                        res_lo <= final_mant[31:0];
                        status <= 32'h00000002; 
                    end
                    state <= S_DONE;
                end

                S_DONE: begin
                    if (ctrl[0] == 1'b0) begin
                        state <= S_IDLE;
                    end
                end
            endcase
        end
    end

    // ---------------------------------------------------------
    // 3. ODCZYT PRZEZ PROCESOR (Układ kombinacyjny)
    // ---------------------------------------------------------
    always @(*) begin
        sdata_out_s = 32'd0; 
        case (saddress)
            16'h0180: sdata_out_s = arg1_lo;
            16'h0188: sdata_out_s = arg1_hi;
            16'h0190: sdata_out_s = arg2_lo;
            16'h0198: sdata_out_s = arg2_hi;
            16'h01A0: sdata_out_s = status; // Zgodnie ze specyfikacją
            // 16'h0140: sdata_out_s = status; // Zgodnie ze specyfikacją
            16'h01A8: sdata_out_s = res_lo;
            16'h01B0: sdata_out_s = res_hi;
            16'h01B8: sdata_out_s = ctrl;
        endcase
    end

    assign sdata_out = sdata_out_s;
    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;

endmodule
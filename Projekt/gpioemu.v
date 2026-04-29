/* verilator lint_off UNUSED */
/* verilator lint_off UNDRIVEN */
/* verilator lint_off MULTIDRIVEN */
/* verilator lint_off COMBDLY */

module gpioemu(
    input           n_reset,
    input  [15:0]   saddress,        // magistrala - adres
    input           srd,             // odczyt przez CPU z magistrali danych
    input           swr,             // zapis przez CPU do magistrali danych
    input  [31:0]   sdata_in,        // magistrala wejściowa CPU
    output [31:0]   sdata_out,       // magistrala wyjściowa do CPU
    input  [31:0]   gpio_in,         // dane z peryferii wejście do modułu
    input           gpio_latch,      // zapis danych na gpio_in
    output [31:0]   gpio_out,        // dane wyjściowe do peryferii
    input           clk,
    output [31:0]   gpio_in_s_insp   // debugging
);

    reg [31:0] gpio_in_s   /* verilator public_flat_rw */;
    reg [31:0] gpio_out_s  /* verilator public_flat_rw */;
    reg [31:0] sdata_in_s  /* verilator public_flat_rw */;

    reg [63:0] arg1, arg2, result;
    reg [31:0] control, status;

    reg        sign_r;
    reg [27:0] exp_sum;
    reg [73:0] mant_prod;

    reg [1:0] state, next_state;
    localparam [1:0] IDLE = 2'b00, CALC = 2'b01, DONE = 2'b10;

    // BIAS = 2^(27-1) = 2^26 = 67108864
    localparam [26:0] BIAS = 27'd67108864;

    // Wykrywanie zera: E=0 i M=0 (specjalny wzorzec)
    wire arg1_is_zero = (arg1[27:1] == 27'd0) && (arg1[63:28] == 36'd0);
    wire arg2_is_zero = (arg2[27:1] == 27'd0) && (arg2[63:28] == 36'd0);

    // Rozszerzone mantysy (74 bity dla zgodności z przypisaniem)
    wire [73:0] mant1_ext = arg1_is_zero ? 74'd0 : {37'd0, 1'b1, arg1[63:28]};
    wire [73:0] mant2_ext = arg2_is_zero ? 74'd0 : {37'd0, 1'b1, arg2[63:28]};

    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state      <= IDLE;
            status     <= 32'h0;
            control    <= 32'h0;
            arg1       <= 64'h0;
            arg2       <= 64'h0;
            result     <= 64'h0;
            gpio_in_s  <= 32'h0;
            gpio_out_s <= 32'h0;
            sdata_in_s <= 32'h0;
            sign_r     <= 1'b0;
            exp_sum    <= 28'h0;
            mant_prod  <= 74'h0;
        end else begin
            state <= next_state;

            if (gpio_latch) begin
                gpio_in_s  <= gpio_in;
                sdata_in_s <= sdata_in;
            end

            if (swr) begin
                case (saddress)
                    16'h0100: arg1[63:32] <= sdata_in;
                    16'h0108: arg1[31:0]  <= sdata_in;
                    16'h00F0: arg2[63:32] <= sdata_in;
                    16'h00F8: arg2[31:0]  <= sdata_in;
                    16'h00D0: control <= sdata_in;
                    default:  ;
                endcase
            end

            // Wszystkie możliwe stany pokryte (poprawka CASEINCOMPLETE)
            case (next_state)
                IDLE: begin
                    status <= 32'h0;
                end
                CALC: begin
                    status <= 32'h1;  // BUSY
                    mant_prod <= mant1_ext * mant2_ext;
                    exp_sum   <= {1'b0, arg1[27:1]} + {1'b0, arg2[27:1]} - {1'b0, BIAS};
                    sign_r    <= arg1[0] ^ arg2[0];
                end
                DONE: begin
                    status <= 32'h2;  // DONE
                    if (arg1_is_zero || arg2_is_zero) begin
                        result <= 64'h0;  // Wynik = 0.0
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
                default: begin
                    // Pokrycie nieużywanych stanów (2'b11)
                    status <= 32'h0;
                end
            endcase
        end
    end

    always @(*) begin
        next_state = state;
        case (state)
            IDLE: if (control == 32'h1) next_state = CALC;
            CALC: next_state = DONE;
            DONE: begin
                if (control == 32'h0)      next_state = IDLE;
                else if (control == 32'h1) next_state = CALC;
            end
            default: next_state = IDLE;
        endcase
    end

    reg [31:0] read_mux;
    always @(*) begin
        case (saddress)
            16'h0100: read_mux = arg1[63:32];
            16'h0108: read_mux = arg1[31:0];
            16'h00F0: read_mux = arg2[63:32];
            16'h00F8: read_mux = arg2[31:0];
            16'h00D0: read_mux = control;
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
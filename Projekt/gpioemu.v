/* verilator lint_off WIDTH */
/* verilator lint_off UNUSED */ // żeby nie zgłaszał ostrzeżeń o nieużywanych sygnałach (m.in. gpio_in_s_insp do testów)
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

    // wymagane rejestry wewnętrzne GPIO (odpowiednio zatrzaskujące wejście i buforujące wyjście)
    reg [31:0] gpio_in_s;
    reg [31:0] gpio_out_s;

    assign gpio_out = gpio_out_s;
    assign gpio_in_s_insp = gpio_in_s;

    // Rejestry ze specyfikacji (adresowane przez procesor)
    reg [31:0] arg1_h, arg1_l;
    reg [31:0] arg2_h, arg2_l;
    reg [31:0] ctrl_reg;
    reg [31:0] status_reg;
    reg [31:0] result_h, result_l;

    // Automat stanów
    reg [1:0] state;
    localparam S_IDLE  = 2'd0, S_CALC  = 2'd1, S_WRITE = 2'd2, S_DONE  = 2'd3;

    // Zatrzaskiwane argumenty
    reg [63:0] arg1_reg;
    reg [63:0] arg2_reg;

    // Format:
    // bit [0] – znak
    // bits[27:1] – wykładnik binarny (27 b, BIAS = 2^26)
    // bits[63:28] – mantysa bez wiodącej jedynki

    wire s1 = arg1_reg[0];
    wire s2 = arg2_reg[0];

    wire [26:0] e1 = arg1_reg[27:1];
    wire [26:0] e2 = arg2_reg[27:1];

    wire [35:0] m1 = arg1_reg[63:28];
    wire [35:0] m2 = arg2_reg[63:28];

    localparam [26:0] BIAS = 27'd67108864; // 2^26

    // Rejestry pomocnicze dla mnożenia
    reg [73:0] m_reg;
    reg signed [27:0] e_reg;
    reg s_reg;
    
    wire arg1_is_zero = (e1 == 0) && (m1 == 0);
    wire arg2_is_zero = (e2 == 0) && (m2 == 0);
    wire [35:0] m_final = m_reg[73] ? m_reg[72:37] : m_reg[71:36];
    wire signed [27:0] e_final = e_reg + (m_reg[73] ? 1 : 0);

    // Zatrzaśnięcie gpio_in (wymaganie)
    always @(posedge gpio_latch or negedge n_reset) begin
        if (!n_reset)
            gpio_in_s <= 32'd0;
        else
            gpio_in_s <= gpio_in;
    end

    // Zapis danych z procesora
    always @(posedge swr or negedge n_reset) begin
        if (!n_reset) begin
            arg1_h <= 0; arg1_l <= 0;
            arg2_h <= 0; arg2_l <= 0;
            ctrl_reg <= 0;
        end else begin
            case (saddress)
                16'h0100: arg1_h <= sdata_in;
                16'h0108: arg1_l <= sdata_in;
                16'h00F0: arg2_h <= sdata_in;
                16'h00F8: arg2_l <= sdata_in;
                16'h00D0: ctrl_reg <= sdata_in;
                default: ;
            endcase
        end
    end

    // Odczyt danych przez procesor
    // always @(*) begin
    //     case (saddress)
    //         16'h0100: sdata_out = arg1_h;
    //         16'h0108: sdata_out = arg1_l;
    //         16'h00F0: sdata_out = arg2_h;
    //         16'h00F8: sdata_out = arg2_l;
    //         16'h00E8: sdata_out = status_reg;
    //         16'h00D8: sdata_out = result_h;
    //         16'h00E0: sdata_out = result_l;
    //         16'h00D0: sdata_out = ctrl_reg;
    //         default:  sdata_out = 32'd0;
    //     endcase
    // end
    always @(posedge srd or negedge n_reset) begin
        if (!n_reset) begin
            sdata_out <= 32'd0;
        end else begin
            case (saddress)
                16'h0100: sdata_out <= arg1_h;
                16'h0108: sdata_out <= arg1_l;
                16'h00F0: sdata_out <= arg2_h;
                16'h00F8: sdata_out <= arg2_l;
                16'h00E8: sdata_out <= status_reg;
                16'h00D8: sdata_out <= result_h;
                16'h00E0: sdata_out <= result_l;
                16'h00D0: sdata_out <= ctrl_reg;
                default:  sdata_out <= 32'd0;
            endcase
        end
    end
    // always @(*) begin
    //     if (!n_reset)
    //         sdata_out = 32'd0; // reset magistrali wyjściowej
    //     else if (srd) begin
    //         case (saddress)
    //             16'h0100: sdata_out = arg1_h;
    //             16'h0108: sdata_out = arg1_l;
    //             16'h00F0: sdata_out = arg2_h;
    //             16'h00F8: sdata_out = arg2_l;
    //             16'h00D0: sdata_out = ctrl_reg;
    //             16'h00E8: sdata_out = status_reg;
    //             16'h00D8: sdata_out = result_h;
    //             16'h00E0: sdata_out = result_l;
    //             default:  sdata_out = 32'd0;
    //         endcase
    //     end else begin
    //         sdata_out = 32'd0; // stan neutralny
    //     end
    // end
    

    // Automat stanów i obliczenia (reakcja na clk)
    always @(posedge clk or negedge n_reset) begin
        if (!n_reset) begin
            state <= S_IDLE;
            status_reg <= 0;
            result_h <= 0; result_l <= 0;
            arg1_reg <= 0;
            arg2_reg <= 0;
            m_reg <= 0;
            e_reg <= 0;
            s_reg <= 0;
            gpio_out_s <= 0;
        end else begin
            case (state)
                S_IDLE: begin
                    status_reg <= 0;
                    if (ctrl_reg[0]) begin
                        // Zatrzaśnięcie argumentów
                        arg1_reg <= {arg1_h, arg1_l};
                        arg2_reg <= {arg2_h, arg2_l};
                        state <= S_CALC;
                    end
                end

                S_CALC: begin
                    status_reg <= 1; // busy
                    // wykrycie zera: E = 0 i M = 0 -> wynik zero
                    if (arg1_is_zero || arg2_is_zero) begin
                        m_reg <= 0;
                        e_reg  <= 0;
                        s_reg <= 0;
                    end else begin
                        m_reg <= {1'b1, m1} * {1'b1, m2}; // iloczyn mantys z ukrytą jedynką
                        e_reg  <= $signed({1'b0, e1}) + $signed({1'b0, e2}) - $signed({1'b0, BIAS});
                        s_reg <= s1 ^ s2; // XOR znaków obu liczb
                    end
                    state <= S_WRITE;
                end
                
                S_WRITE: begin
                    result_h <= m_final[35:4]; // 32 bity
                    result_l <= { m_final[3:0], e_final[26:0], s_reg };
                    state <= S_DONE;
                end

                S_DONE: begin
                    status_reg <= 2;    // done
                    if (!ctrl_reg[0]) begin // Powrót do IDLE dopiero po wyzerowaniu bitu sterującego
                        status_reg <= 0; // idle
                        state <= S_IDLE;
                    end
                end
            endcase
        end
    end

endmodule
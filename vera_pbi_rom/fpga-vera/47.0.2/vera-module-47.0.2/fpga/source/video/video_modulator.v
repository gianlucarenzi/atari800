//`default_nettype none

module video_modulator(
    input  wire        clk,

    input  wire  [3:0] r,
    input  wire  [3:0] g,
    input  wire  [3:0] b,
    input  wire        color_burst,
    input  wire        active,
    input  wire        sync_n_in,

    output reg   [5:0] luma,
    output reg   [5:0] chroma) /* synthesis syn_hier = "hard" */;

    parameter Y_R   = 27; // 38; //  0.299
    parameter Y_G   = 53; // 75; //  0.587
    parameter Y_B   = 10; // 14; //  0.114

    parameter I_R   = 76; //  0.5959
    parameter I_G_n = 35; // -0.2746 (this should actually be -35, so *after* multiplication the result is negated)
    parameter I_B_n = 41; // -0.3213 (this should actually be -41, so *after* multiplication the result is negated)

    parameter Q_R   = 27; //  0.2115
    parameter Q_G_n = 66; // -0.5227 (this should actually be -66, so *after* multiplication the result is negated)
    parameter Q_B   = 40; //  0.3112

    // We set up one DSP for 2 of the 9 multiplications

    wire [15:0] Y_G_times_g_16, Y_R_times_r_16;

    // We use one DSP for two 8x8 unsigned multiplications
    video_modulator_mult_u8xu8_pair video_modulator_mult_yg_yr (
        .clk(clk),
        
        // Y_G_times_g = Y_G * g
        .input_2a_8(Y_G[7:0]),
        .input_2b_8({4'b0000, g}),
        .output_2_16(Y_G_times_g_16),
        
        // Y_R_times_r = Y_R * r
        .input_1a_8(Y_R[7:0]),
        .input_1b_8({4'b0000, r}),
        .output_1_16(Y_R_times_r_16)
    );


    // We need these nine differently shifted values to replace the remaining multiplications by additions
    
    wire [5:0] r_times_4  = { r, 2'b00 };
    wire [6:0] r_times_8  = { r, 3'b000 };
    wire [9:0] r_times_64 = { r, 6'b000000 };
    
    wire [4:0] g_times_2  = { g, 1'b0 };
    wire [8:0] g_times_32 = { g, 5'b00000 };
    wire [9:0] g_times_64 = { g, 6'b000000 };

    wire [4:0] b_times_2  = { b, 1'b0 };
    wire [6:0] b_times_8  = { b, 3'b000 };
    wire [8:0] b_times_32 = { b, 5'b00000 };
    
    // We put together all the 9 multiplication results (all unsigned so far)

    wire [11:0] Y_R_times_r   = Y_R_times_r_16[11:0];       // Y_R_times_r = Y_R * r
    wire [11:0] Y_G_times_g   = Y_G_times_g_16[11:0];       // Y_G_times_g = Y_G * g
    wire [11:0] Y_B_times_b   = b_times_8 + b_times_2;      // Y_B_times_b = Y_B * b and since Y_B is 10 (8+2), Y_B_times_b = 8*b + 2*b
    
    wire [11:0] Q_R_times_r   = Y_R_times_r;                // Q_R_times_r = Q_R * r and since Q_R is equal to Y_R, Q_R_times_r = Y_R_times_r
    wire [11:0] Q_G_n_times_g = g_times_64 + g_times_2;     // Q_G_n_times_g = Q_G_n * g and since Q_G_n is 66 (64+2), Q_G_n_times_g = 64*g + 2*g
    wire [11:0] Q_B_times_b   = b_times_32 + b_times_8;     // Q_B_times_b = Q_B * b and since Q_B is 40 (32+8), Q_B_times_b = 32*b + 8*b

    wire [11:0] I_R_times_r   = r_times_64 + r_times_8 + r_times_4; // I_R_times_r = I_R * r and since I_R is 76 (64+8+4), I_R_times_r = 64*r + 8*r + 4*2
    wire [11:0] I_G_n_times_g = g_times_32 + g_times_2 + g; // I_G_n_times_g = I_G * g and since I_G_n is 35 (32+2+1), I_G_n_times_g = 32*g + 2*g + g
    wire [11:0] I_B_n_times_b = Q_B_times_b + b;            // I_B_n_times_b = I_B_n * b and since I_B_n is 41 (32+8+1), I_B_n_times_b = Q_B_times_b + 1*b
    
    reg signed [11:0] y_s;
    reg signed [11:0] i_s;
    reg signed [11:0] q_s;

    always @(posedge clk) begin
        
        case ({active, color_burst})
            2'b00: begin
                y_s <= (sync_n_in == 0) ? 12'd0 : 12'd544;
                i_s <= 0;
                q_s <= 0;
            end
            2'b01: begin
                y_s <= (sync_n_in == 0) ? 12'd0 : 12'd544;
                i_s <= (I_R * 5'd9) - (I_G_n * 5'd9) - (I_B_n * 5'd0);
                q_s <= (Q_R * 5'd9) - (Q_G_n * 5'd9) + (Q_B * 5'd0);
            end
            2'b10: begin
                y_s <= Y_R_times_r + Y_G_times_g   + Y_B_times_b + (128 + 512);
                i_s <= I_R_times_r - I_G_n_times_g - I_B_n_times_b;               // Effectively negating I_G_n and I_B_n here
                q_s <= Q_R_times_r - Q_G_n_times_g + Q_B_times_b;                 // Effectively negating Q_G_n here
            end
            2'b11: begin
                y_s <= (Y_R * 5'd9) + (Y_G   * 5'd9) + (Y_B * 5'd0) + (128 + 512);
                i_s <= (I_R * 5'd9) - (I_G_n * 5'd9) - (I_B_n * 5'd0);
                q_s <= (Q_R * 5'd9) - (Q_G_n * 5'd9) + (Q_B * 5'd0);
            end
        endcase
        
    end

    // Color burst frequency: 315/88 MHz = 3579545 Hz
    reg  [23:0] phase_accum_r = 0;
    always @(posedge clk) phase_accum_r <= phase_accum_r + 24'd2402192;

    wire [7:0] sinval;
    video_modulator_sinlut sinlut(
        .clk(clk),
        .phase(phase_accum_r[23:15]),
        .value(sinval));

    wire [7:0] cosval;
    video_modulator_coslut coslut(
        .clk(clk),
        .phase(phase_accum_r[23:15]),
        .value(cosval));

    wire signed [7:0] sinval_s = sinval;
    wire signed [7:0] cosval_s = cosval;

    wire signed [7:0] i8_s = i_s[11:4];
    wire signed [7:0] q8_s = q_s[11:4];

    reg         [7:0] lum;
    reg signed [13:0] chroma_s;

    always @(posedge clk) begin
        if (y_s < 0)
            lum <= 0;
        else if (y_s >= 2047)
            lum <= 255;
        else
            lum <= y_s[10:3];

        chroma_s <= (cosval_s * i8_s) + (sinval_s * q8_s);
    end

    always @(posedge clk) begin
        luma   <= lum[7:2];
        chroma <= chroma_s[13:8] + 6'd32;
    end

endmodule

//`default_nettype none

module video_modulator_mult_u8xu8_pair (
    input  wire        clk,
    
    input  wire [7:0] input_1a_8,
    input  wire [7:0] input_1b_8,
    input  wire [7:0] input_2a_8,
    input  wire [7:0] input_2b_8,
    
    output wire [15:0] output_1_16,
    output wire [15:0] output_2_16);
    
    wire        [31:0] output_32;
    assign output_1_16 = output_32[15:0];
    assign output_2_16 = output_32[31:16];

    pmi_dsp mult8x8 ( // port interfaces
        .A({input_2a_8, input_1a_8}),
        .B({input_2b_8, input_1b_8}),
        .C(16'b0),
        .D(16'b0),
        .O(output_32),
        .CLK(clk),
        .CE(1'b1),
        .IRSTTOP(1'b0),
        .IRSTBOT(1'b0),
        .ORSTTOP(1'b0),
        .ORSTBOT(1'b0),
        .AHOLD(1'b0),
        .BHOLD(1'b0),
        .CHOLD(1'b0),
        .DHOLD(1'b0),
        .OHOLDTOP(1'b0),
        .OHOLDBOT(1'b0),
        .OLOADTOP(1'b0),
        .OLOADBOT(1'b0),
        .ADDSUBTOP(1'b0),
        .ADDSUBBOT(1'b0),
        .CO(),
        .CI(1'b0),
        .ACCUMCI(1'b0),
        .ACCUMCO(),
        .SIGNEXTIN(1'b0),
        .SIGNEXTOUT()
    );
    defparam mult8x8.TOPOUTPUT_SELECT = 2'b10; //Mult8x8 data output
    defparam mult8x8.BOTOUTPUT_SELECT = 2'b10; //Mult8x8 data output
    defparam mult8x8.A_SIGNED = 1'b0; //Unsigned Inputs
    defparam mult8x8.B_SIGNED = 1'b0; //Unsigned Inputs
    
endmodule

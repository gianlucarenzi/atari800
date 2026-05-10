//`default_nettype none

module mult_accum (
    input  wire        clk,
    
    input  wire [15:0] input_a_16,
    input  wire [15:0] input_b_16,
    input  wire        mult_enabled,
    input  wire        reset_accum,
    input  wire        accumulate,
    input  wire        add_or_sub,
    
    output wire [31:0] output_32);
    
    pmi_dsp i_mult16x16 ( // port interfaces
        .A(input_a_16),
        .B(input_b_16),
        .C(input_b_16), // This is used to pass through the original value of the cache
        .D(input_a_16), // This is used to pass through the original value of the cache
        .O(output_32),
        .CLK(clk),
        .CE(1'b1),
        .IRSTTOP(1'b0),
        .IRSTBOT(1'b0),
        .ORSTTOP(reset_accum),
        .ORSTBOT(reset_accum),
        .AHOLD(1'b0),
        .BHOLD(1'b0),
        .CHOLD(1'b0),
        .DHOLD(1'b0),
        .OHOLDTOP(!accumulate),
        .OHOLDBOT(!accumulate),
        .OLOADTOP(!mult_enabled), // We are using the LOAD to switch between the multiplier output and (effectively) input C
        .OLOADBOT(!mult_enabled), // We are using the LOAD to switch between the multiplier output and (effectively) input D
        .ADDSUBTOP(add_or_sub),
        .ADDSUBBOT(add_or_sub),
        .CO(),
        .CI(1'b0),  
        .ACCUMCI(1'b0),
        .ACCUMCO(),
        .SIGNEXTIN(1'b0),
        .SIGNEXTOUT()
    );
    defparam i_mult16x16.TOPOUTPUT_SELECT = 2'b00; // Adder output (non registered)
    defparam i_mult16x16.BOTOUTPUT_SELECT = 2'b00; // Adder output (non registered)
    defparam i_mult16x16.A_SIGNED = 1'b1; //Signed Inputs
    defparam i_mult16x16.B_SIGNED = 1'b1;
    
    defparam i_mult16x16.TOPADDSUB_CARRYSELECT  = 2'b10; // 10: Cascade ACCUMOUT from lower Adder/Subtractor
    
    defparam i_mult16x16.TOPADDSUB_LOWERINPUT = 2'b10; // We send the output (the 16 upper bits) of the 16x16 multiplier to the lower side of the top accumilator
    defparam i_mult16x16.TOPADDSUB_UPPERINPUT = 1'b0;  // We send the output of the top (output) flip-flop to the upper side of the top accumilator
    defparam i_mult16x16.BOTADDSUB_LOWERINPUT = 2'b10; // We send the output (the 16 lower bits) of the 16x16 multiplier to the lower side of the bottom accumilator
    defparam i_mult16x16.BOTADDSUB_UPPERINPUT = 1'b0;  // We send the output of the bottom (output) flip-flop to the upper side of the top accumilator
    
    
endmodule
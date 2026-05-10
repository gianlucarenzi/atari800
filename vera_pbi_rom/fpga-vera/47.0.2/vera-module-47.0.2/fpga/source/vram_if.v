//`default_nettype none

module vram_if(
    input  wire        clk,

    // Interface 0 - 8-bit (highest priority)
    input  wire [16:0] if0_addr,
    input  wire        if0_addr_nibble,
    input  wire        if0_4bit_mode,
    input  wire        if0_cache_write_enabled,
    input  wire        if0_transparency_enabled,
    input  wire        if0_one_byte_cache_cycling,
    input  wire  [7:0] if0_cache8,
    input  wire [31:0] if0_mult_accum_cache32,
    input  wire  [7:0] if0_wrdata,
    output wire  [7:0] if0_rddata,
    input  wire        if0_strobe,
    input  wire        if0_write,

    // Interface 1 - 32-bit read only
    input  wire [14:0] if1_addr,
    output wire [31:0] if1_rddata,
    input  wire        if1_strobe,
    output reg         if1_ack,

    // Interface 2 - 32-bit read only
    input  wire [14:0] if2_addr,
    output wire [31:0] if2_rddata,
    input  wire        if2_strobe,
    output reg         if2_ack,

    // Interface 3 - 32-bit read only
    input  wire [14:0] if3_addr,
    output wire [31:0] if3_rddata,
    input  wire        if3_strobe,
    output reg         if3_ack) /* synthesis syn_hier = "hard" */;

    //////////////////////////////////////////////////////////////////////////
    // Main RAM 128kB (32k x 32)
    //////////////////////////////////////////////////////////////////////////
    reg  [14:0] ram_addr;
    reg  [31:0] ram_wrdata;
    reg   [7:0] ram_wrnibblesel;
    wire [31:0] ram_rddata;
    wire        ram_write;

    main_ram main_ram(
        .clk(clk),
        .bus_addr(ram_addr),
        .bus_wrdata(ram_wrdata),
        .bus_wrnibblesel(ram_wrnibblesel),
        .bus_rddata(ram_rddata),
        .bus_write(ram_write));

    //////////////////////////////////////////////////////////////////////////
    // Priority memory access
    //////////////////////////////////////////////////////////////////////////
    reg if0_ack, if0_ack_next;
    reg if1_ack_next;
    reg if2_ack_next;
    reg if3_ack_next;
    
    reg [1:0] byte_0_transparency_nibblesel;
    reg [1:0] byte_1_transparency_nibblesel;
    reg [1:0] byte_2_transparency_nibblesel;
    reg [1:0] byte_3_transparency_nibblesel;
    
    reg [1:0] byte_transparancy_nibblesel;
    
    reg [7:0] if0_wrdata_to_use /* synthesis syn_keep=1 */;
    
    assign ram_write  = if0_strobe && if0_write;

    always @* begin
        
        if (if0_one_byte_cache_cycling) begin
            if0_wrdata_to_use = if0_cache8;
        end else begin
            if0_wrdata_to_use = if0_wrdata;
        end
        
        if (if0_cache_write_enabled && !if0_one_byte_cache_cycling) begin
            // In cache write mode, we use the 32-bit data from the cache
            ram_wrdata = if0_mult_accum_cache32;
        end else begin
            // In non-cache write mode, we use the wrdata and duplicate to all four 8-bit channels
            ram_wrdata = {4{if0_wrdata_to_use}};
        end
        
        if (if0_cache_write_enabled) begin
            // In cache write mode, we use the 32-bit data from the cache
            if (if0_transparency_enabled) begin
                
                if (if0_4bit_mode) begin
                    byte_3_transparency_nibblesel = {ram_wrdata[31:28] != 0, ram_wrdata[27:24] != 0};
                    byte_2_transparency_nibblesel = {ram_wrdata[23:20] != 0, ram_wrdata[19:16] != 0};
                    byte_1_transparency_nibblesel = {ram_wrdata[15:12] != 0, ram_wrdata[11:8] != 0};
                    byte_0_transparency_nibblesel = {ram_wrdata[7:4] != 0, ram_wrdata[3:0] != 0};
                end else begin
                    byte_3_transparency_nibblesel = ram_wrdata[31:24] == 0 ? 2'b00 : 2'b11;
                    byte_2_transparency_nibblesel = ram_wrdata[23:16] == 0 ? 2'b00 : 2'b11;
                    byte_1_transparency_nibblesel = ram_wrdata[15:8] == 0 ? 2'b00 : 2'b11;
                    byte_0_transparency_nibblesel = ram_wrdata[7:0] == 0 ? 2'b00 : 2'b11;
                end
                
                // In transparent cache write mode, we check each byte (or nibble) we are writing: if its 0, its considered transparent and not written to VRAM
                ram_wrnibblesel = {byte_3_transparency_nibblesel,
                                   byte_2_transparency_nibblesel,
                                   byte_1_transparency_nibblesel,
                                   byte_0_transparency_nibblesel};
            end else begin
                // In normal cache write mode, we invert the byte written to us and use it as a nibble mask
                ram_wrnibblesel = ~if0_wrdata; 
            end
        end else begin
            // In non-cache write mode, we check the byte (or nibble) we are writing: if its 0, its considered transparent and not written to VRAM
            if (if0_4bit_mode) begin
                byte_transparancy_nibblesel = {!if0_addr_nibble && (!if0_transparency_enabled || if0_wrdata_to_use[7:4] != 0), 
                                                if0_addr_nibble && (!if0_transparency_enabled || if0_wrdata_to_use[3:0] != 0)};
            end else begin
                byte_transparancy_nibblesel = (if0_transparency_enabled && if0_wrdata_to_use == 0) ? 2'b00 : 2'b11;
            end

            // In non-cache write mode, we write the byte correspronding to the addr[1:0], unless we are in transparant mode and write a 0
            case (if0_addr[1:0])
                2'b00: ram_wrnibblesel = { 6'b000000, byte_transparancy_nibblesel };
                2'b01: ram_wrnibblesel = { 4'b0000, byte_transparancy_nibblesel, 2'b00 };
                2'b10: ram_wrnibblesel = { 2'b00, byte_transparancy_nibblesel, 4'b0000 };
                2'b11: ram_wrnibblesel = { byte_transparancy_nibblesel, 6'b000000 };
            endcase
        end

    end

    always @* begin
        ram_addr     = 15'b0;
        if0_ack_next = 1'b0;
        if1_ack_next = 1'b0;
        if2_ack_next = 1'b0;
        if3_ack_next = 1'b0;

        if (if0_strobe) begin
            ram_addr     = if0_addr[16:2];
            if0_ack_next = 1'b1;

        end else if (if1_strobe) begin
            ram_addr     = if1_addr;
            if1_ack_next = 1'b1;

        end else if (if2_strobe) begin
            ram_addr     = if2_addr;
            if2_ack_next = 1'b1;

        end else if (if3_strobe) begin
            ram_addr     = if3_addr;
            if3_ack_next = 1'b1;
        end
    end

    always @(posedge clk) begin
        if0_ack <= if0_ack_next;
        if1_ack <= if1_ack_next;
        if2_ack <= if2_ack_next;
        if3_ack <= if3_ack_next;
    end

    reg [1:0] if0_addr_r;
    always @(posedge clk) if0_addr_r <= if0_addr[1:0];

    // Memory bus read data selection
    reg [7:0] if0_rddata8;
    reg [7:0] if0_rddata8_r;
    always @* case (if0_addr_r)
        2'b00: if0_rddata8 = ram_rddata[7:0];
        2'b01: if0_rddata8 = ram_rddata[15:8];
        2'b10: if0_rddata8 = ram_rddata[23:16];
        2'b11: if0_rddata8 = ram_rddata[31:24];
    endcase

    always @(posedge clk) begin
        if (if0_ack) begin
            if0_rddata8_r <= if0_rddata8;
        end
    end

    assign if0_rddata = if0_ack ? if0_rddata8 : if0_rddata8_r;
    assign if1_rddata = ram_rddata;
    assign if2_rddata = ram_rddata;
    assign if3_rddata = ram_rddata;

endmodule

`ifndef _ram_2port_be_init_
`define _ram_2port_be_init_

module ram_2port_be #(parameter
	DATA_WIDTH = 25,
	ADDR_WIDTH = 4,
	BYTE_WIDTH = 8,
	BYTES = (DATA_WIDTH % BYTE_WIDTH == 0) ? DATA_WIDTH / BYTE_WIDTH : DATA_WIDTH / BYTE_WIDTH + 1
)(
	input clk_a, write_a,
	input [ADDR_WIDTH-1:0] addr_a,
	input [BYTES-1:0] be_a,
	input [DATA_WIDTH-1:0] data_a,
	output reg [DATA_WIDTH-1:0] q_a,
	
	input clk_b,
	input [ADDR_WIDTH-1:0] addr_b,
	output reg [DATA_WIDTH-1:0] q_b
);
	localparam REM_WIDTH = DATA_WIDTH - (BYTES - 1) * BYTE_WIDTH;

	genvar i;
	
	generate for (i = 0; i < BYTES; i++)
		begin :bytes
			if (i < BYTES - 1)
				begin :main
					reg [BYTE_WIDTH-1:0] ram[2**ADDR_WIDTH];
					
					always_ff @(posedge clk_a)
						if (write_a)
							begin
								if (be_a[i]) ram[addr_a] <= data_a[i * BYTE_WIDTH +: BYTE_WIDTH];
							end
						else
							q_a[i * BYTE_WIDTH +: BYTE_WIDTH] <= ram[addr_a];
					
					always_ff @(posedge clk_b)
						q_b[i * BYTE_WIDTH +: BYTE_WIDTH] <= ram[addr_b];
				end
			else
				begin :rem
					reg [REM_WIDTH-1:0] ram[2**ADDR_WIDTH];
					
					always_ff @(posedge clk_a)
						if (write_a)
							begin
								if (be_a[i]) ram[addr_a] <= data_a[i * BYTE_WIDTH +: REM_WIDTH];
							end
						else
							q_a[i * BYTE_WIDTH +: REM_WIDTH] <= ram[addr_a];
							
					always_ff @(posedge clk_b)
						q_b[i * BYTE_WIDTH +: REM_WIDTH] <= ram[addr_b];
				end
		end
	endgenerate
	
endmodule :ram_2port_be

module ram_2port_be_init #(
	parameter DATA_WIDTH = 25,
	parameter ADDR_WIDTH = 4,
	parameter bit [DATA_WIDTH-1:0] INIT_VALUE = {1'b1, {(DATA_WIDTH-1){1'b0}}},
	parameter BYTE_WIDTH = 8,
	parameter BYTES = (DATA_WIDTH % BYTE_WIDTH == 0) ? DATA_WIDTH / BYTE_WIDTH : DATA_WIDTH / BYTE_WIDTH + 1
)(
	input set,
	input clk_a, write_a,
	input [ADDR_WIDTH-1:0] addr_a,
	input [BYTES-1:0] be_a,
	input [DATA_WIDTH-1:0] data_a,
	output [DATA_WIDTH-1:0] q_a,
	output ready,
	
	input clk_b,
	input [ADDR_WIDTH-1:0] addr_b,
	output [DATA_WIDTH-1:0] q_b
);
	reg [DATA_WIDTH-1:0] ram[2**ADDR_WIDTH];
	reg [ADDR_WIDTH-1:0] addr = '0;
	reg [BYTES-1:0] be = '0;
	reg [DATA_WIDTH-1:0] data = INIT_VALUE;
	reg init = 1'b1, write = 1'b1;
	
	always_ff @(posedge clk_a)
		if (set)
			init <= 1'b1;
		else if (addr == '1)
			init <= 1'b0;
	
	always_ff @(posedge clk_a)
		if (set)
			addr <= '0;
		else if (init)
			addr <= addr + 1'b1;
		else
			addr <= addr_a;
			
	always_ff @(posedge clk_a) begin
		be		<= (set || init) ? {BYTES{1'b1}} : be_a;			
		data	<= (set || init) ? INIT_VALUE : data_a;
		write	<= (set || init) ? 1'b1 : write_a;
	end
			
	assign ready = !(set || init);
	
	ram_2port_be #(.DATA_WIDTH(DATA_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .BYTE_WIDTH(BYTE_WIDTH)) ram_inst(
		.clk_a, .write_a(write), .addr_a(addr), .be_a(be), .data_a(data), .q_a,
		.clk_b, .addr_b, .q_b
	);
	
endmodule :ram_2port_be_init

`endif

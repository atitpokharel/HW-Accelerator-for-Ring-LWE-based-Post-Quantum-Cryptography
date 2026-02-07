module ringlwe_mult16_mmio_parallel (
    input  logic clk, reset_n, chipselect, write, read,
    input  logic [5:0] address,   
    input  logic [31:0] writedata,
    output logic [31:0] readdata
);

    logic status_done, start_pulse, fsm_busy, fsm_done_pulse;

   //lets have some registers, A, S and P(32b)
    logic signed [15:0] reg_A [0:15];
    logic signed [15:0] reg_S [0:15];
    logic signed [31:0] reg_P [0:15];  // Result registers

    //custom struct for fsm
    typedef enum logic [1:0] {IDLE  = 2'b00, ACCUM = 2'b01, DONE  = 2'b10} my_state_type;
    my_state_type cur_state, next_state;

    //index for outer loop
    logic [3:0] i; //can go up to 15 (16 elems), outer loop index
    //accumulator for the result
    logic signed [31:0] acc [0:15];
    assign fsm_busy = (cur_state != IDLE); //busy if not idle

    //next state logic for the fsm
    always_comb begin
        next_state = cur_state;
        case(cur_state)
            IDLE: if (start_pulse) next_state = ACCUM;
            //so, when the index reaches 16, one round is done, go to done
            ACCUM:if (i == 4'd15) next_state = DONE;
            DONE: next_state = IDLE;
            default: next_state = IDLE;
        endcase
    end

    //main sequential logic
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin //reset the registers
            for (int k = 0; k < 16; k++) begin //reset all registers 
                reg_A[k]<= 16'sd0;
                reg_S[k] <= 16'sd0;
                reg_P[k] <= 32'sd0;
            end
            status_done <= 1'b0; //ctrl and stat - 0
            start_pulse <= 1'b0;
            cur_state <= IDLE;//index reset. cur_state to idle
            i <= 4'd0;
            acc <= 32'sd0;
            fsm_done_pulse <= 1'b0;

        end else begin //if not reset, 
            start_pulse <= 1'b0; //lets make sure, default point is 0
            fsm_done_pulse <= 1'b0;
            cur_state <= next_state; //cur_state to next state
            
            case (cur_state)
                IDLE: begin //in idle, if we get start pulse, first clear acc and reset the index, and it begins
                    if (start_pulse) begin
                        acc <= 32'sd0;
                        i <= 4'd0;
                    end
                end

                ACCUM: begin //in accumulator,
                   //so in each cc, one elem is A is used, indexed by i, incrememted later
                   //outer loop case solved, now inner loop is to be parallelized

                   //logic is, if i did 16 inner calc acc[k] += A[i] * S[j] at once, and add them in acc, 
                   //it wont affect my result, so, i can do this in parallel.
                    for (int k = 0; k < 16; k++) begin //so, 16 inner operations at once, it ll use 16 mul
                        if (k >= i) begin //same logic, previously, I used to do i+j to get k, here, I index k so
                        //i can get, k-i to get j, so what value to put in P[k] : mul and add/sub of A[i] and S[k-i]
                            acc[k] <= acc[k] + (reg_A[i] * reg_S[k - i]);
                        end else begin
                            acc[k] <= acc[k] - (reg_A[i] * reg_S[16 + k - i]);
                        end
                    end

                    //incrememnt outer loop index
                    if (i != 4'd15) begin
                        i <= i + 1'b1;
                    end
                end

                DONE: begin
                    fsm_done_pulse <= 1'b1; //done pulse to 1
                    reg_P <= acc; //acc to P
                    status_done <= 1'b1; //done status to 1
                end
            endcase

            //write logic
            if (chipselect && write) begin
                if (address == 6'd32) begin
                    if (writedata[0]) begin //start bit
                        status_done <= 1'b0; //clear done status
                        start_pulse <= 1'b1; //trigger fsm start
                    end
                end
                else if (!fsm_busy) begin //only allow writing inputs if fsm is idle
                    if (address < 6'd16) begin
                        reg_A[address] <= writedata[15:0];
                    end else if (address >= 6'd16 && address < 6'd32) begin
                        // Address 16-31: reg_S (Offset by 16)
                        reg_S[address - 6'd16] <= writedata[15:0];
                    end
                end
            end
        end
    end

    //read logic, same logic as before, just read the registers
    always_comb begin
        readdata = 32'd0;
        if (chipselect && read) begin
            if (address < 6'd16) begin
                readdata = {{16{reg_A[address][15]}}, reg_A[address]};
            end 
            else if (address >= 6'd16 && address < 6'd32) begin
                readdata = {{16{reg_S[address - 6'd16][15]}}, reg_S[address - 6'd16]};
            end 
            else if (address == 6'd32) begin
                readdata = {30'd0, status_done, fsm_busy};
            end 
            else if (address >= 6'd33 && address < 6'd49) begin
                //read result p, offset by 33
                readdata = reg_P[address - 6'd33];
            end
        end
    end

endmodule
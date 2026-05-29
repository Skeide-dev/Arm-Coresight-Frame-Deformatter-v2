library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

package frame_types is
    type channel_t is record
        data  : std_logic_vector(7 downto 0);
        id    : std_logic_vector(6 downto 0);
    end record;
    
    type channel_array is array (0 to 3) of channel_t;
    constant NULL_CHAN : channel_t := (data => (others => '0'), id => (others => '0'));

    -- The block diagram in vivado does not accept "records" as output/input so we map it over to vector
    function to_vector(c : channel_t) return std_logic_vector;
end package; 

package body frame_types is
    function to_vector(c : channel_t) return std_logic_vector is
    begin
        -- Pack record into a standard 16-bit vector: [Data(8) | 0 | ID(7)]
        return c.data & '0' & c.id;
    end function;
end package body;

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use work.frame_types.all;

entity frame_deformatter is 
    Port ( 
        ACLK      : in  STD_LOGIC;
        ARESETN   : in  STD_LOGIC;
        Frame_in  : in  STD_LOGIC_VECTOR (127 downto 0);
        rd_en     : in  STD_LOGIC;
        -- Every clock cycle, we output 4 bytes of data, 4 bytes ID. 
        -- One valid signal so Axi_Master know when to store data into the buffer.
        Channels : out STD_LOGIC_VECTOR (63 downto 0);
        valid_f   : out STD_LOGIC
    );
end frame_deformatter;

architecture Behavioral of frame_deformatter is
    type state_type is (IDLE, CYCLE_A, CYCLE_B, CYCLE_C);
    signal state : state_type := IDLE;

    signal new_id            : std_logic_vector(6 downto 0) := (others => '0');
    signal old_id            : std_logic_vector(6 downto 0) := (others => '0');
    signal NewID_isDelayed : std_logic := '0';
    signal error             : std_logic := '0';
    signal valid_output      : std_logic := '0';

    type byte_array is array (0 to 14) of std_logic_vector(7 downto 0);
    signal bytes_w, bytes_r : byte_array;
    signal auxiliary_w, auxiliary_r : std_logic_vector(7 downto 0);
    
    signal chan_regs : channel_array := (others => NULL_CHAN);

begin
    -- Map internal records to the output ports using the conversion function
Channels <= to_vector(chan_regs(3)) & to_vector(chan_regs(2)) & to_vector(chan_regs(1)) & to_vector(chan_regs(0));
    
    -- Wiring
    auxiliary_w <= Frame_in(127 downto 120);
    gen_mapping: for i in 0 to 14 generate
        bytes_w(i) <= Frame_in((i*8)+7 downto i*8);
    end generate;

    process(ACLK)
        variable v_selector : std_logic_vector(1 downto 0);
    begin
        if rising_edge(ACLK) then
            if ARESETN = '0' then
                state <= IDLE;
                chan_regs <= (others => NULL_CHAN);
                new_id <= (others => '0');
                old_id <= (others => '0');
                NewID_isDelayed <= '0';
                error <= '0';
            else
                case state is
                    when IDLE =>
                    valid_output <= '0';
                        if rd_en = '1' then
                        valid_output <= '1';
                            -- create a snapshot of the data into registers
                            bytes_r     <= bytes_w;
                            auxiliary_r <= auxiliary_w;
                            
                            -- Clear channel 3 immediately (only 3 bytes this cycle)
                            chan_regs(3) <= NULL_CHAN;
                            -- Uses a variable here because vivado had issues accepting the case statements 
                            v_selector := bytes_w(2)(0) & bytes_w(0)(0);
                            case v_selector is -- case statement for ID/DATA bytes, "1" if they are ID, "0" if they are Data
                                when "00" => 
                                    if NewID_isDelayed = '0' then -- was the previous ID delayed?
                                        chan_regs(0) <= (data => (bytes_w(0)(7 downto 1) & auxiliary_w(0)), id => new_id);
                                    else -- Previous ID is delayed!
                                        chan_regs(0) <= (data => (bytes_w(0)(7 downto 1) & auxiliary_w(0)), id => old_id);
                                        NewID_isDelayed <= '0'; -- an ID can only be delayed for one data byte!
                                    end if;
                                    chan_regs(1) <= (data => bytes_w(1), id => new_id);
                                    chan_regs(2) <= (data => (bytes_w(2)(7 downto 1) & auxiliary_w(1)), id => new_id);


                                when "01" => 
                                    chan_regs(0) <= NULL_CHAN; -- byte 0 is ID
                                    
                                    if auxiliary_w(0) = '0' then -- is Byte 0 ID Delayed ?
                                        chan_regs(1) <= (data => bytes_w(1), id => bytes_w(0)(7 downto 1));
                                    else -- Byte 0 ID is delayed! Use the previous ID for Byte 1!
                                        chan_regs(1) <= (data => bytes_w(1), id => new_id);
                                    end if;
                                    chan_regs(2) <= (data => (bytes_w(2)(7 downto 1) & auxiliary_w(1)), id => bytes_w(0)(7 downto 1));
                                    
                                    -- Save snapshots of ID's so next cycle know what to do.
                                    old_id <= new_id;
                                    new_id <= bytes_w(0)(7 downto 1);
                                    NewID_isDelayed <= '0'; -- since we already have gone past the id's next byte, this must be 0.


                                when "10" => 
                                    chan_regs(2) <= NULL_CHAN; -- Byte 2 is ID.
                                    
                                    if NewID_isDelayed = '0' then -- was the previous ID delayed?
                                        chan_regs(0) <= (data => (bytes_w(0)(7 downto 1) & auxiliary_w(0)), id => new_id);
                                    else -- previous ID is delayed!
                                        chan_regs(0) <= (data => (bytes_w(0)(7 downto 1) & auxiliary_w(0)), id => old_id);
                                    end if;
                                    chan_regs(1) <= (data => bytes_w(1), id => new_id);
                                    
                                    -- save ID snapshot for next cycle
                                    old_id <= new_id;
                                    new_id <= bytes_w(2)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_w(1); -- aux bits tell if ID's are delayed or not!
                                    

                                when "11" =>
                                    chan_regs(0) <= NULL_CHAN; -- byte 0 is ID
                                    chan_regs(2) <= NULL_CHAN; -- byte 2 is ID
                                    
                                    if auxiliary_w(0) = '0' then -- is byte 0 ID delayed?
                                        chan_regs(1) <= (data => bytes_w(1), id => bytes_w(0)(7 downto 1));
                                    else -- byte 0 ID is delayed!
                                        chan_regs(1) <= (data => bytes_w(1), id => new_id);
                                    end if;
                                    
                                    -- save ID snapshot for next cycle
                                    old_id <= bytes_w(0)(7 downto 1);
                                    new_id <= bytes_w(2)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_w(1); -- aux(1) tells next cycle if byte 2 is delayed or not!

                                when others => error <= '1'; -- Handles "U" "X" "Z" values, undefined values means something bad happened!
                            end case;
                        state <= CYCLE_A;-- Note: in the next cycles we will always decode 4 bytes instead of 3. This should help split the amount of logic gates across each cycle, since in IDLE state we also check for rd_en.
                        end if;


                    when CYCLE_A => -- Bytes 3, 4, 5, 6
                    valid_output <= '1';
                                v_selector := bytes_r(6)(0) & bytes_r(4)(0);
                                case v_selector is
                                when "00" => 
                                    if NewID_isDelayed = '0' then -- Is the new_ID (byte 2) delayed ?
                                        chan_regs(0) <= (data => bytes_r(3), id => new_id);
                                    else
                                        chan_regs(0) <= (data => bytes_r(3), id => old_id);
                                        NewID_isDelayed <= '0'; 
                                    end if;
                                    chan_regs(1) <= (data => (bytes_r(4)(7 downto 1) & auxiliary_r(2)), id => new_id);
                                    chan_regs(2) <= (data => bytes_r(5), id => new_id);
                                    chan_regs(3) <= (data => (bytes_r(6)(7 downto 1) & auxiliary_r(3)), id => new_id);


                                when "01" =>
                                    chan_regs(1) <= NULL_CHAN; -- byte4 is ID

                                    if NewID_isDelayed = '0' then -- is new_ID (byte 2) delayed?
                                        chan_regs(0) <= (data => bytes_r(3), id => new_id);
                                    else -- new_ID (byte 2) is delayed!
                                        chan_regs(0) <= (data => bytes_r(3), id => old_id);
                                    end if;
                                    if auxiliary_r(2) = '0' then -- is byte 4 ID delayed?
                                        chan_regs(2) <= (data => bytes_r(5), id => bytes_r(4)(7 downto 1));
                                    else  -- byte 4 ID is delayed!
                                        chan_regs(2) <= (data => bytes_r(5), id => new_id);
                                    end if;
                                    chan_regs(3) <= (data => bytes_r(6)(7 downto 1) & auxiliary_r(3), id => bytes_r(4)(7 downto 1));
                                    
                                    old_id <= new_id;
                                    new_id <= bytes_r(4)(7 downto 1);
                                    NewID_isDelayed <= '0'; -- since we already have gone past the id's next byte, this must be 0.

                                when "10" =>
                                    chan_regs(3) <= NULL_CHAN; -- byte 6 is an id

                                    if NewID_isDelayed = '0' then -- is new_ID (byte 2) delayed?
                                        chan_regs(0) <= (data => bytes_r(3), id => new_id);
                                    else -- new_ID (byte 2) is delayed!
                                        chan_regs(0) <= (data => bytes_r(3), id => old_id);
                                    end if;
                                    chan_regs(1) <= (data => bytes_r(4)(7 downto 1) & auxiliary_r(2), id => new_id);
                                    chan_regs(2) <= (data => bytes_r(5), id => new_id);

                                    old_id <= new_id;
                                    new_id <= bytes_r(6)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_r(3);
                                when "11" =>
                                    chan_regs(1) <= NULL_CHAN; -- byte 4 is ID
                                    chan_regs(3) <= NULL_CHAN; -- byte 6 is ID
                                    
                                    if NewID_isDelayed = '0' then -- Is new_ID delayed?
                                        chan_regs(0) <= (data => bytes_r(3), id => new_id);
                                    else -- new_ID is delayed!
                                        chan_regs(0) <= (data => bytes_r(3), id => old_id);
                                    end if;
                                    if auxiliary_r(2) = '0' then -- is byte 4 id delayed?
                                        chan_regs(2) <= (data => bytes_r(5), id => bytes_r(4)(7 downto 1));
                                    else -- byte 4 ID is delayed!
                                        chan_regs(2) <= (data => bytes_r(5), id => new_id);
                                    end if;
                                    
                                    old_id <= bytes_r(4)(7 downto 1);
                                    new_id <= bytes_r(6)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_r(3);
                                when others =>
                                    error <= '1';
                                end case;
                        state <= CYCLE_B;

                    when CYCLE_B => -- Bytes 7, 8, 9, 10
                    valid_output <= '1';
                                v_selector := bytes_r(10)(0) & bytes_r(8)(0);
                                case v_selector is
                                when "00" => 
                                    if NewID_isDelayed = '0' then -- Is the new_ID (byte 6) delayed ?
                                        chan_regs(0) <= (data => bytes_r(7), id => new_id);
                                    else
                                        chan_regs(0) <= (data => bytes_r(7), id => old_id);
                                        NewID_isDelayed <= '0'; 
                                    end if;
                                    chan_regs(1) <= (data => (bytes_r(8)(7 downto 1) & auxiliary_r(4)), id => new_id);
                                    chan_regs(2) <= (data => bytes_r(9), id => new_id);
                                    chan_regs(3) <= (data => (bytes_r(10)(7 downto 1) & auxiliary_r(5)), id => new_id);


                                when "01" =>
                                    chan_regs(1) <= NULL_CHAN; -- byte 8 is ID

                                    if NewID_isDelayed = '0' then -- is new_ID (byte 6) delayed?
                                        chan_regs(0) <= (data => bytes_r(7), id => new_id);
                                    else -- new_ID (byte 6) is delayed!
                                        chan_regs(0) <= (data => bytes_r(7), id => old_id);
                                    end if;
                                    if auxiliary_r(4) = '0' then -- is byte 8 ID immediate?
                                        chan_regs(2) <= (data => bytes_r(9), id => bytes_r(8)(7 downto 1));
                                    else -- byte 4 ID is delayed!
                                        chan_regs(2) <= (data => bytes_r(9), id => new_id);
                                    end if;
                                    chan_regs(3) <= (data => bytes_r(10)(7 downto 1) & auxiliary_r(5), id => bytes_r(8)(7 downto 1));
                                    
                                    old_id <= new_id;
                                    new_id <= bytes_r(8)(7 downto 1);
                                    NewID_isDelayed <= '0'; -- since we already have gone past the id's next byte, this must be 0.

                                when "10" =>
                                    chan_regs(3) <= NULL_CHAN; -- byte 10 is an id
                                    
                                    if NewID_isDelayed = '0' then -- is new_ID (byte 6) delayed?
                                        chan_regs(0) <= (data => bytes_r(7), id => new_id);
                                    else -- new_ID (byte 6) is delayed!
                                        chan_regs(0) <= (data => bytes_r(7), id => old_id);
                                    end if;
                                    chan_regs(1) <= (data => bytes_r(8)(7 downto 1) & auxiliary_r(4), id => new_id);
                                    chan_regs(2) <= (data => bytes_r(9), id => new_id);
                                    
                                    old_id <= new_id;
                                    new_id <= bytes_r(10)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_r(5);
                                when "11" =>
                                    chan_regs(1) <= NULL_CHAN; -- byte 8 is ID
                                    chan_regs(3) <= NULL_CHAN; -- byte 10 is ID

                                    if NewID_isDelayed = '0' then -- Is new_ID delayed?
                                        chan_regs(0) <= (data => bytes_r(7), id => new_id);
                                    else -- new_ID is delayed!
                                        chan_regs(0) <= (data => bytes_r(7), id => old_id);
                                    end if;
                                    if auxiliary_r(4) = '0' then -- is byte 8 id delayed?
                                        chan_regs(2) <= (data => bytes_r(9), id => bytes_r(8)(7 downto 1));
                                    else -- byte 8 ID is delayed!
                                        chan_regs(2) <= (data => bytes_r(9), id => new_id);
                                    end if;
                                    
                                    old_id <= bytes_r(8)(7 downto 1);
                                    new_id <= bytes_r(10)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_r(5);
                                when others =>
                                    error <= '1';
                                end case;
                        state <= CYCLE_C;

                    when CYCLE_C => -- Bytes 11, 12, 13, 14
                    valid_output <= '1';
                                v_selector := bytes_r(14)(0) & bytes_r(12)(0);
                                case v_selector is
                                when "00" => 
                                    if NewID_isDelayed = '0' then -- Is the new_ID (byte 10) delayed ?
                                        chan_regs(0) <= (data => bytes_r(11), id => new_id);
                                    else
                                        chan_regs(0) <= (data => bytes_r(11), id => old_id);
                                        NewID_isDelayed <= '0'; 
                                    end if;
                                    chan_regs(1) <= (data => (bytes_r(12)(7 downto 1) & auxiliary_r(6)), id => new_id);
                                    chan_regs(2) <= (data => bytes_r(13), id => new_id);
                                    chan_regs(3) <= (data => (bytes_r(14)(7 downto 1) & auxiliary_r(7)), id => new_id);


                                when "01" => -- hardest case iv had due to a impossible situation here!
                                    chan_regs(1) <= NULL_CHAN; -- byte 12 is ID

                                    if NewID_isDelayed = '0' then -- is new_ID (byte 10) delayed?
                                        chan_regs(0) <= (data => bytes_r(11), id => new_id);
                                    else -- new_ID (byte 6) is delayed!
                                        chan_regs(0) <= (data => bytes_r(11), id => old_id);                                    
                                    end if;
                                    if auxiliary_r(6) = '0' then -- is byte 12 ID delayed?
                                        chan_regs(2) <= (data => bytes_r(13), id => bytes_r(12)(7 downto 1));
                                    else -- byte 12 ID is delayed!
                                        chan_regs(2) <= (data => bytes_r(13), id => new_id);
                                    end if;
                                    chan_regs(3) <= (data => bytes_r(14)(7 downto 1) & auxiliary_r(7), id => bytes_r(12)(7 downto 1));

                                    old_id <= new_id;
                                    new_id <= bytes_r(12)(7 downto 1);
                                    NewID_isDelayed <= '0'; -- since we already have gone past the id's next byte, this must be 0.

                                when "10" =>
                                    chan_regs(3) <= NULL_CHAN; -- byte 10 is an id

                                    if NewID_isDelayed = '0' then -- is new_ID (byte 10) delayed?
                                        chan_regs(0) <= (data => bytes_r(11), id => new_id);
                                    else -- new_ID (byte 10) is delayed!
                                        chan_regs(0) <= (data => bytes_r(11), id => old_id);
                                    end if;
                                    chan_regs(1) <= (data => bytes_r(12)(7 downto 1) & auxiliary_r(6), id => new_id);
                                    chan_regs(2) <= (data => bytes_r(13), id => new_id);

                                    old_id <= new_id;------------------------------
                                    new_id <= bytes_r(14)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_r(7);
                                when "11" =>
                                    chan_regs(1) <= NULL_CHAN; -- byte 12 is ID
                                    chan_regs(3) <= NULL_CHAN; -- byte 14 is ID

                                    if NewID_isDelayed = '0' then -- Is new_ID delayed?
                                        chan_regs(0) <= (data => bytes_r(11), id => new_id);
                                    else -- new_ID is delayed!
                                        chan_regs(0) <= (data => bytes_r(11), id => old_id);
                                    end if;
                                    if auxiliary_r(6) = '0' then -- is byte 12 id delayed?
                                        chan_regs(2) <= (data => bytes_r(13), id => bytes_r(12)(7 downto 1));
                                    else -- byte 12 ID is delayed!
                                        chan_regs(2) <= (data => bytes_r(13), id => new_id);
                                    end if;

                                    old_id <= bytes_r(12)(7 downto 1);
                                    new_id <= bytes_r(14)(7 downto 1);
                                    NewID_isDelayed <= auxiliary_r(7);
                                when others =>
                                    error <= '1';
                                end case;
                        state <= IDLE;

                    when others => state <= IDLE;
                end case;
            end if;
        end if;
    end process;
     valid_f <= valid_output;
end Behavioral;

    


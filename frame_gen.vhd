library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity frame_gen is
    Port (
        ACLK      : in  STD_LOGIC;
        ARESETN         : in  STD_LOGIC;
        trace_data    : in  STD_LOGIC_VECTOR(31 downto 0);
        -- Output to frame_decoder
        frame     : out STD_LOGIC_VECTOR(127 downto 0);
        rd_en    : out STD_LOGIC
    );
end frame_gen;

architecture Behavioral of frame_gen is
    signal frame_reg : std_logic_vector(127 downto 0);
    constant SYNC_PATTERN : std_logic_vector(31 downto 0) := X"7FFFFFFF";
    constant HALFWORD     : std_logic_vector(31 downto 0) := X"7FFF7FFF";
    signal   count        : unsigned(1 downto 0)          := "00";
    signal   error        : STD_LOGIC                     := '0';
    signal begin_latch    : STD_LOGIC                     := '0';
begin


 
 frame <= frame_reg;
    process(ACLK)
    begin
        if rising_edge(ACLK) then
            if ARESETN = '0' then
                rd_en <= '0';
                count <= "00";
                error <= '0';
                begin_latch <= '0';
            else
                rd_en <= '0';

                if trace_data /= SYNC_PATTERN then -- Check for Full Frame Synch Packet
                    if trace_data /= HALFWORD then -- Check for halfwords (idle-patterns)
                    if begin_latch = '1' then -- before tpiu is configured it sends "00000001" words, the latch ignores them until the first sync frame is detected meaning the tpiu is turned on.
                        -- Shift existing data left and add the new word at the bottom
                        frame_reg <= trace_data & frame_reg(127 downto 32);

                        if count = "11" then
                            -- On the 4th word, a full frame has been gathered, push read signal 
                           rd_en <= '1';
                            count       <= "00";
                        else
                            count       <= count + 1; -- circular counter to check which 32-bit word we are at
                        end if;
                    end if; -- end latch check
                    end if; -- end halfword check
                else
                begin_latch <= '1'; -- first sync frame detected, TPIU is turned on.
                    if count /= "00" then
                        error <= '1'; -- synch packet appear inside frame, error!
                    else
                        error <= '0'; -- synchronized, reset the error flag!
                    end if; 
                end if; -- end frame synch
            end if; -- end reset
        end if; -- end clock
    end process;
end Behavioral;
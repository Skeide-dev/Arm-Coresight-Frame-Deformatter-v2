library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

entity bram_to_gen is
    Port (
        clk        : in  STD_LOGIC;
        aresetn        : in  STD_LOGIC;
        trace_data : in  STD_LOGIC_VECTOR(31 downto 0);
        trace_out  : out STD_LOGIC_VECTOR(31 downto 0)
    );
end bram_to_gen;

architecture Behavioral of bram_to_gen is

    type t_state is (IDLE, ACTIVE);
    signal state : t_state := IDLE;

begin

    process(clk)
    begin
        if rising_edge(clk) then
            if aresetn = '0' then
                state <= IDLE;
            else
            case state is

                when IDLE =>
                    trace_out <= x"7FFFFFFF";
                    state     <= ACTIVE;

                when ACTIVE =>
                    trace_out <= trace_data;

            end case;
        end if;
        end if;
    end process;

end Behavioral;
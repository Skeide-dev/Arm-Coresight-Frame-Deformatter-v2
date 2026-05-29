library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity axi_master is
    port (
        clk           : in  std_logic;
        resetn        : in  std_logic;
        -- Input channels from frame_decoder
        cpacked          : in  std_logic_vector(63 downto 0);
        valid_in       : in  std_logic;
        -- Control
        start_streaming   : in  std_logic;  -- from GPIO: stop filling, start streaming
                -- AXI4-Stream Master (to DMA S2MM)
        m_axis_tdata   : out std_logic_vector(63 downto 0);
        m_axis_tvalid  : out std_logic;
        m_axis_tready  : in  std_logic;
        m_axis_tkeep   : out std_logic_vector(7 downto 0);
        m_axis_tlast   : out std_logic
    );
end entity;

architecture rtl of axi_master is

    type abuffer_t is array (0 to 1023) of std_logic_vector(63 downto 0);
    signal abuffer : abuffer_t := (others => (others => '0'));  -- initialized to zero, no padding needed

    type state_t is (FILLING, STREAMING, DONE);
    signal state : state_t := FILLING;

    signal wr_ptr       : integer range 0 to 1024 := 0; -- 1024 to not cause error in code.
    signal rd_ptr       : integer range 0 to 1023 := 1;
    signal s_tvalid     : std_logic := '0';
    signal s_tlast      : std_logic := '0';

begin

    -- Static assignments
    m_axis_tkeep  <= (others => '1');  -- all bytes valid
    m_axis_tvalid <= s_tvalid;
    m_axis_tlast  <= s_tlast;
    process(clk)
    begin
        if rising_edge(clk) then
            if resetn = '0' then
                wr_ptr     <= 0;
                rd_ptr     <= 1;
                s_tvalid   <= '0';
                s_tlast    <= '0';
                state      <= FILLING;
                m_axis_tdata <= (others => '0');
            else
                case state is
                        when FILLING =>
                        s_tvalid <= '0';
                        s_tlast  <= '0';
                            if valid_in = '1' AND wr_ptr < 1024 then
                                abuffer(wr_ptr) <= cpacked;
                                wr_ptr <= wr_ptr + 1;
                            end if;
                            if start_streaming = '1' then
                            s_tvalid     <= '1';
                            m_axis_tdata <= abuffer(0);
                            state <= STREAMING;
                            end if;
                            
                        when STREAMING =>
                            s_tlast  <= '0';
                            s_tvalid     <= '1';
                            if m_axis_tready = '1' then
                            
                            m_axis_tdata <= abuffer(rd_ptr);
                                if rd_ptr = 1023 then
                                    s_tlast  <= '1'; -- last data
                                    state    <= DONE;
                                else
                                    rd_ptr   <= rd_ptr + 1;
                                end if;
                            end if;

                        
                        when DONE =>
                        if m_axis_tready = '1' then
                            s_tvalid <= '0';
                            s_tlast <= '0';
                        end if;
                        when others =>
                        state <= FILLING;

                end case;
            end if;
        end if;
    end process;
end architecture;

restart

set fp [open "/home/noctua1/project_1/fire_channels.csv" w]
puts $fp "Time_ns, Chan0, Chan1, Chan2, Chan3"


set step       8
set total      19000
set recording  0
set done       0
set prev_rd_en 0


for {set t 0} {$t <= $total && $done == 0} {incr t $step} {

    set rd_en     [get_value {/bramtrace_wrapper/bramtrace_i/frame_deformatter_0/rd_en}]
    set bram_addr [get_value -radix hex {/bramtrace_wrapper/bramtrace_i/bram_address_counter_0_bram_addr}]

    # Read 64-bit Channels_0 as hex, then convert
    set raw64 [get_value -radix hex {/bramtrace_wrapper/Channels_0}]
    # Remove any leading 0x if present
    set raw64 [string trim $raw64]
    set raw64 [regsub {^0x} $raw64 ""]
    # split into 4 x 16-bit fields
    set hex3 [string range $raw64 0 3]
    set hex2 [string range $raw64 4 7]
    set hex1 [string range $raw64 8 11]
    set hex0 [string range $raw64 12 15]

    # Detect rising edge of rd_en
    if {$recording == 0 && $prev_rd_en == 0 && $rd_en == 1} {
        set recording 1
        puts "✓ rd_en HIGH at ${t}ns — recording started"
    }

    # Write data only while recording
    if {$recording == 1} {
        # Write row to CSV
        puts $fp "$t, $hex0, $hex1, $hex2, $hex3"

        # Stop when bram_addr reaches 908
        if {$bram_addr == "908"} {
            set done 1
            puts "✓ bram_addr reached 908 at ${t}ns — recording stopped"
        }
    }

    set prev_rd_en $rd_en
    run ${step}ns
}

close $fp
puts "✓ CSV saved."

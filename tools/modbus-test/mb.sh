#!/bin/bash
# Modbus TCP client for Git Bash (or any bash with /dev/tcp): no Python, no
# installs. Uses only bash, printf, dd, od, awk and timeout.
#   mb.sh <ip> [port]            read the whole map and print it decoded
#   mb.sh <ip> [port] writetest  try a target write and the AUTO coil:
#                                read-only firmware answers exception 1
#   mb.sh <ip> [port] raw <pdu>  send one PDU in hex (e.g. 03 00 0a 00 02)
#                                and print the reply PDU in hex
# Register map: docs/modbus-register-map.md.
IP=${1:?usage: mb.sh <ip> [port] [writetest | raw <hex bytes>]}
shift
PORT=502
if [[ $1 =~ ^[0-9]+$ ]]; then PORT=$1; shift; fi
CMD=${1:-map}
shift

exec 3<>"/dev/tcp/$IP/$PORT" || { echo "cannot connect to $IP:$PORT"; exit 1; }
TID=0

# Exactly n bytes from the socket as hex pairs, or nothing after 3 s.
readn() { timeout 3 dd bs=1 count="$1" <&3 2>/dev/null | od -An -v -tx1 | tr -s ' \n' ' '; }

# req <hex bytes of the PDU>: prints the reply PDU as hex bytes.
req() {
    local pdu=($*) n=$(( $# + 1 )) out="" b
    TID=$(( (TID + 1) & 0xffff ))
    for b in $(printf '%02x %02x 00 00 %02x %02x 01' $((TID >> 8)) $((TID & 255)) $((n >> 8)) $((n & 255))) "${pdu[@]}"; do
        out+="\\x$b"
    done
    printf "$out" >&3
    local hdr=($(readn 7))
    if [ ${#hdr[@]} -ne 7 ]; then echo "no reply"; return 1; fi
    local len=$(( 0x${hdr[4]} * 256 + 0x${hdr[5]} - 1 ))
    readn "$len"
    echo
}

# Exception check: prints "exception N" and returns 1 for an error reply.
ex() {
    local r=($1)
    if (( 0x${r[0]} & 0x80 )); then echo "exception $(( 0x${r[1]} ))"; return 1; fi
    return 0
}

case $CMD in
raw)
    req "$@"
    ;;
writetest)
    # Target 4.8 mbar (0x4099999a) to registers 12-13, then AUTO coil on.
    r=$(req 10 00 0c 00 02 04 40 99 99 9a); printf 'write target 4.8: %s  ' "$r"; ex "$r" && echo "accepted"
    r=$(req 05 00 00 ff 00);                printf 'AUTO coil on:     %s  ' "$r"; ex "$r" && echo "accepted"
    r=$(req 04 00 00 00 01);                printf 'FC4 (unsupported): %s  ' "$r"; ex "$r" && echo "accepted"
    r=$(req 03 00 14 00 0a);                printf 'read past the map: %s  ' "$r"; ex "$r" && echo "accepted"
    ;;
map)
    regs=$(req 03 00 00 00 1a) || exit 1
    ex "$regs" || exit 1
    coils=$(req 01 00 00 00 04)
    echo "$regs" | awk -v coils="$coils" '
    function hex(h,   i, v) { v = 0; h = tolower(h)
        for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
        return v }
    function reg(i) { return hex($(3 + 2 * i) $(4 + 2 * i)) }
    function f32(i,   u, s, e, m, v) { u = reg(i) * 65536 + reg(i + 1)
        s = int(u / 2147483648); e = int(u / 8388608) % 256; m = u % 8388608
        if (e == 255) return "nan"
        v = (e == 0) ? m * 2 ^ -149 : (1 + m / 8388608) * 2 ^ (e - 127)
        return sprintf("%.3f", s ? -v : v) }
    {
        split("NOT HOMED,HOMING,MANUAL,AUTO", mode, ",")
        split("reading OK,position set,homed,moving,closed switch pressed,fault,warning,target set,remote writes on,motor supply on", sn, ",")
        printf "map version %d, heartbeat %d, mode %s\n", reg(0), reg(1), (reg(2) < 4 ? mode[reg(2) + 1] : reg(2))
        s = ""; for (b = 0; b < 10; b++) if (int(reg(3) / 2 ^ b) % 2) s = s (s ? ", " : "") sn[b + 1]
        printf "status: %s\n", s ? s : "none"
        printf "regulation %d, faults 0x%04x, warnings 0x%04x, fault count %d, events %d\n", reg(4), reg(5), reg(6), reg(7), reg(8)
        printf "pressure      %s mbar\n", f32(10)
        printf "target        %s mbar\n", f32(12)
        printf "valve         %s %%\n", f32(14)
        printf "valve target  %s %%\n", f32(16)
        printf "range         %s mbar\n", f32(18)
        printf "excitation    %s V\n", f32(20)
        printf "temperature   %s C\n", f32(22)
        printf "uptime        %d s\n", reg(24) * 65536 + reg(25)
        split(coils, c, " ")
        printf "coils 0-3 (AUTO, STOP, RESET, OK): %d%d%d%d\n", hex(c[3]) % 2, int(hex(c[3]) / 2) % 2, int(hex(c[3]) / 4) % 2, int(hex(c[3]) / 8) % 2
    }'
    ;;
*)
    echo "unknown command $CMD"; exit 1
    ;;
esac
exec 3<&-

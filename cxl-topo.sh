#!/bin/bash
#
# cxl-topo.sh — Dump CXL topology as a tree from sysfs
#

CXL=/sys/bus/cxl/devices

[ -d "$CXL" ] || { echo "No CXL bus found"; exit 1; }

hex_to_dec()
{
    printf '%u' "$1" 2>/dev/null
}

hex_gt()
{
    local a b
    a=$(printf '%032x' "$1" 2>/dev/null) || return 1
    b=$(printf '%032x' "$2" 2>/dev/null) || return 1
    [ "$a" \> "$b" ]
}

hex_eq()
{
    local a b
    a=$(printf '%032x' "$1" 2>/dev/null) || return 1
    b=$(printf '%032x' "$2" 2>/dev/null) || return 1
    [ "$a" = "$b" ]
}

human_size()
{
    local s
    s=$(hex_to_dec "$1") || return
    [ "$s" = "0" ] && return
    if [ "${#s}" -ge 10 ] || [ "$s" -ge 1073741824 ] 2>/dev/null; then
        printf "%d.%d GiB" $((s/1073741824)) $((s%1073741824*10/1073741824))
    else
        printf "%d MiB" $((s/1048576))
    fi
}

T()
{
    local i
    for ((i = 0; i < $1; i++)); do printf "   "; done
    printf "└─ %s\n" "$2"
}

dport_target()
{
    local t
    t=$(readlink "$1" 2>/dev/null)
    [ -z "$t" ] && return
    local bdf
    bdf=$(echo "$t" | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | tail -1)
    if [ -n "$bdf" ]; then
        echo "$bdf"
    else
        basename "$t"
    fi
}

cxl_fbsta()
{
    local bdf=$1
    lspci -vvv -s "$bdf" 2>/dev/null | awk '
    /Flex Bus Port/ { fb=1 }
    fb && /FBSta:/ {
        s = ""
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^(Cache|IO|Mem)[+-]$/) s = s $i " "
        }
        sub(/ $/, "", s); print s; exit
    }
    '
}

dec_desc()
{
    local d=$1
    local dt sz st tt md g
    dt=$(cat "$d/devtype" 2>/dev/null)
    sz=$(cat "$d/size" 2>/dev/null); sz=$(hex_to_dec "$sz")
    st=$(cat "$d/start" 2>/dev/null)
    tt=$(cat "$d/target_type" 2>/dev/null)
    md=$(cat "$d/mode" 2>/dev/null)
    g=$(human_size "$sz")

    printf "%s (" "$(basename "$d")"
    case "$dt" in
        *root*)     printf "root"
                    if [ -n "$g" ] && [ -n "$st" ]; then
                        printf ", %s @ %s" "$g" "$st"
                    elif [ -n "$g" ]; then
                        printf ", %s" "$g"
                    fi
                    printf ")"
                    # Check if start matches a CFMWS base
                    local c
                    for c in "${cfmws_bases[@]}"; do
                        [ "$c" = "$st" ] && printf " <- CFMWS" && break
                    done
                    return ;;
        *switch*)   printf "switch"
                    [ "$sz" -eq 0 ] && printf ", disabled" || { [ -n "$g" ] && printf ", %s" "$g"; }
                    [ -n "$tt" ] && printf ", %s" "$tt" ;;
        *endpoint*) printf "endpoint"
                    [ -n "$g" ] && printf ", %s" "$g"
                    [ -n "$md" ] && printf ", %s" "$md" ;;
    esac
    printf ")"
}

memdev_for_bdf()
{
    local bdf=$1
    for md in "$CXL"/mem*; do
        [ -d "$md" ] || continue
        local mbdf
        mbdf=$(readlink "$md" 2>/dev/null | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | tail -1)
        [ "$mbdf" = "$bdf" ] && basename "$md" && return
    done
}

# Walk a port/root directory. Chain: dport -> decoder -> subport -> recurse
walk()
{
    local real=$1 depth=$2

    for dp in "$real"/dport*; do
        [ -d "$dp" ] || continue
        local dpn dpid tgt
        dpn=$(basename "$dp")
        dpid=$(cat "$dp/id" 2>/dev/null)
        [ -z "$dpid" ] && dpid=${dpn#dport}
        tgt=$(dport_target "$dp")

        # Skip dports with no matching decoder and no endpoints
        local has_dec=0
        for dec in "$real"/decoder*; do
            [ -d "$dec" ] || continue
            local tl
            tl=$(cat "$dec/target_list" 2>/dev/null)
            echo ",$tl," | grep -q ",$dpid," && has_dec=1 && break
        done
        if [ "$has_dec" = 0 ]; then
            local has_ep=0
            for ep in "$real"/endpoint*; do
                [ -d "$ep" ] || continue
                local ep_pdp
                ep_pdp=$(readlink -f "$ep/parent_dport" 2>/dev/null)
                [ "$ep_pdp" = "$(readlink -f "$dp")" ] && has_ep=1 && break
            done
            [ "$has_ep" = 0 ] && continue
        fi

        local fb=""
        case "$tgt" in *:*.*) fb=$(cxl_fbsta "$tgt") ;; esac
        T "$depth" "$dpn -> ${tgt:-(?)}${fb:+ ($fb)}"

        # PCI device(s) behind bridge (once per dport)
        show_pci_children "$tgt" $((depth + 1))

        # Print matching decoders; show endpoints under active ones
        local ep_shown=0
        for dec in "$real"/decoder*; do
            [ -d "$dec" ] || continue
            local tl dec_sz
            tl=$(cat "$dec/target_list" 2>/dev/null)
            echo ",$tl," | grep -q ",$dpid," || continue
            dec_sz=$(cat "$dec/size" 2>/dev/null)
            T $((depth + 1)) "$(dec_desc "$dec")"

            # Show endpoints belonging to this dport under the first active decoder
            if [ "$ep_shown" = 0 ] && [ "$dec_sz" != "0" ] && [ "$dec_sz" != "0x0" ]; then
                for ep in "$real"/endpoint*; do
                    [ -d "$ep" ] || continue
                    # Filter: only show endpoints whose parent_dport matches this dport
                    local ep_pdp
                    ep_pdp=$(readlink -f "$ep/parent_dport" 2>/dev/null)
                    [ "$ep_pdp" = "$(readlink -f "$dp")" ] || continue
                    local epn ebdf mem ep_numa
                    epn=$(basename "$ep")
                    # Get device BDF and memdev from the endpoint's uport symlink
                    ebdf=""
                    mem=""
                    local uport_path
                    uport_path=$(readlink "$ep/uport" 2>/dev/null)
                    if [ -n "$uport_path" ]; then
                        ebdf=$(echo "$uport_path" | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | tail -1)
                        mem=$(basename "$uport_path")
                    fi
                    ep_numa=""
                    if [ -n "$ebdf" ]; then
                        local ep_full
                        case "$ebdf" in *:*:*.*) ep_full=$ebdf ;; *) ep_full="0000:$ebdf" ;; esac
                        ep_numa=$(cat "/sys/bus/pci/devices/$ep_full/numa_node" 2>/dev/null)
                    fi
                    T $((depth + 2)) "$epn${mem:+ / $mem} (${ebdf:--})${ep_numa:+ node=$ep_numa}"
                    for edec in "$ep"/decoder*; do
                        [ -d "$edec" ] || continue
                        T $((depth + 3)) "$(dec_desc "$edec")"
                    done
                done
                ep_shown=1
            fi
        done

        # Show endpoints for this dport even if no active decoder
        if [ "$ep_shown" = 0 ]; then
            for ep in "$real"/endpoint*; do
                [ -d "$ep" ] || continue
                local ep_pdp
                ep_pdp=$(readlink -f "$ep/parent_dport" 2>/dev/null)
                [ "$ep_pdp" = "$(readlink -f "$dp")" ] || continue
                local epn ebdf mem ep_numa
                epn=$(basename "$ep")
                ebdf=""
                mem=""
                local uport_path
                uport_path=$(readlink "$ep/uport" 2>/dev/null)
                if [ -n "$uport_path" ]; then
                    ebdf=$(echo "$uport_path" | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | tail -1)
                    mem=$(basename "$uport_path")
                fi
                ep_numa=""
                if [ -n "$ebdf" ]; then
                    local ep_full
                    case "$ebdf" in *:*:*.*) ep_full=$ebdf ;; *) ep_full="0000:$ebdf" ;; esac
                    ep_numa=$(cat "/sys/bus/pci/devices/$ep_full/numa_node" 2>/dev/null)
                fi
                T $((depth + 1)) "$epn${mem:+ / $mem} (${ebdf:--})${ep_numa:+ node=$ep_numa}"
                for edec in "$ep"/decoder*; do
                    [ -d "$edec" ] || continue
                    T $((depth + 2)) "$(dec_desc "$edec")"
                done
            done
        fi

        # Match sub-ports by PCI segment (pciDDDD:BB -> DDDD:BB)
        local root_seg=""
        case "$tgt" in pci*) root_seg=${tgt#pci} ;; esac

        for port in "$real"/port*; do
            [ -d "$port" ] || continue
            # Skip ports with no dports (empty host bridges)
            ls "$port"/dport* >/dev/null 2>&1 || continue

            if [ -n "$root_seg" ]; then
                local matched=0 sub_dp sub_tgt sub_seg
                for sub_dp in "$port"/dport*; do
                    [ -d "$sub_dp" ] || continue
                    sub_tgt=$(dport_target "$sub_dp")
                    sub_seg=${sub_tgt%:*.*}
                    [ "$sub_seg" = "$root_seg" ] && matched=1 && break
                done
                [ "$matched" = 0 ] && continue
            fi

            T $((depth + 2)) "$(basename "$port")"
            walk "$port" $((depth + 3))
        done
    done

    # Show unconfigured decoders not claimed by any displayed dport
    for dec in "$real"/decoder*; do
        [ -d "$dec" ] || continue
        local tl dec_sz
        tl=$(cat "$dec/target_list" 2>/dev/null)
        dec_sz=$(cat "$dec/size" 2>/dev/null)
        [ "$dec_sz" != "0" ] && [ "$dec_sz" != "0x0" ] && continue
        local claimed=0
        for dp in "$real"/dport*; do
            [ -d "$dp" ] || continue
            local dpid
            dpid=$(cat "$dp/id" 2>/dev/null)
            [ -z "$dpid" ] && dpid=${dp##*dport}
            echo ",$tl," | grep -q ",$dpid," && claimed=1 && break
        done
        [ "$claimed" = 1 ] && continue
        T "$depth" "$(dec_desc "$dec") (unconfigured)"
    done
}

# Parse DVSEC ranges from lspci (BDF -> range line)
nr_dvsec=0
if command -v lspci >/dev/null 2>&1; then
    while IFS='|' read -r bdf range; do
        dvsec_bdf[$nr_dvsec]=$bdf
        dvsec_range[$nr_dvsec]=$range
        nr_dvsec=$((nr_dvsec + 1))
    done < <(lspci -vvv 2>/dev/null | awk '
    /^[0-9a-f]+:/ { bdf=$1 }
    /PCIe DVSEC for CXL Devices/ { d=1 }
    d && /Range[0-9]+:/ && !/size=0x0\]/ {
        sub(/^[ \t]+/, ""); print bdf "|" $0
    }
    d && /^[^ \t]/ && !/PCIe DVSEC/ { d=0 }
    ')
fi

# Show DVSEC ranges for devices behind a dport's bridge
show_pci_children()
{
    local bridge_bdf=$1 depth=$2 full_bridge child short_bdf desc i
    [ -z "$bridge_bdf" ] && return
    case "$bridge_bdf" in
        *:*:*.*) full_bridge=$bridge_bdf ;;
        *:*.*) full_bridge="0000:$bridge_bdf" ;;
        *) return ;;
    esac
    for child in /sys/bus/pci/devices/$full_bridge/????:??:??.?; do
        [ -d "$child" ] || continue
        local child_bdf
        child_bdf=$(basename "$child")
        short_bdf=${child_bdf#0000:}
        desc=$(lspci -s "$child_bdf" 2>/dev/null | sed "s/^$short_bdf //")
        local cfb drv child_numa
        cfb=$(cxl_fbsta "$child_bdf")
        drv=""
        if [ -e "/sys/bus/pci/devices/$child_bdf/driver" ]; then
            drv=$(basename "$(readlink "/sys/bus/pci/devices/$child_bdf/driver")")
        fi
        child_numa=$(cat "/sys/bus/pci/devices/$child_bdf/numa_node" 2>/dev/null)
        T "$depth" "$short_bdf ${desc:-(unknown)}${cfb:+ ($cfb)}${drv:+ ($drv)}${child_numa:+ node=$child_numa}"
        # Show DVSEC ranges for this device
        for ((i = 0; i < nr_dvsec; i++)); do
            if [ "${dvsec_bdf[$i]}" = "$short_bdf" ] || [ "${dvsec_bdf[$i]}" = "$child_bdf" ]; then
                T $((depth + 1)) "DVSEC ${dvsec_range[$i]}"
            fi
        done
    done
}

# Collect CFMWS bases from root decoder start addresses
cfmws_bases=()
for dec in "$CXL"/decoder0.*; do
    [ -d "$dec" ] || continue
    st=$(cat "$dec/start" 2>/dev/null)
    [ -n "$st" ] && [ "$st" != "0x0" ] && [ "$st" != "0" ] && cfmws_bases+=("$st")
done

echo "=== CXL Topology ==="
echo

for root in "$CXL"/root*; do
    [ -d "$root" ] || continue
    echo "$(basename "$root") (ACPI.CXL)"
    walk "$(readlink -f "$root")" 0
done

# Regions
echo
echo "Regions:"
found=0
for reg in "$CXL"/region*; do
    [ -d "$reg" ] || continue; found=1
    sz=$(cat "$reg/size" 2>/dev/null)
    g=$(human_size "$((sz))")
    echo "  $(basename "$reg"): ${g:-?} @ $(cat "$reg/resource" 2>/dev/null) mode=$(cat "$reg/mode" 2>/dev/null)"
done
[ "$found" = 0 ] && echo "  (none)"

# Sanity checks
warnings=()

# Check each switch decoder's target_type against device FBSta
for root in "$CXL"/root*; do
    [ -d "$root" ] || continue
    rp=$(readlink -f "$root")
    for port in "$rp"/port*; do
        [ -d "$port" ] || continue
        for dec in "$port"/decoder*; do
            [ -d "$dec" ] || continue
            local_tt=$(cat "$dec/target_type" 2>/dev/null)
            [ -z "$local_tt" ] && continue
            # Find device behind this port's dport
            for dp in "$port"/dport*; do
                [ -d "$dp" ] || continue
                local_tgt=$(dport_target "$dp")
                case "$local_tgt" in *:*.*) ;; *) continue ;; esac
                # Check child devices
                case "$local_tgt" in
                    *:*:*.*) local_full=$local_tgt ;;
                    *:*.*) local_full="0000:$local_tgt" ;;
                esac
                for child in /sys/bus/pci/devices/$local_full/????:??:??.?; do
                    [ -d "$child" ] || continue
                    local_cbdf=$(basename "$child")
                    local_short=${local_cbdf#0000:}
                    local_fb=$(cxl_fbsta "$local_cbdf")
                    [ -z "$local_fb" ] && continue
                    # Type-2 = Cache+ Mem+, target_type should be accelerator
                    if echo "$local_fb" | grep -q 'Cache+' && echo "$local_fb" | grep -q 'Mem+'; then
                        if [ "$local_tt" = "expander" ]; then
                            warnings+=("$(basename "$dec"): target_type 'expander' but $local_short is Type-2 ($local_fb)")
                        fi
                    fi
                    # CXL.cache mismatch: device supports Cache+ but root port doesn't
                    local_rp_fb=$(cxl_fbsta "$local_tgt")
                    if echo "$local_fb" | grep -q 'Cache+' && echo "$local_rp_fb" | grep -q 'Cache-'; then
                        warnings+=("$local_short: CXL.cache not negotiated on root port ($local_rp_fb)")
                    fi
                done
            done
        done
    done
done

# DVSEC Range vs CFMWS mismatch
for ((i = 0; i < nr_dvsec; i++)); do
    local_range=${dvsec_range[$i]}
    local_bdf=${dvsec_bdf[$i]}
    # Extract range base address (16-digit hex without 0x prefix)
    local_rbase=$(echo "$local_range" | grep -oiE '[0-9a-f]{16}' | head -1)
    [ -z "$local_rbase" ] && continue
    local_rbase_hex="0x$local_rbase"
    local_match=0
    for c in "${cfmws_bases[@]}"; do
        if hex_eq "$local_rbase_hex" "$c"; then
            local_match=1
            break
        fi
    done
    if [ "$local_match" = 0 ]; then
        warnings+=("$local_bdf: DVSEC Range base $local_rbase_hex != any CFMWS base")
    fi
done

# ATS/PASID status for CXL devices
for ((i = 0; i < nr_dvsec; i++)); do
    local_bdf=${dvsec_bdf[$i]}
    case "$local_bdf" in
        *:*:*.*) local_full=$local_bdf ;;
        *:*.*) local_full="0000:$local_bdf" ;;
        *) continue ;;
    esac
    local_ats=$(lspci -vvv -s "$local_bdf" 2>/dev/null | awk '/ATS:/{for(j=1;j<=NF;j++) if($j~/^(Enable|STU)/) printf "%s ", $j; print ""; exit}')
    local_pasid=$(lspci -vvv -s "$local_bdf" 2>/dev/null | awk '/PASIDCap/{for(j=1;j<=NF;j++) if($j~/^(Enable|Width)/) printf "%s ", $j; print ""; exit}')
    if [ -n "$local_ats" ] || [ -n "$local_pasid" ]; then
        [ -n "$local_ats" ] && echo "$local_ats" | grep -q 'Enable-' && \
            warnings+=("$local_bdf: ATS not enabled")
        [ -n "$local_pasid" ] && echo "$local_pasid" | grep -q 'Enable-' && \
            warnings+=("$local_bdf: PASID not enabled")
    fi
done


# Decoder chain validation: root -> switch -> endpoint sizes
for root in "$CXL"/root*; do
    [ -d "$root" ] || continue
    rp=$(readlink -f "$root")
    for rdec in "$rp"/decoder*; do
        [ -d "$rdec" ] || continue
        local_rsz=$(cat "$rdec/size" 2>/dev/null)
        [ "$local_rsz" = "0" ] || [ "$local_rsz" = "0x0" ] && continue
        local_rname=$(basename "$rdec")
        # Check switch decoders under sub-ports
        for port in "$rp"/port*; do
            [ -d "$port" ] || continue
            for sdec in "$port"/decoder*; do
                [ -d "$sdec" ] || continue
                local_ssz=$(cat "$sdec/size" 2>/dev/null)
                local_sname=$(basename "$sdec")
                # Active root but disabled switch = broken chain
                if [ "$local_ssz" = "0" ] || [ "$local_ssz" = "0x0" ]; then
                    # Only warn if there's no endpoint (dead end)
                    ls "$port"/endpoint* >/dev/null 2>&1 || \
                        warnings+=("$local_sname: disabled in chain below $local_rname")
                elif hex_gt "$local_ssz" "$local_rsz"; then
                    warnings+=("$local_sname: size > parent $local_rname")
                fi
            done
        done
    done
done

# QoS class mismatch on regions
for reg in "$CXL"/region*; do
    [ -d "$reg" ] || continue
    local_qos=$(cat "$reg/qos_class_mismatch" 2>/dev/null)
    if [ "$local_qos" = "true" ] || [ "$local_qos" = "1" ]; then
        warnings+=("$(basename "$reg"): qos_class_mismatch")
    fi
done

echo
echo "Warnings:"
if [ ${#warnings[@]} -eq 0 ]; then
    echo "  (none)"
else
    for w in "${warnings[@]}"; do
        echo "  $w"
    done
fi

echo
nr_ep=$(ls -d "$CXL"/endpoint* 2>/dev/null | wc -l)
nr_mem=$(ls -d "$CXL"/mem* 2>/dev/null | wc -l)
nr_reg=$(ls -d "$CXL"/region* 2>/dev/null | wc -l)
echo "=== endpoints=$nr_ep memdevs=$nr_mem regions=$nr_reg ==="

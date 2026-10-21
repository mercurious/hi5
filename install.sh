#!/usr/bin/env bash
# Install or update HI5 on a Miyoo Mini Plus running OnionOS. Run it on a computer on the
# same network as the Miyoo:
#
#   curl -fsSL https://raw.githubusercontent.com/mercurious/hi5/main/install.sh | bash
#
# It finds the Miyoo over SSH (turn SSH on in Onion: Apps > Tweaks > Network), compares the
# installed version with the latest GitHub release, and copies the new build to
# SD:/App/HI5 if the release is newer. Your hi5.sav and samples stay where they are.
#
#   HI5_HOST=192.168.1.28   skip the scan and use this address
#   HI5_USER=onion          SSH user (Onion's default login is onion / onion)
#   HI5_FORCE=1             reinstall even if the Miyoo already has the latest version
#
# The address that worked is saved in ~/.config/hi5/host and tried first next time.

set -euo pipefail

REPO=mercurious/hi5
APPS=/mnt/SDCARD/App
SSH_USER=${HI5_USER:-onion}
SAVED=${XDG_CONFIG_HOME:-$HOME/.config}/hi5/host

say() { printf 'hi5: %s\n' "$*" >&2; }
die() { say "$*"; exit 1; }

# print "IP BANNER" if IP answers on port 22 with an SSH banner
probe() {
    local banner=""
    { exec 3<>"/dev/tcp/$1/22"; } 2>/dev/null || return 0
    IFS= read -r -t 2 banner <&3 || true
    exec 3<&-
    [[ $banner == SSH-* ]] && printf '%s %s\n' "$1" "${banner%$'\r'}"
    return 0
}

# probe one address, giving up after 3 s (a host on another network can take minutes to time out)
probe_quick() {
    local i pid
    probe "$1" >"$tmp/one" &
    pid=$!
    for i in $(seq 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    { kill "$pid" || true; wait "$pid" || true; } 2>/dev/null
    cat "$tmp/one"
}

local_ip() {
    if command -v ip >/dev/null; then
        ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p'
    else
        local ifc
        ifc=$(route -n get default 2>/dev/null | awk '/interface:/ {print $2}')
        [ -n "$ifc" ] && ipconfig getifaddr "$ifc"
    fi
}

# probe every address on this machine's /24 at once; Onion's sshd is dropbear, which sets
# the Miyoo apart from the OpenSSH hosts on a typical home network
scan() {
    local me prefix i line found=()
    me=$(local_ip || true)
    [ -n "$me" ] || die "can't work out this computer's LAN address; set HI5_HOST=<miyoo ip>"
    prefix=${me%.*}
    say "looking for the Miyoo on $prefix.0/24..."
    for i in $(seq 1 254); do
        [ "$prefix.$i" = "$me" ] || probe "$prefix.$i" &
    done >"$tmp/scan"
    sleep 3
    { kill $(jobs -p) || true; wait; } 2>/dev/null
    while read -r line; do found+=("$line"); done < <(grep -i dropbear "$tmp/scan" | sort -t. -k4 -n)

    if [ ${#found[@]} -eq 0 ]; then
        say "no Miyoo answered on $prefix.0/24."
        say "turn on SSH in Onion (Apps > Tweaks > Network > SSH), or set HI5_HOST=<miyoo ip>"
        [ -s "$tmp/scan" ] && say "other SSH hosts: $(cut -d' ' -f1 "$tmp/scan" | tr '\n' ' ')"
        exit 1
    elif [ ${#found[@]} -eq 1 ]; then
        host=${found[0]%% *}
    else
        say "more than one candidate:"
        for i in "${!found[@]}"; do say "  $((i + 1))) ${found[$i]}"; done
        read -r -p "hi5: which one? " i </dev/tty
        [[ $i =~ ^[0-9]+$ ]] && [ "$i" -ge 1 ] && [ "$i" -le ${#found[@]} ] || die "no such choice"
        host=${found[$((i - 1))]%% *}
    fi
}

# true if dotted version $1 is newer than $2
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        n = split(a, x, "."); m = split(b, y, ".")
        for (i = 1; i <= (n > m ? n : m); i++) {
            if (x[i] + 0 > y[i] + 0) exit 0
            if (x[i] + 0 < y[i] + 0) exit 1
        }
        exit 1 }'
}

on_miyoo() {
    ssh -o ControlMaster=auto -o ControlPath="$tmp/ssh" -o ControlPersist=120 \
        -o StrictHostKeyChecking=accept-new -o HostKeyAlgorithms=+ssh-rsa \
        -o ConnectTimeout=5 "$SSH_USER@$host" "$@"
}

main() {
    local c latest installed

    for c in curl unzip ssh tar awk; do
        command -v "$c" >/dev/null || die "this needs $c"
    done

    tmp=$(mktemp -d)
    trap 'ssh -o ControlPath="$tmp/ssh" -O exit miyoo 2>/dev/null || true; rm -rf "$tmp"' EXIT

    latest=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
        sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1) || true
    [ -n "$latest" ] || die "couldn't read the latest release from github.com/$REPO"
    say "latest release: v$latest"

    host=${HI5_HOST:-}
    if [ -z "$host" ] && [ -s "$SAVED" ]; then
        host=$(cat "$SAVED")
        if [ -n "$(probe_quick "$host")" ]; then
            say "using the saved address $host"
        else
            say "saved address $host isn't answering"
            host=
        fi
    fi
    [ -n "$host" ] || scan

    say "connecting to $SSH_USER@$host (Onion's default password is onion)"
    installed=$(on_miyoo "test -d $APPS || exit 3; cat $APPS/HI5/VERSION 2>/dev/null; true" </dev/null) ||
        die "couldn't log in to $host, or it has no $APPS (is it the OnionOS card?)"
    mkdir -p "$(dirname "$SAVED")" && echo "$host" >"$SAVED"

    if [ -z "$installed" ]; then
        say "the Miyoo has no versioned HI5 yet"
    elif [ "${HI5_FORCE:-}" != 1 ] && ! newer "$latest" "$installed"; then
        say "the Miyoo already has v$installed, nothing to do (HI5_FORCE=1 reinstalls)"
        return
    else
        say "the Miyoo has v$installed"
    fi

    if on_miyoo "pidof hi5" </dev/null >/dev/null; then
        die "HI5 is running on the Miyoo; quit it with MENU and run this again"
    fi

    say "downloading v$latest..."
    curl -fsSL -o "$tmp/HI5.zip" "https://github.com/$REPO/releases/download/v$latest/HI5.zip"
    unzip -q "$tmp/HI5.zip" -d "$tmp/zip"
    [ -f "$tmp/zip/HI5/hi5" ] || die "the release zip has no HI5/hi5"

    say "copying to $host:$APPS/HI5..."
    (cd "$tmp/zip" && COPYFILE_DISABLE=1 tar --format=ustar -cf - HI5) |
        on_miyoo "tar -xf - -C $APPS && sync"
    say "HI5 v$latest is installed. Launch it from Apps."
}

# everything runs from here, so bash has read the whole script before `curl | bash` starts it
main "$@"

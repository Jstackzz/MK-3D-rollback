#!/bin/sh
# Installs the MK3D Online server as an always-on service on a Linux machine with a public address (an Oracle Cloud
# VM, a VPS: Ubuntu, Debian, Fedora...). From Windows, tools\deploy-server.ps1 copies server/ over and runs this; on
# the machine itself, run it as root from the repository:
#
#     sudo sh server/deploy/install.sh [name-or-address-players-use [oldest-launcher-version]]
#
# It copies server/ to /opt/mk3d, keeps the database and certificate in /var/lib/mk3d, makes the certificate the
# first time, runs the server under systemd as the user mk3d (restarted if it stops, started at boot, logging to the
# journal), and opens TCP 7777 and UDP 7778 in the machine's own firewall: ufw or firewalld when one is active, and
# the iptables rules Oracle Cloud's Ubuntu images keep in /etc/iptables/rules.v4 (they let in only SSH). Running it
# again updates the server and keeps the certificate and the accounts. The cloud's network firewall (Oracle's
# security list, a security group) must allow the same two ports. Given an oldest launcher version (after a release
# every player must take), the server tells older launchers to update; it is kept in /var/lib/mk3d/min-client for
# later runs.
set -eu

repo=$(cd "$(dirname "$0")/../.." && pwd)
data=/var/lib/mk3d
app=/opt/mk3d

if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then
    echo "The server needs Python 3.11 or newer as python3 (Ubuntu 24.04 has 3.12)." >&2
    exit 1
fi
id mk3d >/dev/null 2>&1 || useradd --system --home-dir "$data" --shell /usr/sbin/nologin mk3d
mkdir -p "$data" "$app"
if [ -n "${2:-}" ]; then
    if ! printf '%s\n' "$2" | grep -Eqx '[0-9]+(\.[0-9]+)*'; then
        echo "An oldest launcher version looks like 0.1.4, not $2." >&2
        exit 1
    fi
    echo "$2" > "$data/min-client"
fi
min_client=$(cat "$data/min-client" 2>/dev/null || echo 0.1.0)
rm -rf "$app/server"
cp -r "$repo/server" "$app/server"
rm -rf "$app/server/tests" "$app/server/deploy"
if [ ! -f "$data/server-cert.pem" ]; then
    (cd "$app" && python3 -m server.makecert --out-dir "$data" ${1:+--host "$1"})
fi
chown -R mk3d: "$data"
chmod 600 "$data/server-key.pem"

cat > /etc/systemd/system/mk3d-server.service <<EOF
[Unit]
Description=MK3D Online server
After=network-online.target
Wants=network-online.target

[Service]
User=mk3d
WorkingDirectory=$app
ExecStart=/usr/bin/env python3 -m server --host 0.0.0.0 --port 7777 --udp-port 7778 --db $data/server.sqlite --min-client $min_client
Restart=always
RestartSec=3
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=full
ProtectHome=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable mk3d-server >/dev/null
systemctl restart mk3d-server

if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    ufw allow 7777/tcp >/dev/null && ufw allow 7778/udp >/dev/null
fi
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --quiet --permanent --add-port=7777/tcp --add-port=7778/udp && firewall-cmd --quiet --reload
fi
if [ -f /etc/iptables/rules.v4 ] && command -v iptables >/dev/null 2>&1; then
    # ahead of the rule that rejects everything else, then saved so a reboot keeps them
    iptables -C INPUT -p tcp --dport 7777 -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport 7777 -j ACCEPT
    iptables -C INPUT -p udp --dport 7778 -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport 7778 -j ACCEPT
    if command -v netfilter-persistent >/dev/null 2>&1; then
        netfilter-persistent save >/dev/null 2>&1
    else
        iptables-save > /etc/iptables/rules.v4
    fi
fi

sleep 2
if ! systemctl is-active --quiet mk3d-server; then
    journalctl -u mk3d-server --no-pager -n 20 >&2
    echo "The server did not stay up; its last log lines are above." >&2
    exit 1
fi
echo
echo "MK3D server running on TCP 7777 and UDP 7778; launchers older than $min_client are told to update."
echo "Certificate fingerprint:"
(cd "$app" && python3 -m server.makecert --out-dir "$data" --show)
echo "Make the player package on Windows with:"
echo "    powershell -File tools\\package.ps1 -Server ${1:-<the server address>}:7777 -Fingerprint <the fingerprint above>"
echo "Its log: journalctl -u mk3d-server -f"

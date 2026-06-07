#!/usr/bin/env bash
set -euo pipefail

mkdir -p /etc/frr /run/frr /run/lldpd /var/log/frr
touch /etc/frr/zebra.conf /etc/frr/bgpd.conf /etc/frr/staticd.conf /etc/frr/vtysh.conf

cat >/etc/frr/daemons <<'EOF'
zebra=yes
bgpd=yes
staticd=yes
ospfd=no
ospf6d=no
ripd=no
ripngd=no
isisd=yes
pimd=no
ldpd=yes
nhrpd=no
eigrpd=no
babeld=no
sharpd=no
pbrd=no
bfdd=no
fabricd=no
vrrpd=no
pathd=no

vtysh_enable=yes
zebra_options="  -A 127.0.0.1 -s 90000000"
bgpd_options="   -A 127.0.0.1"
staticd_options="-A 127.0.0.1"
isisd_options="  -A 127.0.0.1"
ldpd_options="   -A 127.0.0.1"
EOF

chown -R frr:frr /etc/frr /run/frr /var/log/frr 2>/dev/null || true
chmod 640 /etc/frr/*.conf /etc/frr/daemons 2>/dev/null || true

if [ -x /usr/lib/frr/frrinit.sh ]; then
  /usr/lib/frr/frrinit.sh start
else
  /usr/lib/frr/zebra -d -A 127.0.0.1 -s 90000000
  /usr/lib/frr/staticd -d -A 127.0.0.1
  /usr/lib/frr/bgpd -d -A 127.0.0.1
fi

while ! vtysh -c "show version" >/dev/null 2>&1; do
  sleep 1
done

if command -v lldpd >/dev/null 2>&1; then
  lldpd -d -I eth1 >/var/log/frr/lldpd.log 2>&1 &
  for _ in $(seq 1 20); do
    if lldpcli show configuration >/dev/null 2>&1; then
      lldpcli configure system hostname "$(hostname)" >/dev/null 2>&1 || true
      lldpcli configure system description "CNetNexus FRR CI peer" >/dev/null 2>&1 || true
      lldpcli configure lldp tx-interval 5 >/dev/null 2>&1 || true
      lldpcli configure lldp tx-hold 2 >/dev/null 2>&1 || true
      break
    fi
    sleep 1
  done
fi

tail -F /var/log/frr/*.log /dev/null

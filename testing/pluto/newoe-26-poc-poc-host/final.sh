: ==== cut ====
ipsec look # ../../pluto/bin/ipsec-look.sh
ipsec auto --status
: ==== tuc ====
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi

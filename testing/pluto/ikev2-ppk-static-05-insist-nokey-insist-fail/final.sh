ipsec whack --shutdown
grep -e leak /tmp/pluto.log | grep -v -e '|'
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi

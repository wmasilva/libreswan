# show we received and logged the non-zero reserved fields
hostname | grep east > /dev/null && grep "reserved: 00 00 01" /tmp/pluto.log
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi

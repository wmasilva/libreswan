# on east this should show 2 sets of in/fwd/out policies
../../pluto/bin/ipsec-look.sh
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi

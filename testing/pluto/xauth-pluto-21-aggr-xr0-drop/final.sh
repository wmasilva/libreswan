grep -E '(inserting|handling) event (EVENT_v1_SEND_XAUTH|EVENT_RETRANSMIT)' /tmp/pluto.log
: ==== cut ====
ipsec auto --status
: ==== tuc ====
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi

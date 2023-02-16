/testing/guestbin/swan-prep --x509
crlutil -I -i /testing/x509/crls/cacrlvalid.crl -d sql:/etc/ipsec.d
ipsec certutil -D -n west
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add nss-cert-crl
ipsec auto --status |grep nss-cert-crl
echo "initdone"

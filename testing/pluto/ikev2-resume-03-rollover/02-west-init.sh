../../guestbin/swan-prep --nokeys # PSK
ipsec start
../../guestbin/wait-until-pluto-started
ipsec add west-east
ipsec whack --impair suppress-retransmits

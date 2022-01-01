#!/usr/bin/env bash

export LC_ALL=C
TOPDIR=${TOPDIR:-$(git rev-parse --show-toplevel)}
BUILDDIR=${BUILDDIR:-$TOPDIR}

BINDIR=${BINDIR:-$BUILDDIR/src}
MANDIR=${MANDIR:-$TOPDIR/doc/man}

EMRALSD=${EMRALSD:-$BINDIR/emralsd}
EMRALSCLI=${EMRALSCLI:-$BINDIR/emrals-cli}
EMRALSTX=${EMRALSTX:-$BINDIR/emrals-tx}
WALLET_TOOL=${WALLET_TOOL:-$BINDIR/emrals-wallet}
EMRALSQT=${EMRALSQT:-$BINDIR/qt/emrals-qt}

[ ! -x $EMRALSD ] && echo "$EMRALSD not found or not executable." && exit 1

# The autodetected version git tag can screw up manpage output a little bit
read -r -a BTCVER <<< "$($EMRALSCLI --version | head -n1 | awk -F'[ -]' '{ print $6, $7 }')"

# Create a footer file with copyright content.
# This gets autodetected fine for emralsd if --version-string is not set,
# but has different outcomes for emrals-qt and emrals-cli.
echo "[COPYRIGHT]" > footer.h2m
$EMRALSD --version | sed -n '1!p' >> footer.h2m

for cmd in $EMRALSD $EMRALSCLI $EMRALSTX $WALLET_TOOL $EMRALSQT; do
  cmdname="${cmd##*/}"
  help2man -N --version-string=${BTCVER[0]} --include=footer.h2m -o ${MANDIR}/${cmdname}.1 ${cmd}
  sed -i "s/\\\-${BTCVER[1]}//g" ${MANDIR}/${cmdname}.1
done

rm -f footer.h2m

BitGreen Core version 1.5.0.2 is now available from:

<https://github.com/bitgreen/bitgreen/releases/tag/v1.5.0.2>

This is a new minor version release, including new features, various bug
fixes and performance improvements.

Please report bugs using the issue tracker at GitHub:

<https://github.com/bitgreen/bitgreen/issues>

# 1.5.0.2 change log

### UTXO Splitter

- #0681027 Add UTXO Splitter Tool (mrmetech)
- #7cb0a5e split utxo (konez2k)
- #544b7b1 fix utxo value (konez2k)
- #a02f8b3 clear split utxo on clear (konez2k)
- #7594710 wallet: refactor utxo split (konez2k)

### Mining

- #b551a80 fix staking chain depth (konez2k)
- #a098699 set stake modifier checkpoint (konez2k)
- #f959f13 add stake split threshold settings (konez2k)
- #a53321b split utx only on spendable ones (konez2k)
- #1d44de0 add stake combine threshold settings (konez2k)
- #f42439b remove PoS duplicate check (konez2k)

### Network

- #6e0139a net_processing: return on each processed message (konez2k)

### Masternodes

- #60240e0 generate a bls secret/public key without RPC (konez2k)

### Dependencies

- #c3efb57 upgrade libevent dependency (konez2k)

### Consensus

- #13982be update regtest spork address (konez2k)

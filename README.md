# syslog-remote_to_journal-remote
Listens on UDP port 514, accepts syslog messages, 
formats and forwards them to `systemd-journal-remote` service.

Assumes `systemd-journal-remote` is already set up on localhost port 19532.

## Installation
``` shell
make
sudo make install
sudo systemctl enable --now syslog-remote_to_journal-remote.socket
```

You can change the listening syslog port by editing the `syslog-remote_to_journal-remote.socket` file.

# TODO
- [ ] ? batch simultaneous syslog messages into one http post?
- [ ] ? Support https using the certificates defined in /etc/systemd/journal-remote.conf ?
- [ ] ? Add command line switches to configure systemd-journal-remote host and port, for instance?

---
CC0 This work has been marked as dedicated to the public domain.

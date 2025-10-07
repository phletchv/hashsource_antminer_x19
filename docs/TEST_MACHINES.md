# Test Machines

## 1

- HashSource
- 192.30.1.24
- SSH User/Pass: root/root

## 2

- HashSource
- 192.168.1.27
- SSH User/Pass: root/root

## 3

- Bitmain Stock Firmware
- 192.30.1.176
- SSH User/Pass: miner/miner

Use sshpass when connecting with password, it's installed
And these SSH/SCP flags: `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null`

When using SCP, use the legacy flag:
`sshpass -p 'root' scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null`

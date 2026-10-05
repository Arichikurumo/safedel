# safedel

Safely delete any device by writing random data before formatting the device.

`safedel` securely erases a whole disk on Linux and then writes a fresh, empty
GPT or MBR partition table, ready for `fdisk`/`parted` and `mkfs`.

## How it erases

| Device | Default (`--method auto`) |
|---|---|
| HDD | Overwrite with cryptographically strong random data (ChaCha20 keyed from `getrandom()`) |
| NVMe SSD | **NVMe Sanitize** (Block Erase → Crypto Erase → Overwrite), falling back to **Format NVM with Secure Erase** |
| SATA SSD | **ATA Security Erase** (enhanced when supported) |
| USB / SD / other flash | Overwrite, with a warning that spare blocks cannot be reached |

Overwriting flash memory is not reliable: wear-leveling and spare blocks keep
old data out of reach. That's why SSDs use the drive's built-in erase command
when it's available. Firmware erase is never sent through USB bridges, because
it can brick drives there.

## Safety

Before writing anything, `safedel`:

- refuses partitions and accepts only whole disks
- refuses if the disk or any of its partitions is **mounted**, used as **swap**,
  or held by **LVM / LUKS / RAID** (and flags system mounts like `/` and `/boot`)
- opens the device with `O_EXCL`, so the kernel itself rejects a busy device
- shows the model, serial and size, and requires you to **type the device name**
- `--dry-run` runs all checks and shows the plan without writing anything

## Build

```sh
make
sudo make install   # installs to /usr/local/sbin
```

## Usage

```
safedel [options] <device>

  -m, --method METHOD  auto (default), overwrite or firmware
  -n, --passes N       number of random overwrite passes (default 1)
  -p, --pattern LIST   explicit pass list, e.g. zero,ones,random
  -V, --verify         read back and verify the last overwrite pass
  -t, --table TYPE     partition table to write: gpt (default), mbr, none
  -d, --dry-run        run all checks and show the plan, write nothing
```

Examples:

```sh
sudo safedel --dry-run /dev/sdb
sudo safedel --verify /dev/sdb
sudo safedel --method overwrite --pattern zero,ones,random --table mbr /dev/sdc
```

A single random pass is sufficient for modern drives (NIST SP 800-88). Use
more passes only if a policy requires it.

The target can also be a regular file (a disk image), which is handy for testing:

```sh
truncate -s 64M test.img && ./safedel --verify test.img
```

## Notes

- **ATA Security Erase** temporarily sets the user password `safedel`, which
  the erase clears. If the erase is interrupted (for example by a power loss),
  the tool prints the `hdparm` command that removes the password.
- If the drive reports that security is **frozen** (most BIOSes freeze it at
  boot), suspend and resume the machine, or hot-plug the drive, and try again.
- **NVMe Sanitize** runs inside the drive and resumes after a power cycle.
  It is refused on controllers that have more than one namespace, because it
  would erase all of them.

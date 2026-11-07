# hid-logitech-headset

Linux kernel driver that reports the battery of the Logitech PRO X 2 LIGHTSPEED headset
(USB receiver `046d:0af7`) as a power supply, so UPower, GNOME and the Plasma battery applet
show its level.

This is the out-of-tree build of a patch under review on linux-input:
https://lore.kernel.org/linux-input/20261011124110.62621-1-meven@kde.org/T/#u
(v3, v4 follows on the same list). The source is the v4 patch, with the USB ids carried locally
since `hid-ids.h` is private to the kernel tree.

## Why a driver

The receiver speaks no HID++, so `hid-logitech-hidpp` has nothing to talk to. Its descriptor
holds the media keys and two vendor collections; the battery answers a fixed 64 byte frame on
report 0x51 of usage page 0xffa0. The driver asks every 120 s, and one second after the headset
is switched on, and publishes the answer with `POWER_SUPPLY_SCOPE_DEVICE`, so desktops treat it
as a peripheral and not as the machine's battery. The media keys keep working through hid-input.

The protocol comes from [HeadsetControl](https://github.com/Sapd/HeadsetControl).

## Arch Linux

The AUR package `hid-logitech-headset-dkms` installs the module through DKMS together with the
hwdb entry below. If you ran `install.sh` before, run `sudo ./install.sh --remove` first, since
both install the source to the same place.

## Install with DKMS

    sudo ./install.sh

This copies the source to `/usr/src/hid-logitech-headset-<version>` and builds it for the
running kernel. DKMS then rebuilds it on every kernel update. Replug the receiver, or
`sudo modprobe hid-logitech-headset`; udev loads it by modalias from then on.

After a kernel update, before rebooting, the running kernel may have lost its build tree. Build
for the new one instead:

    sudo ./install.sh --kernel <version>

Remove it with `sudo ./install.sh --remove`.

## Build by hand

    make
    sudo insmod hid-logitech-headset.ko

Then replug the receiver so the device moves from hid-generic to this driver.

## Check it

    cat /sys/class/power_supply/logitech-headset-*-battery/{present,status,capacity}
    upower -d | grep -A12 headset

With the headset switched off, `present` reads 0 and `capacity` returns "No data available".

`probe.py` sends the same request from userspace, over hidraw, which is how the protocol was
checked before the driver existed:

    sudo ./probe.py /dev/hidrawN      # the node whose HID_ID is 046D:0AF7

## Shown as a headset in Plasma

UPower takes the device kind from the sound card's `SOUND_FORM_FACTOR`, which systemd sets by
matching the model name. "PRO_X_2_LIGHTSPEED" carries no "headset", so the kind stays
"audio-device", which the Plasma applet does not list. Until
https://github.com/systemd/systemd/pull/43737 lands, install the entry from this repository
(the AUR package installs it for you):

    sudo install -m 644 71-hid-logitech-headset.hwdb /etc/udev/hwdb.d/
    sudo systemd-hwdb update
    sudo udevadm trigger /sys/class/sound/card*
    sudo systemctl restart upower

## Once the kernel ships it

When your distribution kernel includes `hid-logitech-headset`, run `sudo ./install.sh --remove`;
the DKMS copy in `updates/` would otherwise take precedence over the in-tree module.

## Other headsets

Only the PRO X 2 LIGHTSPEED `0af7` has been tested. The PRO X 2 variants `0afb` and `0afc`
and the older G PRO / PRO X speak HID++ and belong to `hid-logitech-hidpp`. For other models,
[HeadsetControl](https://github.com/Sapd/HeadsetControl) reads the battery from userspace.

### G522 LIGHTSPEED owners wanted

The G522 LIGHTSPEED (`046d:0b18`) answers on the same vendor page with a different frame
layout, so the driver can support it with one more table entry. That entry was written from
HeadsetControl and never run on a real G522, so it is not in the driver.

If you own one, please open an issue with:

- the output of `lsusb -d 046d:0b18`,
- the report descriptor:
  `xxd /sys/bus/hid/devices/0003:046D:0B18.*/report_descriptor`,
- the output of `headsetcontrol -b`, with the headset on, and once while charging.

That is enough to write the support and send you a build to try.

## License

GPL-2.0-or-later, see `LICENSE`.

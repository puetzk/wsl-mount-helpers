# wsl-mount-helpers

Helpers to more tightly integrate [wsl --mount], LUKS [cryptsetup], and the [wsl UNC] provider,
into an always-visible folder that windows can simply access, triggering normal windows
UAC/credential prompts as necessary to bring it online.

[wsl --mount]: https://devblogs.microsoft.com/commandline/access-linux-filesystems-in-windows-and-wsl-2/
[cryptsetup]: https://man7.org/linux/man-pages/man8/cryptsetup.8.html
[wsl UNC]: https://devblogs.microsoft.com/commandline/whats-new-for-wsl-in-windows-10-version-1903/#accessing-linux-files-from-windows

## Usage (Debian cryptdisks_start)

Debian/Ubuntu's [crypttab]/cryptdisks_start supports a `keyscript=` option (that systemd does not), giving a place to hook in luks-askpass-wincred

[crypttab]: https://manpages.debian.org/bullseye/cryptsetup/crypttab.5.en.html

### /etc/wsl.conf
```
[boot]
command = /usr/local/sbin/wsl-mount-findfs.exe --mount PARTUUID=9cae1423-26bb-4676-87bf-ec2dd707b27f --bare; cryptdisks_start data; mount /data
```

### /etc/crypttab
```
data PARTUUID=9cae1423-26bb-4676-87bf-ec2dd707b27f data keyscript=/usr/local/sbin/luks-askpass-wincred.exe
```

### /etc/fstab

```
/dev/mapper/data       /data   btrfs   defaults,noauto,subvol=@data   0       0
```

This has the advantage that it starts up automatically, but the disadvantage that it starts (requiring UAC and password)
whether you're using the /data volume or not, and it's asynchronous (/data is empty at first)

## Usage (systemd)

### /etc/wsl.conf
```
[boot]
systemd=true
[interop]
enabled=true
```

### /etc/binfmt.d/WSLInterop.conf
When using systemd mode, wsl no longer (since some July/Aug 2023 update?) enables .exe interop,
which is required in order to run the win32 code of wsl-mount-findfs.exe and luks-askpass-wincred.exe
https://www.freedesktop.org/software/systemd/man/binfmt.d.html#
https://serverfault.com/questions/1141533/wsl2-stopped-running-windows-tools-seemingly-all-of-a-sudden-cannot-execute-bi
https://github.com/microsoft/WSL/issues/8843
https://github.com/systemd/systemd/issues/28126
```
:WSLInterop:M::MZ::/init:PF
```

### /etc/systemd/system/wsl-mount@
```
[Unit]
Description=wsl.exe --mount <Device> --bare

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/wsl-mount-findfs.exe --mount "%I" --bare
ExecCondition=sh -c '! findfs "%I"'
```

### /etc/systemd/systemd-cryptsetup@data.service

use `systemd-escape ...` to escape your PARTUUID=... string

```
[Unit]
Description=Cryptography Setup for %I
DefaultDependencies=no
IgnoreOnIsolate=true
After=cryptsetup-pre.target systemd-udevd-kernel.socket
Before=blockdev@dev-mapper-%i.target
Wants=blockdev@dev-mapper-%i.target
Conflicts=umount.target
Before=cryptsetup.target
BindsTo=wsl-mount@PARTUUID\x3d9cae1423\x2d26bb\x2d4676\x2d87bf\x2dec2dd707b27f
After=wsl-mount@PARTUUID\x3d9cae1423\x2d26bb\x2d4676\x2d87bf\x2dec2dd707b27f
BindsTo=dev-disk-by\x2dpartuuid-x3d9cae1423\x2d26bb\x2d4676\x2d87bf\x2dec2dd707b27f
After=dev-disk-by\x2dpartuuid-x3d9cae1423\x2d26bb\x2d4676\x2d87bf\x2dec2dd707b27f
Before=umount.target

[Service]
Type=oneshot
RemainAfterExit=yes
KeyringMode=shared
ExecStart=sh -c '/usr/local/sbin/luks-askpass-wincred.exe @data | /usr/sbin/cryptsetup open --type luks /dev/disk/by-partuuid/6b123123-236b-1b43-97ea-776941c0d2ee %I'
ExecStop=/usr/sbin/cryptsetup close %I
```

### /etc/systemd/system/data.mount
```
[Unit]
Description=Mount WSL Shared Folder
BindsTo=systemd-cryptsetup@data.service
After=systemd-cryptsetup@data.service

[Mount]
What=/dev/mapper/luks-data
Where=/data
Type=btrfs
Options=subvol=@data
```

### /etc/systemd/system/data.automount
```
[Unit]
Description=Automount WSL Shared Folders
[Install]
WantedBy=local-fs.target
[Automount]
Where=/data
```

Then issue the command

`systemctl enable data.automount`

This is more complex, but it starts only if /data is used, and (mostly) includes service stop as well.

# Issues

Accessing \\wsl.localhost\Ubuntu\data somehow fails to trigger the autofs when there's no WSL processes...
I assume somehow whatever minimal startup WSL's /init does not depend on systemd's [local-fs.target]

Peeking at the state from outside (via \\wsl.localhost\Ubuntu\proc), it looks like the special WSL /init
does not exec /sbin/init (i.e. systemd) at all for filesystem access,
only when launching "real" processes

[local-fs.target]: https://www.freedesktop.org/software/systemd/man/systemd.special.html#local-fs.target

There's also some interesting issues with the p9rdr.sys and/or \\wsl.localhost (or, [prior to build 21354](https://learn.microsoft.com/en-us/windows/wsl/release-notes#build-21354) `\\wsl$`) UNC provider: 

- `subst d: \\wsl.localhost\Ubuntu\data` works
  but shows as "Disconnected Network Drive" even while working (maybe the redirector fails to report some status?)
- `net use d: \\wsl.localhost\Ubuntu\data`
  fails with "System error 67 has occurred. The network name cannot be found."
  (this makes sense now that I realize it's a UNC redirector), as [microsoft says](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/support-for-unc-naming-and-mup)
  > MUP is not involved during an operation that creates a mapped drive letter (the "NET USE" command, for example). This operation is handled by the multiple provider router (MPR)
- "Add a network location" does work as desired to list this as a "Network locations"
  in explorer, under "This PC", which seems appropriate (though it doesn't result in a legacy DOS drive letter)

 - <details>
   <summary>extra reading/link-dump</summary>

   - https://github.com/microsoft/WSL/issues/7883#issuecomment-1055861739
   - https://github.com/microsoft/WSL/issues/9163
   - https://www.tiraniddo.dev/2019/07/digging-into-wsl-p9-file-system.html
   - https://nelsonslog.wordpress.com/2019/06/01/wsl-access-to-linux-files-via-plan-9/
   - https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/the-kernel-network-mini-redirector-driver
   - https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/support-for-unc-naming-and-mup
   </details>

# TODO

## Extend to a generic wincred systemd-askpass provider

See about extending luks-askpass-wincred into https://systemd.io/PASSWORD_AGENTS/ system;
Then the existing [systemd-cryptsetup-generator] should "just work" with less customization

[systemd-cryptsetup-generator]: https://www.freedesktop.org/software/systemd/man/systemd-cryptsetup-generator.html#]

## running standalone
It would be cool to package a tiny executable (statically-linked to [libblkid],
[libcryptsetup], and the [mount] syscall) as a tiny WSL "distribution"
which *only* provided such filesystem access, with minimal overhead

[libblkid]: https://github.com/util-linux/util-linux/blob/master/libblkid/src/blkid.h.in
[libcryptsetup]: https://gitlab.com/cryptsetup/cryptsetup/-/blob/main/lib/libcryptsetup.h
[mount]: https://man7.org/linux/man-pages/man2/mount.2.html

- https://learn.microsoft.com/en-us/windows/wsl/use-custom-distro
- https://github.com/yuk7/AlpineWSL/

## minimal shim WSL "distribution"

It should also be possible to extend the above to a shim that mounts an existing
linux rootfs over the above's (nearly-empty) `/` and then exec's /sbin/init,
launching that "native" linux userspace within WSL's kernel.

However, this would need to propagate some of the setup done by WSL's `/init`
to preserve the rest of the windows integration (binfmt_misc, /mnt/c, etc).
Some discussion of the challenges at:

- https://askubuntu.com/questions/1376250/migrate-a-physical-ubuntu-20-04-desktop-to-wslg-in-windows-11-by-mounting-the-sy
- https://unix.stackexchange.com/questions/683650/run-existing-os-from-dualboot-as-wsl

Could also probably take inspiration from some of the namespace/`unshare` hacks
that were done to integrate systemd before Microsoft offically supported it:

- https://github.com/shayne/wsl2-hacks
- https://github.com/arkane-systems/genie

# See also

- [WinBtrfs](https://github.com/maharmstone/btrfs), an native NT driver for btrfs.
  Note that the author claims this is a full re-implementation from scratch.
  btrfs is still evolving, so beware any compatibility.
- https://github.com/vricosti/libwindevblk
  This offers a posix-ish "block device" abstraction of the win32 API,
  though in the end wsl-mount-findfs ended up operating only through
  Win32DeviceIoControl without any raw reads.
- https://unix.stackexchange.com/questions/702807/finding-the-tools-filesystem-mounted-on-init-in-wsl2
  https://learn.microsoft.com/en-us/archive/blogs/wsl/windows-and-ubuntu-interoperability

  Some info on the magical WSL /init daemon, though not a lot...

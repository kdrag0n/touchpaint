# Touchpaint

Touchpaint is a Linux kernel module designed to achieve the lowest possible end-to-end input latency on modern smartphones. It aims to reverse the trend of newer computers exhibiting [significantly worse latency than older ones](https://danluu.com/input-lag/) and demonstrate that modern devices are *capable* of delivering low latency, though perhaps not in general-purpose operating systems or graphics rendering pipelines.

Tap latency with Touchpaint has been measured to be as low as 10 ms — almost as low as possible on a 60 Hz display — on the Asus ROG Phone II (240 Hz touch scan rate) and 20 ms on the Asus ZenFone 6 (120 Hz touch scan rate). The display was refreshing at 60 Hz on both devices.

This low latency is made possible by a custom minimal graphics stack that writes directly to the display's framebuffer as soon as a touchscreen interrupt is received. The display controller scans out the framebuffer directly on every vblank event and sends the pixels to the display immediately. Scheduling delays are minimized and no buffering or context switches are involved. The GPU is not used as it will increase latency for such simple rendering tasks like this.

For equivalent touch latency testing on Android and iOS, check out the [Android](https://github.com/kdrag0n/touchpaint-android) and [Flutter](https://github.com/kdrag0n/touchpaint-flutter) apps.

## Requirements

This module makes several assumptions:

- The bootloader leaves a framebuffer (that the display controller scans out independently) set up for continuous splash handoff before booting Linux
- The framebuffer's color format is ARGB_8888 (i.e. 8 bpc / 32 bpp)

In general, the aforementioned assumptions are true for almost all modern Android devices that have Qualcomm Snapdragon SoCs. Your mileage may vary on other platforms. Bringing up your own framebuffer is difficult, but adapting to a different color format should be trivial as long as the pixel size is the same (32 bpp).

The only changes that are strictly necessary are the core Touchpaint module commits as well as the commit to prevent the display driver (mdss_fb or msm_drm, depending on kernel version) from [taking over the framebuffer](https://github.com/kdrag0n/touchpaint/commit/eeee8bf9a705) at boot. However, if you have a Qualcomm SoC, is recommended for convenient debugging.

## Configuration

Touchpaint will need to be configured for every new device. All configuration variables are located at the top of the file. The framebuffer address, size, width, and height will need to be updated for the module to work properly.

The default config's framebuffer address and size should work for almost all Snapdragon 855 devices, but they will need to be changed on other platforms. The corresponding device tree node is usually named `cont_splash_region` on Snapdragon SoCs.

## Userspace

Strictly speaking, there are no specific requirements for userspace, since Touchpaint resides in the kernel. However, it is recommended to use the provided [minimal Alpine Linux ramdisk](https://github.com/kdrag0n/touchpaint/blob/master/ramdisk/alpine-rd.cpio.gz) as a minimal userspace to prevent Android from hogging CPU (as a result of not being able to take over the display) and causing jitter or lag.

If you decide to use the Alpine Linux ramdisk and you have a Qualcomm SoC, you should prevent the bootloader from [disabling cpuidle in lpm-levels](https://github.com/kdrag0n/touchpaint/commit/1eedf30258fd), otherwise the device will heat up very quickly and never cool down once the ramdisk boots.

You must also enable `CONFIG_DEVTMPFS` in the kernel for Alpine to work correctly, and optionally `CONFIG_USB_CONFIGFS_RNDIS` if you want USB debugging to work.

The provided Alpine ramdisk's init script will start an SSH server over virtual USB Ethernet for easy debugging. Connection information is exposed in the device serial number:

```
usb 7-2: new high-speed USB device number 120 using xhci_hcd
usb 7-2: New USB device found, idVendor=0b05, idProduct=4daf, bcdDevice= 4.14
usb 7-2: New USB device strings: Mfr=1, Product=2, SerialNumber=3
usb 7-2: Product: Alpine GNU/Linux
usb 7-2: Manufacturer: Linux
usb 7-2: SerialNumber: SSH on 10.15.19.82
```

## Tuning

Make sure you're using the provided Alpine Linux ramdisk to minimize userspace CPU usage.

### Touchscreen IRQ afinity

The touchscreen IRQ should be pinned to the fastest CPU available in the system, e.g. CPU7 (Prime) on the Snapdragon 855 and any of CPU4-7 on most big.LITTLE SoCs. This can be done by extracting the ramdisk and repacking it with the touchscreen IRQ number changed in `/init`.

### I2C bus clock

[Overclocking the touchscreen's I2C bus](https://github.com/kdrag0n/touchpaint/commit/e016b1e03bd1) can help reduce latency slightly. On the Asus ZenFone 6, the time taken to read events from the touchscreen dropped from 3-4 ms to 1-2 ms after overclocking its I2C bus from 400 KHz to 1 MHz, which is quite significant at this scale.

### 128-bit integers

Kernel support for 128-bit integers (which are supported by all ARMv8 CPUs) will improve rendering performance significantly, especially for larger blocks, as it allows up to 4 pixels to be drawn with a single instruction. The following commits will need to backported from mainline in older downstream kernels:

- [arm64: support __int128 on gcc 5+](https://github.com/torvalds/linux/commit/fb8722735f50)
- [arm64: support __int128 with clang](https://github.com/torvalds/linux/commit/ad40bdafb495)

## Modes

This module has several different modes:

- Paint (0, default) — simple paint tool
- Fill (1) — useful for testing tap latency with a slow-motion camera
- Bounce (2) — similar to the AOSP [TouchLatency](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-10.0.0_r40/tests/TouchLatency/) app's ball mode
- Follow (3) — similar to [Microsoft Research](https://www.youtube.com/watch?v=vOvQCPLkPt4)'s touch latency demo video

You can switch modes by cycling through them with the volume-up key (recommended), or alternatively by writing the desired mode to `/sys/module/touchpaint/parameters/mode`.

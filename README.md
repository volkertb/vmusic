# About

**VMusic** is an extension pack for [VirtualBox](https://www.virtualbox.org), containing
some virtual devices for common music hardware:

* A `mpu401` device emulating a MPU-401 "compatible" dumb/UART-only, on the usual ports 0x330-0x331.
This allows the guest to output MIDI data to the host. The raw MIDI data is sent to a "Virtual RawMIDI" ALSA device
which can be connected with either a real MIDI device or a synthesizer such as [FluidSynth](https://www.fluidsynth.org/)
or [Munt](https://sourceforge.net/projects/munt/). MIDI input is also partially supported.

* An `adlib` device emulating an OPL2/OPL3 FM synthesizer using the [Nuked OPL3 emulator](https://github.com/nukeykt/Nuked-OPL3).
By default this device is configured on the standard AdLib ports, 0x388-0x38B, but can also be configured
to listen simultaneously on a second set of ports in order to provide Sound Blaster FM compatibility
(e.g. 0x220-0x223).
The generated audio is sent directly via ALSA to the default PCM output device (usually PulseAudio), ignoring 
VirtualBox settings.

* A `emu8000` device emulating the EMU8000 chip, the wavetable synthesis device that was included in the Sound Blaster AWE32,
using code from the [PCem emulator](https://www.pcem-emulator.co.uk/).
This allows most software to assume an SB AWE32 is installed (rather than VirtualBox's standard SB16).
The default base port of 0x620 matches the default base port of VirtualBox's SB (0x220).
Using this device requires the AWE32.RAW file that can be dumped using the AWE-DUMP tool, as with PCem.
Like the AdLib device, the generated audio is sent directly via ALSA.

Note that **this extension pack only works with Linux hosts**, but should work with any type of guests. 
To make an extension pack work in Windows, it would need to be
[signed like a kernel mode driver](https://forums.virtualbox.org/viewtopic.php?f=10&t=103801),
which is practically impossible for an individual.

These devices can be combined with the standard VirtualBox SB16 emulation, to experience a more complete SB16
emulation (or even SB AWE32), albeit it is not necessary.
You can enable each device independently, e.g. to have pure MPU-401 only.
Note that "SB MIDI" support is not implemented; for MIDI out you can only use the MPU-401 device. Most Sound Blaster
drivers post-SB16 already use the MPU-401 device.

[TOC]

### Screenshots

<img src="https://depot.javispedro.com/vbox/VBoxPrefs.png" alt="VirtualBox Preferences dialog showing Extensions panel with VMusic installed" style="max-width: 40%; vertical-align:top;" />
<img src="https://depot.javispedro.com/vbox/win98e.png" alt="Screenshot of Windows 98 playing CANYON.MID while showing all the 3 devices available for MIDI output" style="max-width: 40%; vertical-align:top;" />

<img src="https://depot.javispedro.com/vbox/VirtualBoxMunt.png" alt="Screenshot of VirtualBox playing The Secret of Monkey Island while connected to the Munt MT-32 Emulator" style="max-width: 70%;" />


# Installing

1.  Download
    [VMusic-0.3.2-vbox7.0.4.vbox-extpack](https://depot.javispedro.com/vbox/VMusic-0.3.2-vbox7.0.4.vbox-extpack).
    This is built for VirtualBox 7.0.4, albeit it should work for most other recent versions in the 7.0.x series.
    For VirtualBox 6.1.x series, please use
    [VMusic-0.3.1-vbox6.1.32.vbox-extpack](https://depot.javispedro.com/vbox/VMusic-0.3.1-vbox6.1.32.vbox-extpack).

2.  Open VirtualBox, go to File → Tools → [Extension pack manager](https://depot.javispedro.com/vbox/VBoxPrefs.png),
    and install the downloaded VMusic-_something_.vbox-extpack file.

Alternatively, run `VBoxManage extpack install VMusic.vbox-extpack` on a terminal.

# Using

Each device must be enabled on each VM individually, and there is no GUI to do it right now.

Run the following, replacing `$vm` with the name of your Virtual Machine:

```shell
# To enable the MPU-401 device
VBoxManage setextradata "$vm" VBoxInternal/Devices/mpu401/0/Trusted 1
# To enable the Adlib device
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Trusted 1
# To enable the EMU8000 device
VBoxManage setextradata "$vm" VBoxInternal/Devices/emu8000/0/Config/RomFile "$HOME/.pcem/roms/awe32.raw"

# Optional: to enable the Adlib device on the default SB16 ports too
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Config/MirrorPort "0x220"
# Optional: to enable an IRQ for MPU-401 MIDI input
VBoxManage setextradata "$vm" VBoxInternal/Devices/mpu401/0/Config/IRQ 9
```

If the devices have been correctly enabled, you should see the following messages in the
VBox.log file of a virtual machine after it has been powered on:

```{ use_pygments=false }
00:00:00.799849 Installed Extension Packs:
00:00:00.799866   VMusic (Version: 0.3 r0; VRDE Module: )
...
00:00:00.920058 adlib0: Configured on port 0x388-0x38b
00:00:00.920066 adlib0: Mirrored on port 0x220-0x223
00:00:00.920825 mpu401#0: Configured on port 0x330-0x331
00:00:00.924719 emu8000#0: Configured on ports 0x620-0x623, 0xA20-0xA23, 0xE20-0xE23
```

### Connecting Adlib or EMU8000

You do not need to do anything else to hear the emulated audio.
It will be automatically sent to the default ALSA PCM out device,
ignoring your preferred output device set in the VirtualBox GUI.
There is currently no way to change that.

### Connecting MPU-401

Even after you power on a virtual machine using the MPU-401 device, you still need to connect 
the output from the virtual machine with either a real MIDI out device or a software synthesizer,
using the standard ALSA utility `aconnect`. 

The following assumes you are going to be using a software synthesizer like [FluidSynth](https://www.fluidsynth.org/)
or [Munt](https://sourceforge.net/projects/munt/).

First, start the virtual machine. Second, start and configure your software synthesizer.

If you run `aconnect -l` at this point, you will see a list of all your real/virtual MIDI devices in the system. Sample output from  `aconnect -l`:
```{ use_pygments=false }
client 0: 'System' [type=kernel]
    0 'Timer           '
	Connecting To: 142:0
    1 'Announce        '
	Connecting To: 142:0, 129:0
...
client 128: 'Client-128' [type=user,pid=4450]
    0 'Virtual RawMIDI '
client 129: 'Munt MT-32' [type=user,pid=8451]
    0 'Standard        '
```

This indicates that there is a `Munt MT-32` synthesizer at port 129:0 , and a `Virtual RawMIDI` at port 128:0.
The latter is the virtual MIDI device used by the MPU-401 emulation. So, to send the virtual machine's MIDI output to Munt,
connect the two ports by running:

```{ use_pygments=false }
aconnect 128:0 129:0
```

Note that the port numbers may be completely different in your system.

Also, [Qsynth](https://qsynth.sourceforge.io/) (a GUI frontend for FluidSynth) has an option to automatically connect
all virtual MIDI ports to it, in which case you may not need to connect anything.

For MIDI input, you should do the connection in the opposite direction: connect from your real MIDI hardware to the
`Virtual RawMIDI` device.

# Building

You need the standard C++ building tools, make, libasound and headers (e.g. `libasound2-dev` in Ubuntu).

First, ensure that, in the directory where the VMusic source resides, you add two extra directories:

* `VirtualBox.src` containing the unpacked pristine VirtualBox source from
[virtualbox.org](https://www.virtualbox.org/wiki/Downloads), e.g. the contents of
[VirtualBox-7.0.4.tar.bz2](https://download.virtualbox.org/virtualbox/7.0.4/VirtualBox-7.0.4.tar.bz2).

* `VirtualBox.linux.amd64` containing the VirtualBox runtime library: `VBoxRT.so`.
Either copy this file from the official VirtualBox Linux build, or from your distribution.
E.g. copy `/usr/lib/virtualbox/VBoxRT.so` into `VirtualBox.linux.amd64/VBoxRT.so`.

After this, just type `make` followed by `make pack` and `VMusic.vbox-extpack` should be generated.

# Changelog

* v0.3.2 minor changes to fix compatibility with VirtualBox 7.0.0

* v0.3.1 EMU8000 RAM is now saved in snapshots,
 OPL emulator uses 49716Hz sample rate by default now,
 renamed some config settings.

* v0.3 added support in MPU-401 for UART-mode MIDI input, and the EMU8000 device.

* v0.2 is the initial release

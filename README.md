# About

**VMusic** is an extension pack for [VirtualBox](https://www.virtualbox.org), containing
some virtual devices for common music hardware:

* An `adlib` device emulating an OPL2/OPL3 using the [Nuked OPL3 emulator](https://github.com/nukeykt/Nuked-OPL3).
By default this device is configured on the standard Adlib Gold ports, 0x388-0x38B, but can also be configured
to listen simultaneously on a second set of ports in order to provide some Sound Blaster Pro/SB FM compatibility
(e.g. 0x220-0x223).
The generated audio is sent directly via ALSA to the default PCM output device (usually PulseAudio), ignoring 
VirtualBox settings.

* A `mpu401` device emulating a MPU-401 "compatible" dumb/UART-only, on the usual ports 0x330-0x331.
This allows the guest to output MIDI data to the host. The raw MIDI data is sent to a "Virtual RawMIDI" ALSA device
which can be connected with either a real MIDI device or a synthesizer such as [FluidSynth](https://www.fluidsynth.org/)
or [Munt](https://sourceforge.net/projects/munt/).

Note that **this extension pack only works with Linux hosts**, but should work with any type of guests. 
To make an extension pack work in Windows, it would need to be
[signed like a kernel mode driver](https://forums.virtualbox.org/viewtopic.php?f=10&t=103801),
which is practically impossible for an individual.

These devices can be used with the standard VirtualBox SB16 emulation, so as to experience a more complete SB16
emulation, albeit is also not necessary. You can enable each device independently, e.g. to have pure Adlib card only.
Note that "SB MIDI" support is not implemented; for MIDI out you can only use the Mpu401 device. Most Sound Blaster
drivers post-SB16 use the Mpu401 device.

![Screenshot of VirtualBox playing The Secret of Monkey Island while connected to the Munt MT-32 Emulator](http://depot.javispedro.com/vbox/VirtualBoxMunt.png)

# Installing

You can try using the [VMusic.vbox-extpack](http://depot.javispedro.com/vbox/VMusic-0.2-vbox6.1.30.vbox-extpack)
I built for VirtualBox 6.1.30,
which you can install into VirtualBox through the VirtualBox Preferences -> Extension Packs GUI,
or by running `VBoxManage extpack install VMusic.vbox-extpack`.
This should work at least for most other recent versions in the 6.1.x series.

# Using

Each device must be enabled on each VM individually, and there is no GUI to do it right now.

Run the following, replacing `$vm` with the name of your Virtual Machine:

```shell
# To enable the Adlib device
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Trusted 1
# To enable the Adlib device on the default SB16 ports too
VBoxManage setextradata "$vm" VBoxInternal/Devices/adlib/0/Config/MirrorPort "0x220"
# To enable the MPU-401 device
VBoxManage setextradata "$vm" VBoxInternal/Devices/mpu401/0/Trusted 1
```

If the devices have been correctly enabled, you should see the following messages in the
VBox.log file of a virtual machine after it has been powered on:

```{ use_pygments=false }
00:00:00.799849 Installed Extension Packs:
00:00:00.799866   VMusic (Version: 0.2 r0; VRDE Module: )
...
00:00:00.920058 adlib0: Configured on port 0x388-0x38b
00:00:00.920066 adlib0: Mirrored on port 0x220-0x223
00:00:00.920825 mpu401#0: Configured on port 0x330-0x331
```

### Connecting Adlib

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

# Building

You need the standard C++ building tools, make, libasound and headers (e.g. `libasound2-dev` in Ubuntu).

First, ensure that, in the directory where the VMusic source resides, you add two extra directories:

* `VirtualBox.src` containing the unpacked pristine VirtualBox source from [virtualbox.org](https://www.virtualbox.org/wiki/Downloads), e.g. the contents of [VirtualBox-6.1.32.tar.bz2](https://download.virtualbox.org/virtualbox/6.1.32/VirtualBox-6.1.32.tar.bz2).

* `VirtualBox.linux.amd64` containing at least the following VirtualBox Linux.amd64 libraries,
either from an official installation or your distribution's package: `VBoxRT.so` and `VBoxVMM.so`.
E.g. copy `/usr/lib/virtualbox/VBoxRT.so` into `VirtualBox.linux.amd64/VBoxRT.so`.

After this, just type `make` followed by `make pack` and `VMusic.vbox-extpack` should be generated.



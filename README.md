# Plain QUIC Plugin

It's a slight modification version of the QUIC dissector in the Wireshark repository. It assumes that all packets are transmitted in plain text, thus skipping all decryption operations.

It sounds ridiculous at first glance; why do we ever send plain QUIC packets to the wire? It's written for our course that teaches students QUIC by guiding them in implementing it from scratch. Since complete QUIC implementation is almost impossible for a one-semester class, students can implement only features they are interested in, such as connection migration or flow control. QUIC handshake and packet protection are optional features, as you might expect. This plugin might help them read packet dumps.


# Installation

## install latest wireshark

Current code were written for Wireshark 4.2.4, yet it is compatiable with the latest(as of March 2025)  stable version(4.4.5) of Wireshark.

```bash
sudo add-apt-repository ppa:wireshark-dev/stable
sudo apt update
sudo apt install wireshark
```

Then you should expect the latest stable version of wireshark(v 4.4.5) installed.

Optional:
Enable current user to capture (which means that `sudo` is not required):
```bash
sudo dpkg-reconfigure wireshark-common
sudo usermod -aG wireshark $USER
newgrp wireshark
```


## build this plugin

```bash
sudo apt install wireshark-dev
mkdir -p build
cd build
cmake ..
make
```

Then you should be able to found `build/libplain_quic.so` .


# install the plugin

The `.so` file you got is need to be installed manually into the  [Plugin folders](https://www.wireshark.org/docs/wsug_html_chunked/ChPluginFolders.html) .

For Ubuntu and Wireshark 4.4.x, 

```bash
mkdir -p ~/.local/lib/wireshark/plugins/4.4/epan
cp build/libplain_quic.so ~/.local/lib/wireshark/plugins/4.4/epan/
```

Open wireshark, and Click `Help -> About wireshark -> Folders` to check the installation folder, and `Help -> About wireshark -> Plugins` to check the installation. 


Then, you are expected to be able to inspect the frames of packets in `doc.pcap` with wireshark.


For the usage of this plugin, refer to the [documentation](doc/README.md).

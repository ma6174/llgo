# idfparts 提取清单（P1b 钉死版本）
- 来源镜像: espressif/idf:latest (docker image 7c39a75ed932)
- IDF commit: ad20ded62a2 (master, v6.3-dev) —— 与 P0 调研 SYMBOLS.md 用的 ~/esp/esp-idf commit ad20ded6 同一版本，osi 函数表 v0x9 (ESP_WIFI_OS_ADAPTER_VERSION=0x9, MAGIC=0xDEADBEAF) 与 blob 一一配对
- 提取日期: 2026-11-27
- 提取方式: docker run --rm --entrypoint bash espressif/idf:latest -c 'cd /opt/esp/idf && tar cf - ...'

## lib/ (sha256)
7492f324451a4d359998fbfc12670bd22f19026def0d73c72a69025c9c2baac0  lib/libcore.a
0ddd497f5b4625475971bf63897308104be250b48f19e4b576519b29b1c705ba  lib/libespnow.a
9bc746c863216cf1aa297d84eb45c93c99e4e2a57319f0849d20bbfd6ce64409  lib/libmesh.a
f8667a5e174c770538e90e30e6026fb58ef459cecc6d5fc75a3001bd8020fc4c  lib/libnet80211.a
8806b9ce293d79e99be94e1c1b1ed72a9e725fb7692f27bf29c27421f2f6b191  lib/libcoexist.a
116c7c3a55e2ddfd7937edad2667b5d64717f1967519a0201ca6354246f0b1a2  lib/libphy.a
9fc7562ec712a4512699433ce21c8bcfd6d4ec6137243aac5eec2d72af72d7c6  lib/libpp.a
755b21571faf0170bf30c9a1889aaad864f716e3cb3d2c50b7fed68b3e5c45fa  lib/libsmartconfig.a
d5e2691f22932dc03abcf8848fc96867190a454b3c89d1405b2b026d3adf73e8  lib/libwapi.a

## rom/ (13 个 ld, sha256)
267c12cec4d6092dded363722e2545924b432bf9add9089b5670e98b8ec44833  rom/esp32s31.rom.api.ld
e9e42bd47704c2cefc1f137325ea12cfb6c73c2e0e847c36d344ce8afd7a6020  rom/esp32s31.rom.coexist.ld
d1927ed76c039fe74df4eea192b534aa8eaa1f5c74e00aa26fa91eb903e872ff  rom/esp32s31.rom.ld
3b0395460ffc66cfa8ac03679e1bd6c6bdc073729989995eb97f6637473f2b4d  rom/esp32s31.rom.libc.ld
c157547b2231b13fb51e79dcac31f4c545da539018ffc8d50e79154b656542d5  rom/esp32s31.rom.libgcc.ld
976949c1ed488926cd0e5d637504048f9b60891926315a0d755f4befb93eeac5  rom/esp32s31.rom.net80211.ld
ffc4c02978d3bba99749811044d45b540b4aa737d58b3f9dfd8f94860a1247b4  rom/esp32s31.rom.newlib-nano.ld
2f42d6a558599fa91dc3173d1f888c8d6b70439ff1fba214a0fd4a1e9a52faeb  rom/esp32s31.rom.newlib.ld
e135d66fb4e676adb6408fbaf9794ba4981b4cc09a5d2cb20cc3e490db86c6c6  rom/esp32s31.rom.phy.ld
f37d8f3c026611f82c620811d6292c7d9c47fcb5819c1cf4953f4f63d0c9cd30  rom/esp32s31.rom.pp.ld
e9f842c7525463b6fc3ff681e14b020ee1660ffe6fabbbdd2f3c29914388cc67  rom/esp32s31.rom.rvfp.ld
8ec54e6f9427e38cd544bbc8ab8d5e786981f5d7b6f25d3382748e4c8fba02dd  rom/esp32s31.rom.spiflash.ld
9d72db1b94a4b080b34e759c8e0f69b0bca669b987a97434553986336c59a222  rom/esp32s31.rom.version.ld

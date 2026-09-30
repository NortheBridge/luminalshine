# Vendored PyroWave provenance

Source: https://github.com/Koloses/Solarflare/tree/48ae555e73428926ae4be234f15513967a839397/third-party/pyrowave
Client: https://github.com/Koloses/aurora-qt/tree/2c574a9e79da5b3fa95bab90ffd3d322f5d63ac0

Codec and allocation sources retain their original license notices.
LuminalShine changes: Windows scRGB-to-PQ conversion in rgb2yuv.comp;
Windows build compatibility where documented in the diff.
The host adapter under src/pyrowave derives from the same Solarflare revision.

Decoder sources (pyrowave_decoder.cpp and pyrowave_decoder.h) are pinned to the
Aurora client revision above, including its noncoherent decoder-input flush support.
Encoder sources remain pinned to the Solarflare revision above.

# Цена dp_depth в эмулированном TLS

Машина: 4 CPU, Intel(R) Xeon(R) Processor @ 2.10GHz; Linux-6.18.44-fc-v185-x86_64-with-glibc2.39. Коммит 0e9bec0; `dpl build --release` (C с -O2), `DPL_THREADS=1`; медиана 9 кругов, прогоны чередуются.

- android — Android (16134705, +pgo, +bolt, +lto, +mlgo, based on r574158c) clang version 21.0.0 (https://android.googlesource.com/toolchain/llvm-project 9f872551d3c681d06fd303b36f16ed5c274735eb), запуск под qemu-aarch64 version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18), `-cpu cortex-a72`; время эмуляции, не устройства: годится для отношений вариантов, не для абсолютных чисел.
- host — Ubuntu clang version 18.1.3 (1ubuntu1), x86-64; раскладки кода `-falign-loops` = 1, 16, 32, 64, у варианта — лучшая.

Варианты: emutls — эмулированный TLS (у android сейчас), elftls — ELF TLS (у android — API 29), notls — без TLS (нижняя граница, с потоками нельзя). Цена — к elftls и к notls.

## android

| нагрузка | emutls, мс | elftls, мс | notls, мс | emutls к elftls | elftls к notls |
|---|---|---|---|---|---|
| вызовы: fib(32) | 408 | 153 | 131 | +166% | +17% |
| рельеф по шуму | 3103 | 2898 | 2889 | +7% | +0% |
| симуляция тел | 593 | 587 | 542 | +1% | +8% |
| М2: Pool, 100 тыс. × 100 кадров | 1560 | 1471 | 1553 | +6% | -5% |

## host

| нагрузка | emutls, мс | elftls, мс | notls, мс | emutls к elftls | elftls к notls |
|---|---|---|---|---|---|
| вызовы: fib(32) | 44 | 22 | 21 | +104% | +1% |
| рельеф по шуму | 201 | 205 | 196 | -2% | +5% |
| симуляция тел | 34 | 35 | 33 | -3% | +6% |
| М2: Pool, 100 тыс. × 100 кадров | 339 | 353 | 361 | -4% | -2% |

Вывод вариантов совпал: да.

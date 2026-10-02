# BLE Heart Rate (OBS Plugin)

[![GitHub](https://img.shields.io/badge/GitHub-Discrutans%2Fobs--ble--hr-blue?logo=github)](https://github.com/Discrutans/obs-ble-hr)
[![Release](https://img.shields.io/github/v/release/Discrutans/obs-ble-hr)](https://github.com/Discrutans/obs-ble-hr/releases)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

Нативный плагин OBS Studio для прямого подключения умных часов / фитнес-браслетов по **BLE** (GATT Heart Rate `0x180D`) и отображения пульса (BPM) на стриме.

**Репозиторий:** https://github.com/Discrutans/obs-ble-hr  
**Лицензия:** [GPL-3.0](LICENSE)

## Download (без сборки)

Готовые сборки Windows x64: **[Releases](https://github.com/Discrutans/obs-ble-hr/releases)**

1. Скачайте `ble-heart-rate-windows-x64-v*-setup.exe` (рекомендуется) или `.zip`.
2. **Setup.exe:** запустите установщик и следуйте мастеру.
3. **ZIP (вручную):** распакуйте папку `ble-heart-rate` в  
   `C:\ProgramData\obs-studio\plugins\`
4. Перезапустите OBS → **Источники → + → Датчик пульса (BLE)**.

## Возможности

- Источник **«Датчик пульса (BLE)»** / **Heart Rate (BLE)**
- Сканирование BLE, выбор устройства (имя + RSSI)
- Подписка на Heart Rate Measurement (`0x2A37`)
- Автоподключение к последнему устройству при запуске OBS
- Оверлей: BPM + анимированное сердце (частота = `60000 / BPM` мс)
- 3 зоны пульса со своими цветами (покой / нагрузка / пик)
- Кастомизация сердца и позиция текста (рядом / внутри)
- При отсутствии данных > 5 с — `--`, сердце без пульсации
- Авто-реконнект в фоне
- Локализация `ru-RU` / `en-US`
- BLE-операции в отдельном потоке (без блокировки рендера OBS)
- Ссылки: поддержка автора и GitHub

## Требования (использование)

- Windows 10/11 x64
- OBS Studio
- Bluetooth-адаптер с поддержкой BLE

## Сборка из исходников

Нужны Visual Studio 2022 (Desktop C++), CMake 3.28+, интернет при первой конфигурации (OBS SDK в `.deps`).

```bat
git clone https://github.com/Discrutans/obs-ble-hr.git
cd obs-ble-hr
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

DLL: `build_x64\rundir\RelWithDebInfo\ble-heart-rate.dll`  
Локали: скопируйте `data\` рядом с плагином в  
`C:\ProgramData\obs-studio\plugins\ble-heart-rate\`.

Установщик Windows собирается через Inno Setup (`installer\ble-heart-rate.iss`) после подготовки папки `dist\ble-heart-rate\`.

## macOS

Сборка через CMake presets для macOS. Используется CoreBluetooth (`ble_manager_mac.mm`). Нужны разрешения Bluetooth в Info.plist хоста OBS.

## Структура

| Путь | Назначение |
|------|------------|
| `src/plugin-main.cpp` | Загрузка модуля OBS |
| `src/hr-ble-source.cpp` | Source, Properties, рендер |
| `src/ble/` | BLE Windows / macOS |
| `installer/` | Inno Setup |
| `data/locale/` | en-US / ru-RU |

Основано на [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

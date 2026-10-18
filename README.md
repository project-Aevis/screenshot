# Aevis Screenshot

Interactive screenshot and screen capture tool for the Aevis Desktop Environment.

Part of the **[Project Aevis](https://github.com/project-Aevis)** suite.

## Features

- Full screen, active window, or freeform rectangular area capture
- Annotation tools: shapes, arrows, blur/pixelate region, text tool
- Quick copy to clipboard, save to file, or open in photo viewer
- Global hotkey shortcut integration

## Dependencies

### Debian / Ubuntu

```shell
sudo apt install cmake qt6-base-dev qt6-declarative-dev
```

### Arch Linux

```shell
sudo pacman -S extra-cmake-modules qt6-base qt6-declarative
```

## Build and Install

```shell
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
sudo make install
```

## License

This project is licensed under the GNU General Public License v3.0 (GPLv3).

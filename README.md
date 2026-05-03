# Q_overlay
WIP vulkan layer act as in-game overlay.

# Building
## cmake
```
cmake -B build -G Ninja
cmake --build build
```
## meson
```
meson setup build
meson compile -C build
```

# Using
```
VK_LAYER_PATH=./build VK_INSTANCE_LAYERS=VK_LAYER_q_overlay vkcube
```
Layer logs into stdour/stderr and /tmp/q_overlay.log
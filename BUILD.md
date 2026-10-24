# Build do Furnace (fork piano-roll): Linux e Windows (MinGW)

Base: tag `v0.6.8.3`, branch `piano-roll`.

Existem dois caminhos. O caminho A é o que esta sessão usa e é o mesmo que o autor do Furnace usa para gerar os releases oficiais de Windows (`scripts/release-win64.sh`): compilação cruzada a partir de Linux com MinGW. O caminho B é para compilar direto no Windows.

## A. Compilação cruzada em Linux (Ubuntu 24.04), gera `furnace.exe`

```
sudo apt-get install -y git cmake ninja-build mingw-w64
git clone --recursive https://github.com/tildearrow/furnace.git
cd furnace
git checkout -b piano-roll v0.6.8.3
mkdir build-win && cd build-win
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=../scripts/Cross-MinGW-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_LOCALE=ON -DUSE_MOMO=ON -DUSE_BACKWARD=OFF -DSUPPORT_VISTA=ON ..
ninja
x86_64-w64-mingw32-strip -s furnace.exe
```

Resultado: `build-win/furnace.exe`, executável único (SDL2 e demais bibliotecas vão estáticas). Roda em Windows 10/11 64 bits sem instalar nada.

## B. Direto no Windows com MSYS2 (MINGW64)

1. Instalar o MSYS2 de https://www.msys2.org/ até o passo 4 do guia deles.
2. Abrir pelo menu Iniciar o atalho "MSYS2 MINGW64" (ícone azul). Não usar UCRT64.
3. No console:

```
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-gcc git
git clone --recursive https://github.com/tildearrow/furnace.git
cd furnace
git checkout -b piano-roll v0.6.8.3
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
```

Resultado: `build/furnace.exe`.

## Build Linux usado para os screenshots

```
sudo apt-get install -y libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxinerama-dev libxss-dev libxxf86vm-dev libasound2-dev libgl-dev
mkdir build-linux && cd build-linux
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
ninja
xvfb-run -s "-screen 0 1600x900x24" ./furnace
```

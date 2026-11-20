# LiveHub Multistream

Plugin do OBS Studio usado pelo [LiveHub](https://livehub.cloud) para multistream e canvas vertical.

Fase 0: o plugin apenas registra o vendor `livehub-multistream` no obs-websocket e responde ao request `status` (versao do plugin, versao do OBS e versao da API). Requer OBS 31.1 ou mais novo.

## Licenca

GPLv2 ou posterior.

## Compilar

```powershell
$env:Path = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:Path"
cmd /c "cmake --preset windows-x64 > %TEMP%\lhms-cfg.log 2>&1"
cmd /c "cmake --build --preset windows-x64 --config RelWithDebInfo > %TEMP%\lhms-build.log 2>&1"
```

A DLL sai em `build_x64\RelWithDebInfo\LiveHub-multistream.dll`.

## Instalacao

- `C:\ProgramData\obs-studio\plugins\LiveHub-multistream\bin\64bit\` (DLL)
- `C:\ProgramData\obs-studio\plugins\LiveHub-multistream\data\` (locale etc.)

O instalador do LiveHub faz isso automaticamente.

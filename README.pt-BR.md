# M720 Mapper

[English](README.md)

Remapeador nativo para Windows x64 e Logitech M720 Triathlon pareado via Bluetooth LE, PID B015.

A Logitech fabrica o mouse M720 Triathlon. O M720 Mapper é um projeto independente, sem afiliação com a Logitech.

| Botão | Atalho |
| --- | --- |
| Avançar | Ctrl+C |
| Voltar | Ctrl+V |
| Polegar / gesto | Print Screen |

O clique central, a inclinação da roda e a rolagem mantêm o comportamento normal. A ação de Print Screen segue a configuração do Windows.

## Usar

Execute `M720Mapper.exe` para iniciar sem janela. Ao trocar de computador pelo Easy-Switch, deixe o programa em execução. Quando o Windows detectar o retorno do mouse, o Mapper lê o estado HID++ da nova conexão e rearma os três atalhos. Se o serviço GATT ainda não estiver pronto, a nova tentativa ocorre em até 2,5 s, além do tempo de resposta do Bluetooth no Windows.

```powershell
.\M720Mapper.exe --status
.\M720Mapper.exe --stop
```

`--stop` aguarda a restauração do reporting temporário na conexão atual. Ao perder a conexão, o Mapper descarta o estado anterior e evita reaplicá-lo à nova sessão. Em um encerramento abrupto, o estado temporário pode permanecer no mouse até a reconexão.

Códigos de saída de `--status`: 0 para atalhos ativos, 1 enquanto aguarda o mouse desconectado, 3 para parado, 4 para inicialização ou nova tentativa e 2 para erro. O Mapper usa o nível de privilégio do usuário. O Windows pode bloquear o envio de teclas para aplicativos elevados.

## Iniciar com o Windows

Use o instalador da versão publicada em uma sessão normal do PowerShell.

```powershell
.\scripts\Install-M720Mapper.ps1
```

O script copia o executável para `%LOCALAPPDATA%\M720Mapper` e registra a inicialização do usuário em HKCU Run. Ele pode migrar a versão anterior se o auxiliar de parada original estiver disponível. Guarda uma cópia do binário, dos auxiliares e da entrada anterior. Use `-RequireActive` para exigir que os atalhos estejam ativos antes de confirmar a instalação.

O instalador consulta `--status` antes de confirmar a entrada de inicialização. Ele aceita o código 0 ou, sem `-RequireActive`, o código 1. Os dois scripts interrompem a operação se uma instância existente falhar ao encerrar de forma graciosa. Se a inicialização falhar, o instalador tenta recuperar os arquivos e a entrada anterior a partir do backup.

```powershell
.\scripts\Uninstall-M720Mapper.ps1
```

A desinstalação encerra o Mapper, remove seu executável e a entrada de inicialização. Preserva o log e os backups de migração. O log fica em `%LOCALAPPDATA%\M720Mapper\Mapper.log`, com limite de 1 MiB por arquivo e uma cópia anterior.

## Diagnosticar e compilar

```powershell
.\M720Mapper.exe --diagnose
.\M720Mapper.exe --probe
.\M720Mapper.exe --listen 30
```

`--diagnose` lê a característica GATT. `--probe` consulta o protocolo e o reporting dos três botões. `--listen` registra as pressões já desviadas pelo mouse durante 1 a 120 s. Encerre a instância residente antes de usar `--probe` ou `--listen`. Esses comandos preservam o reporting dos botões. Eles habilitam as notificações GATT temporariamente quando necessário e restauram a configuração anterior.

Use Visual Studio Build Tools 2022, MSVC x64, Windows SDK com cabeçalhos C++/WinRT e CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

A compilação Release vincula o runtime C++ estaticamente. O executável usa as APIs de Bluetooth e entrada do Windows. Os testes cobrem o formato das mensagens, o pareamento de respostas, a ordem e repetição de cliques e o plano de restauração em ordem inversa.

Em uma amostra com os atalhos ativos, o Windows mediu 2,76 MiB de memória privada comprometida e 17,21 MiB residentes, dos quais 15,17 MiB eram páginas compartilhadas. A amostra ficou abaixo da meta de 5 MiB privados e acima da meta de 10 MiB residentes.

# Battle Royale 3D

Protótipo offline de battle royale em primeira pessoa para Android, feito em C++17 com NativeActivity e OpenGL ES 2.0. O lobby mostra o personagem no centro e oferece `PLAY`, skins, loja e configurações. `PLAY` abre Solo, Treino ou Hardcore antes da seleção de rifle, SMG ou sniper. Treino usa menos bots e não tem tempestade; Hardcore aumenta o dano recebido. Skins cosméticas podem ser compradas com moedas obtidas na partida. O visual usa geometria voxel original, postos de dois andares com interiores acessíveis, ruas marcadas, árvores cúbicas e céu com nuvens em blocos. A arena tem 400 x 400 unidades. Os bots usam equipamentos e uniformes táticos variados, avançam e flanqueiam. A partida inclui aceleração, gravidade, salto, colisões, projéteis com trajetória, recuo, loot físico e zona segura móvel. O menu inclui ajuste de sensibilidade e editor para arrastar os controles do HUD. Efeitos sonoros sintetizados cobrem combate, passos, itens, interface e eventos da partida. Não há rede ou partidas multiplayer.

## Controles

- Joystick no canto inferior esquerdo: movimentar
- Arrastar no lado direito: girar a câmera e mirar
- Ícone de mira: alternar zoom, maior precisão e assistência magnética perto de um bot
- `SETTINGS` no lobby: ajustar sensibilidade e arrastar controles em `EDIT HUD`
- Retículo circular: fica vermelho quando um bot entra na mira
- Ícone de postura: alternar entre ficar em pé e agachar
- Ícone de seta para cima: saltar
- `PLAY`: escolher modo de partida e depois arma
- `SKINS` e `STORE`: selecionar ou comprar visuais cosméticos
- `RESTART` na tela de resultado: iniciar outra partida com a mesma arma e configurações
- Manter o ícone de disparo pressionado: atirar; a arma usa pente e reserva
- `REL`: recarregar; a recarga leva alguns instantes
- Manter `MED` pressionado: usar kit médico; soltar o botão interrompe a cura
- Aproximar-se das caixas no mapa para coletar munição ou kits médicos
- Tocar em `TAP TO DEPLOY` para iniciar a partida

## Compilar

Abra esta pasta no Android Studio com Android SDK 35, Android NDK e CMake 3.22.1 instalados; sincronize o Gradle e execute a configuração `app` em um dispositivo Android ou emulador com suporte a OpenGL ES 2.0.

Também é possível compilar pelo terminal após configurar `ANDROID_HOME` para o SDK:

```bash
gradle assembleDebug
```
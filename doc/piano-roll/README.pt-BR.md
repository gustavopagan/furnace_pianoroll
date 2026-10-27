# Piano roll para o Furnace

[English](README.md) | Português

Fork do [Furnace](https://github.com/tildearrow/furnace) v0.6.8.3 com uma janela de piano roll por canal. O roll lê e escreve direto nas patterns do tracker. Não existe formato novo de música: tudo o que você faz no roll vira notas, instrumentos, volumes e efeitos comuns, que o Furnace original abre normalmente.

![demonstração](piano-roll-demo.gif)

## Download

Página [Releases](https://github.com/gustavopagan/furnace_pianoroll/releases):

| Sistema | Arquivo |
|---|---|
| Windows 64 bits | `...-Windows-x86_64.zip`: descompacte e rode `furnace.exe` |
| macOS Apple Silicon (M1 em diante) | `...-macOS-arm64.dmg` |
| macOS Intel | `...-macOS-x86_64.dmg` |
| Linux x86_64 | `...-Linux-x86_64.tar.gz`: descompacte e rode `./furnace` |

Os executáveis não são assinados. No Windows, clique em "Mais informações" > "Executar assim mesmo" (ou botão direito no zip > Propriedades > Desbloquear antes de descompactar). No macOS, na primeira vez abra com botão direito > Abrir.

## Sumário

- [Abrir a janela](#abrir-a-janela)
- [A janela, de cima para baixo](#a-janela-de-cima-para-baixo)
- [Criar, mover e apagar notas](#criar-mover-e-apagar-notas)
- [Selecionar várias notas](#selecionar-várias-notas)
- [Copiar e colar](#copiar-e-colar)
- [Trocar o instrumento](#trocar-o-instrumento)
- [Pattern repetida](#pattern-repetida)
- [Painel de nota e efeitos](#painel-de-nota-e-efeitos)
- [Portamento](#portamento)
- [Faixas de volume e de efeito](#faixas-de-volume-e-de-efeito)
- [Snap e precisão dentro da linha](#snap-e-precisão-dentro-da-linha)
- [Nomes de linha e Fold](#nomes-de-linha-e-fold)
- [Sincronia com o tracker](#sincronia-com-o-tracker)
- [Transporte estilo DAW](#transporte-estilo-daw)
- [Atalhos](#atalhos)
- [Arquivos, idioma e limites](#arquivos-idioma-e-limites)

## Abrir a janela

Menu `window` > `piano roll`, ou **Ctrl+Shift+R** (dá para trocar em Settings > Keyboard).

![menu](img/02-menu.png)

## A janela, de cima para baixo

![visão geral](img/01-overview.png)

- **Barra, linha 1:** canal, ferramentas (Select **S**, Draw **D**, Erase **E**), Snap, fim das notas desenhadas (OFF ou REL), Fold, Ghost notes, Follow playback.
- **Barra, linha 2:** zoom horizontal e vertical, o que fazer com pattern repetida, Lanes, Clear transport, e dicas ou avisos.
- **Painel de nota:** início, fim, duração, altura, instrumento e efeitos da nota selecionada (ou de várias).
- **Barra de arranjo:** um bloco por posição da order. Patterns repetidas aparecem em âmbar com "xN". Clicar num bloco rola até ele.
- **Régua:** linhas e compassos. Clicar põe o marcador de play.
- **Roll:** teclado à esquerda e as notas do canal. Os outros canais do mesmo chip aparecem em cinza (Ghost notes).
- **Faixas (Lanes):** volume e um efeito à escolha, linha a linha.

## Criar, mover e apagar notas

| Ação | Como |
|---|---|
| Criar | ferramenta Draw: clique (usa a última duração) ou clique e arraste |
| Mover | arraste o corpo da nota |
| Mudar a duração | arraste a borda direita |
| Duplicar | Shift + arrastar (a cópia vai para o lado da original, nunca por cima) |
| Apagar | Delete, ou ferramenta Erase (clique ou arraste por cima) |
| Copiar, recortar, colar | Ctrl+C / Ctrl+X, depois clique na régua onde as notas vão e aperte Ctrl+V (vale entre canais) |
| Desfazer / refazer | Ctrl+Z / Ctrl+Y (o mesmo histórico do tracker) |

![desenhar](img/03-draw.png)

![mover](img/04-move.png)

Cada gesto vira um único Ctrl+Z. Um canal do tracker é monofônico: uma nota começa onde a anterior acaba. Por isso o roll nunca apaga uma nota que não faz parte da edição; quem encurta é a nota editada.

Mover ou duplicar uma nota leva junto os efeitos da linha onde ela começa, o volume dessa linha e o corte `ECxx` dentro dela. Efeitos que valem para a música inteira (velocidade, tempo, pulos de order, fim de música) ficam no lugar.

## Selecionar várias notas

Com Select, arraste um retângulo em área vazia. Ctrl+clique soma ou tira uma nota. Ctrl+A seleciona tudo, Esc limpa. Arrastar uma das selecionadas move todas juntas.

![retângulo de seleção](img/05-select.png)

![várias selecionadas](img/06-multi.png)

## Copiar e colar

Selecione notas e aperte Ctrl+C (ou Ctrl+X para recortar). Clique na régua onde a primeira nota deve ficar e aperte Ctrl+V. Sem marcador, as notas vão para o cursor do tracker. Dá para trocar o canal do roll antes de colar. As notas coladas mantêm duração, altura, instrumento, volume e efeitos da linha inicial (efeitos que valem para a música inteira ficam de fora). Notas que começam dentro do trecho colado são substituídas; uma nota que vinha tocando é cortada, como em qualquer edição no tracker. A colagem inteira é um único Ctrl+Z, e pattern repetida é tratada como em qualquer outra edição.

## Trocar o instrumento

Com notas selecionadas, clique num instrumento da lista Instruments: ele é aplicado a todas. O seletor Instrument do painel faz o mesmo.

![instrumento](img/07-instrument.png)

## Pattern repetida

Se a edição cai numa pattern usada em mais de uma posição da order, o roll pergunta: editar todas, tornar única naquela posição ou cancelar. A resposta vale até fechar o Furnace e pode ser trocada no seletor "Repeated" da barra.

![pattern repetida](img/08-repeated.png)

## Painel de nota e efeitos

Mostra a nota selecionada, ou as várias selecionadas ("..." quando os valores diferem). Início e fim usam ordem:linha em hexadecimal, como o tracker. Duração, transposição (-12, -1, +1, +12) e instrumento valem para todas as selecionadas.

![painel](img/09-panel.png)

**Efeitos** aparecem agrupados pelo código:

- editar o valor muda as linhas que já têm o efeito;
- **+N** acrescenta nas N notas que não têm;
- **x** remove;
- **Add** escreve um efeito novo em todas: digite o código, Enter, o valor, Enter (ou clique em Apply).

Duplo clique numa nota leva direto ao campo Add. Se uma linha não tiver coluna de efeito livre, ela é pulada e a barra avisa. O roll não cria colunas de efeito sozinho; para isso, use o `++` no canto do canal no tracker.

## Portamento

Uma nota com `03xx` ganha uma linha amarela que sai da nota anterior, faz a rampa até a altura nova e segue pela nota. O comprimento da rampa é o tempo real do deslize, calculado como o Furnace faz em linear pitch (o padrão).

![portamento](img/10-portamento.png)

Arraste a bolinha no fim da rampa para a esquerda ou direita: o valor do `03` muda junto (rampa mais curta, deslize mais rápido).

![âncora do portamento](img/11-anchor.png)

Um círculo vazado avisa que a nota tem `03` mas não há nada soando antes, então ela não vai deslizar.

![sem deslize](img/12-noslide.png)

## Faixas de volume e de efeito

Ligue com o botão **Lanes**.

- **Volume:** degraus com o volume em vigor (ele vale até a próxima escrita). Arraste para desenhar; botão direito ou Erase apaga.

![faixa de volume](img/13-volume.png)

- **Efeito:** o seletor à esquerda escolhe o efeito; `*` quer dizer "o mais usado do canal". A altura da barra é o valor (0 a FF). Com Draw, arrastar escreve o efeito; botão direito ou Erase remove. Clicar numa linha leva a linha para o painel, mesmo sem nota ali. É assim que se editam efeitos soltos, como um `EC04` no meio de uma nota.

![efeito solto](img/14-loose-effect.png)

## Snap e precisão dentro da linha

O seletor **Snap** define a grade:

- **row:** notas presas às linhas;
- **1/2** e **tick:** início e fim dentro da linha, escritos como `EDxx` (atraso) e `ECxx` (corte).

![snap tick](img/15-snap-tick.png)

Arrastando em modo tick, a nota começa e termina no meio da linha:

![arrastando em tick](img/15-snap-tick-drag.png)

No tracker, o resultado são efeitos comuns:

![resultado no tracker](img/15-snap-tick-tracker.png)

Se não houver coluna de efeito livre para o `ED` ou o `EC`, a edição inteira é recusada e nada muda.

## Nomes de linha e Fold

Botão direito numa tecla do teclado do roll dá um nome àquela altura no canal ("kick", "snare", nome do sample). O nome aparece na tecla e dentro das notas.

![nomear linha](img/16-rowname-popup.png)

![linhas nomeadas](img/16-rownames.png)

**Fold** esconde as alturas sem nota nem nome, útil para bateria:

![fold](img/17-fold.png)

## Sincronia com o tracker

Clicar numa nota leva o cursor do tracker até ela. Mover o cursor do tracker leva o roll para aquele canal e linha; o contorno no roll mostra onde o cursor está. Durante a reprodução, o roll não mexe no cursor do tracker. **Follow playback** faz o roll acompanhar a música tocando.

## Transporte estilo DAW

Ligue o botão **piano roll** ao lado de Poly, nos controles de play:

![botão piano roll](img/18-transport-toggle.png)

Com ele ligado:

- **Espaço** toca e para em qualquer janela (por cima do atalho normal do Furnace), exceto enquanto você digita num campo;
- clique na régua para pôr o **marcador**: o play começa dali, sempre, até você clicar em **Clear transport**. Clicar na régua com a música tocando pula para lá;
- **I** marca o início do loop no marcador e **O** marca o fim. Com **Repeat pattern** ligado (o botão de setas circulares), a música toca em loop dentro da seleção e corta as notas na volta;
- **Clear transport** limpa o marcador e o loop. F5 continua tocando do começo da música.

![transporte com loop](img/18-transport.png)

A faixa clara na régua é o loop; o triângulo é o marcador; a linha branca é a posição da música. A barra mostra "start" e "loop" em ordem:linha.

Com o botão desligado, o Furnace se comporta como o original.

## Atalhos

| Tecla | Função |
|---|---|
| Ctrl+Shift+R | abrir o piano roll |
| S / D / E | Select / Draw / Erase |
| Delete | apagar as selecionadas |
| Ctrl+C / Ctrl+X / Ctrl+V | copiar / recortar / colar no marcador |
| Ctrl+A / Esc | selecionar tudo / limpar a seleção |
| Shift + arrastar | duplicar |
| Ctrl + clique | somar ou tirar da seleção |
| Ctrl+Z / Ctrl+Y | desfazer / refazer |
| Espaço | play/stop (com o botão piano roll ligado) |
| I / O | início / fim do loop no marcador |
| botão direito no teclado | nomear a linha |

## Arquivos, idioma e limites

**Arquivos.** As preferências por canal (fim OFF/REL, Fold, nomes de linha) ficam ao lado da música, em `<música>.fur.roll.json`. O `.fur` não muda de formato. Zoom, canal, ferramenta, Snap, Ghost, Follow e Lanes ficam na configuração do Furnace.

**Idioma.** Os textos da janela estão traduzidos para português do Brasil. Escolha o idioma em Settings > General > Language e reinicie o Furnace. A pasta `locale` precisa estar ao lado do executável.

**Limites conhecidos.**

- Edita um canal por vez.
- Marcador e loop funcionam por linha, não por tick, e valem até fechar o Furnace.
- Em músicas que não usam linear pitch, a rampa do portamento é aproximada (desenhada tracejada).
- Os executáveis não são assinados.

## Código-fonte e licença

GPL-2.0 ou posterior, como o Furnace. O código está neste repositório, na branch `piano-roll`; as mudanças sobre a v0.6.8.3 podem ser vistas comparando com a tag `v0.6.8.3`. Instruções de compilação em [BUILD.md](BUILD.md).
